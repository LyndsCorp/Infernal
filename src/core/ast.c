/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: core/ast.c
*/

#include <stdlib.h>
#include <stdint.h>
#include "ast.h"
#include "memory.h"
#include "runtime/error.h"

typedef struct ASTRegistryEntry {
    ASTNode *node;
    struct ASTRegistryEntry *next;
} ASTRegistryEntry;

static ASTRegistryEntry *ast_registry = NULL;

/*
 * Conjunto hash de punteros AST registrados. No se puede usar un campo
 * dentro de ASTNode como marca de pertenencia porque ast_free_all() puede
 * visitar un mismo nodo a través de un padre después de que ese nodo ya
 * haya sido liberado. Consultar una marca dentro de memoria liberada sería
 * un use-after-free.
 *
 * La tabla mantiene la comprobación O(1) media que buscamos sin tocar el
 * objeto para saber si sigue vivo.
 */
static ASTNode **ast_ptr_set = NULL;
static size_t ast_ptr_cap = 0;
static size_t ast_ptr_count = 0;

static size_t ast_ptr_hash(const ASTNode *node) {
    uintptr_t x = (uintptr_t)node;
#if UINTPTR_MAX > 0xffffffffU
    x ^= x >> 33;
    x *= (uintptr_t)0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= (uintptr_t)0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
#else
    x ^= x >> 16;
    x *= (uintptr_t)0x7feb352dU;
    x ^= x >> 15;
    x *= (uintptr_t)0x846ca68bU;
    x ^= x >> 16;
#endif
    return (size_t)x;
}

static void ast_ptr_set_rehash(size_t new_cap) {
    ASTNode **new_set = infernal_calloc(new_cap, sizeof(*new_set));

    if (ast_ptr_set) {
        for (size_t i = 0; i < ast_ptr_cap; i++) {
            ASTNode *node = ast_ptr_set[i];
            if (!node) continue;
            size_t pos = ast_ptr_hash(node) & (new_cap - 1);
            while (new_set[pos]) pos = (pos + 1) & (new_cap - 1);
            new_set[pos] = node;
        }
        free(ast_ptr_set);
    }

    ast_ptr_set = new_set;
    ast_ptr_cap = new_cap;
}

static void ast_ptr_set_add(ASTNode *node) {
    if (!node) return;
    if (ast_ptr_cap == 0) ast_ptr_set_rehash(64);
    if ((ast_ptr_count + 1) * 10 >= ast_ptr_cap * 7)
        ast_ptr_set_rehash(ast_ptr_cap * 2);

    size_t pos = ast_ptr_hash(node) & (ast_ptr_cap - 1);
    while (ast_ptr_set[pos]) {
        if (ast_ptr_set[pos] == node) return;
        pos = (pos + 1) & (ast_ptr_cap - 1);
    }
    ast_ptr_set[pos] = node;
    ast_ptr_count++;
}

static bool ast_ptr_set_contains(const ASTNode *node) {
    if (!node || !ast_ptr_set || ast_ptr_cap == 0) return false;
    size_t pos = ast_ptr_hash(node) & (ast_ptr_cap - 1);
    while (ast_ptr_set[pos]) {
        if (ast_ptr_set[pos] == node) return true;
        pos = (pos + 1) & (ast_ptr_cap - 1);
    }
    return false;
}

static void ast_ptr_set_remove(const ASTNode *node) {
    if (!node || !ast_ptr_set || ast_ptr_cap == 0) return;
    size_t pos = ast_ptr_hash(node) & (ast_ptr_cap - 1);
    while (ast_ptr_set[pos]) {
        if (ast_ptr_set[pos] != node) {
            pos = (pos + 1) & (ast_ptr_cap - 1);
            continue;
        }

        ast_ptr_set[pos] = NULL;
        ast_ptr_count--;

        /* Reinsertar el cluster posterior para conservar el probing. */
        size_t next = (pos + 1) & (ast_ptr_cap - 1);
        while (ast_ptr_set[next]) {
            ASTNode *moved = ast_ptr_set[next];
            ast_ptr_set[next] = NULL;
            ast_ptr_count--;

            size_t dst = ast_ptr_hash(moved) & (ast_ptr_cap - 1);
            while (ast_ptr_set[dst]) dst = (dst + 1) & (ast_ptr_cap - 1);
            ast_ptr_set[dst] = moved;
            ast_ptr_count++;
            next = (next + 1) & (ast_ptr_cap - 1);
        }
        return;
    }
}

static void ast_ptr_set_clear(void) {
    free(ast_ptr_set);
    ast_ptr_set = NULL;
    ast_ptr_cap = 0;
    ast_ptr_count = 0;
}

typedef struct NodeListAllocEntry {
    ASTNode **ptr;
    struct NodeListAllocEntry *next;
} NodeListAllocEntry;

static NodeListAllocEntry *nodelist_alloc_registry = NULL;

static void nodelist_alloc_add(ASTNode **ptr) {
    if (!ptr) return;
    /* Evitar entradas duplicadas que provocan dobles free */
    for (NodeListAllocEntry *entry = nodelist_alloc_registry; entry; entry = entry->next) {
        if (entry->ptr == ptr) return;
    }
    NodeListAllocEntry *entry = infernal_malloc(sizeof(*entry));
    if (!entry) return;
    entry->ptr = ptr;
    entry->next = nodelist_alloc_registry;
    nodelist_alloc_registry = entry;
}

static void nodelist_alloc_update(ASTNode **old_ptr, ASTNode **new_ptr) {
    if (old_ptr == new_ptr) return;
    for (NodeListAllocEntry *entry = nodelist_alloc_registry; entry; entry = entry->next) {
        if (entry->ptr == old_ptr) {
            entry->ptr = new_ptr;
            /* Si el nuevo puntero ya estaba registrado en otra entrada,
             * eliminamos la entrada antigua para no dejar duplicados. */
            for (NodeListAllocEntry *other = nodelist_alloc_registry; other; other = other->next) {
                if (other != entry && other->ptr == new_ptr) {
                    /* Quitar esta entrada duplicada de la lista */
                    NodeListAllocEntry **link = &nodelist_alloc_registry;
                    while (*link) {
                        if (*link == other) {
                            *link = other->next;
                            free(other);
                            return;
                        }
                        link = &(*link)->next;
                    }
                }
            }
            return;
        }
    }
    /* El puntero antiguo no estaba registrado: registrar el nuevo. */
    nodelist_alloc_add(new_ptr);
}

static void nodelist_alloc_remove(ASTNode **ptr) {
    if (!ptr) return;
    NodeListAllocEntry **link = &nodelist_alloc_registry;
    while (*link) {
        if ((*link)->ptr == ptr) {
            NodeListAllocEntry *entry = *link;
            *link = entry->next;
            free(entry);
            /* No avanzar: puede haber más entradas con el mismo ptr */
        } else {
            link = &(*link)->next;
        }
    }
}

static void nodelist_alloc_free_orphans(void) {
    while (nodelist_alloc_registry) {
        NodeListAllocEntry *entry = nodelist_alloc_registry;
        nodelist_alloc_registry = entry->next;
        if (entry->ptr) {
            free(entry->ptr);
            entry->ptr = NULL;
        }
        free(entry);
    }
}

static void ast_registry_add(ASTNode *node) {
    if (!node) return;
    ASTRegistryEntry *entry = infernal_malloc(sizeof(*entry));
    entry->node = node;
    ast_ptr_set_add(node);
    entry->next = ast_registry;
    ast_registry = entry;
}

static void ast_registry_remove(ASTNode *node) {
    ASTRegistryEntry **link = &ast_registry;
    while (*link) {
        if ((*link)->node == node) {
            ASTRegistryEntry *entry = *link;
            *link = entry->next;
            ast_ptr_set_remove(node);
            free(entry);
            return;
        }
        link = &(*link)->next;
    }
}

ASTNode *node_create(int kind, int line) {
    ASTNode *n = infernal_calloc(1, sizeof(ASTNode));
    n->kind = kind;
    n->line = line;
    ast_registry_add(n);
    return n;
}

void nodelist_add(NodeList *list, ASTNode *node) {
    if (list->count >= list->cap) {
        list->cap = list->cap == 0 ? 8 : list->cap * 2;
        ASTNode **old_stmts = list->stmts;
        list->stmts = infernal_realloc(list->stmts, list->cap * sizeof(ASTNode*));
        nodelist_alloc_update(old_stmts, list->stmts);
    }
    list->stmts[list->count++] = node;
}


static void free_flag_spec(FlagSpec *spec) {
    if (!spec) return;
    for (int i = 0; i < spec->name_count; i++) {
        free(spec->names[i]);
        spec->names[i] = NULL;
    }
    free(spec->names);
    spec->names = NULL;
    spec->name_count = 0;

    free(spec->var_name);
    spec->var_name = NULL;

    if (spec->body_tokens) {
        for (int i = 0; i < spec->body_count; i++) {
            free(spec->body_tokens[i].lexeme);
            spec->body_tokens[i].lexeme = NULL;
        }
        free(spec->body_tokens);
        spec->body_tokens = NULL;
    }
    spec->body_count = 0;
}

static void ast_free_internal(ASTNode *node);
static void nodelist_free_internal(NodeList *list) {
    if (!list) return;

    if (!list->stmts) {
        list->count = 0;
        list->cap = 0;
        return;
    }

    ASTNode **stmts = list->stmts;

    // Liberar cada nodo del array
    for (int i = 0; i < list->count; i++) {
        if (list->stmts[i] != NULL) {
            ast_free_internal(list->stmts[i]);
            list->stmts[i] = NULL;  // evitar doble liberación
        }
    }
    nodelist_alloc_remove(stmts);
    free(stmts);
    list->stmts = NULL;
    list->count = 0;
    list->cap = 0;
}

static bool ast_is_registered(const ASTNode *node) {
    return ast_ptr_set_contains(node);
}

static void ast_free_internal(ASTNode *node) {
    if (!node || !ast_is_registered(node)) return;
    ast_registry_remove(node);
    switch (node->kind) {
        case NODE_PROGRAM: nodelist_free_internal(&node->data.prog.stmts); break;
        case NODE_EXPR_STMT: ast_free_internal(node->data.expr_stmt.expr); break;
        case NODE_CMD_STMT: free(node->data.cmd_stmt.cmd); break;
        case NODE_SHELL_CMD: free(node->data.shell_cmd.cmd); break;
        case NODE_ASSIGN:
            free(node->data.assign.name); free(node->data.assign.cmd_str);
            ast_free_internal(node->data.assign.value); ast_free_internal(node->data.assign.lhs_index);
            break;
        case NODE_IF: ast_free_internal(node->data.if_stmt.cond); nodelist_free_internal(&node->data.if_stmt.then_block); nodelist_free_internal(&node->data.if_stmt.else_block); break;
        case NODE_WHILE: ast_free_internal(node->data.while_stmt.cond); nodelist_free_internal(&node->data.while_stmt.body); break;
        case NODE_SWITCH:
            ast_free_internal(node->data.switch_stmt.expr);
            for (int i = 0; i < node->data.switch_stmt.case_count; i++) {
                ast_free_internal(node->data.switch_stmt.cases[i].value);
                nodelist_free_internal(&node->data.switch_stmt.cases[i].body);
            }
            free(node->data.switch_stmt.cases);
            nodelist_free_internal(&node->data.switch_stmt.default_block);
            break;
        case NODE_FOR: free(node->data.for_stmt.var); ast_free_internal(node->data.for_stmt.init); ast_free_internal(node->data.for_stmt.cond); ast_free_internal(node->data.for_stmt.incr); nodelist_free_internal(&node->data.for_stmt.body); break;
        case NODE_FUNC_DEF:
            free(node->data.func.name);
            for (int i = 0; i < node->data.func.param_count; i++) free(node->data.func.params[i]);
            free(node->data.func.params); free(node->data.func.ptypes);
        nodelist_free_internal(&node->data.func.body);
        break;
        case NODE_RETURN: ast_free_internal(node->data.ret.expr); break;
        case NODE_IMPORT: free(node->data.import.path); free(node->data.import.alias); nodelist_free_internal(&node->data.import.module_block); break;
        case NODE_TRY: nodelist_free_internal(&node->data.try_stmt.try_block); nodelist_free_internal(&node->data.try_stmt.catch_block); break;
        case NODE_VAR: free(node->data.var.name); break;
        case NODE_LITERAL: free(node->data.lit.sval); break;
        case NODE_BINOP: ast_free_internal(node->data.binop.left); ast_free_internal(node->data.binop.right); break;
        case NODE_CALL:
            free(node->data.call.name);
            for (int i = 0; i < node->data.call.argc; i++) ast_free_internal(node->data.call.args[i]);
            free(node->data.call.args);
        break;
        case NODE_INDEX: ast_free_internal(node->data.idx.list); ast_free_internal(node->data.idx.index); break;
        case NODE_FLAGS:
            for (int i = 0; i < node->data.flags.spec_count; i++) free_flag_spec(&node->data.flags.specs[i]);
            free(node->data.flags.specs);
        break;
        case NODE_LIST:
            for (int i = 0; i < node->data.list_lit.count; i++) ast_free_internal(node->data.list_lit.items[i]);
            free(node->data.list_lit.items);
        break;
        case NODE_FOR_IN:
            free(node->data.for_in.var);
            free(node->data.for_in.index_var);
            ast_free_internal(node->data.for_in.list_expr);
            nodelist_free_internal(&node->data.for_in.body);
            break;
        case NODE_PORTAL: free(node->data.portal.name); break;
        case NODE_REPEAT: ast_free_internal(node->data.repeat.line_expr); free(node->data.repeat.portal_name); break;
        case NODE_SLICE: ast_free_internal(node->data.slice.list); break;
        case NODE_EXECUTE:
            ast_free_internal(node->data.execute.path_expr);
            for (int i = 0; i < node->data.execute.argc; i++) free(node->data.execute.args[i]);
            free(node->data.execute.args);
        break;
        case NODE_MAP:
            for (int i = 0; i < node->data.map.pair_count; i++) {
                free(node->data.map.pairs[i].key);
                ast_free_internal(node->data.map.pairs[i].value);
            }
            free(node->data.map.pairs);
            break;
        case NODE_UNARY: ast_free_internal(node->data.unary.operand); break;
        case NODE_POST_INC: case NODE_POST_DEC: ast_free_internal(node->data.post_op.var); break;
        case NODE_DEFINE:
            free(node->data.define.name);
            ast_free_internal(node->data.define.value);
            break;
        case NODE_EMBEDDED:
            nodelist_free_internal(&node->data.embedded.body);
            break;
        case NODE_EMPTY_AMBIGUOUS: break;
        case NODE_BREAK: case NODE_CONTINUE: break;
    }
    free(node);
}

void nodelist_free(NodeList *list) {
    if (!list) return;
    nodelist_free_internal(list);
}

void ast_free(ASTNode *node) {
    ast_free_internal(node);
}

ASTNode *deep_copy_ast(ASTNode *node) {
    if (!node) return NULL;
    ASTNode *copy = node_create(node->kind, node->line);
    switch (node->kind) {
        case NODE_VAR:
            copy->data.var.name = node->data.var.name
                ? infernal_strdup(node->data.var.name) : NULL;
            copy->data.var.clone = node->data.var.clone;
            break;
        case NODE_LITERAL:
            copy->data.lit.type = node->data.lit.type;
            copy->data.lit.ival = node->data.lit.ival;
            copy->data.lit.fval = node->data.lit.fval;
            copy->data.lit.bval = node->data.lit.bval;
            copy->data.lit.sval = node->data.lit.sval
                ? infernal_strdup(node->data.lit.sval) : NULL;
            break;
        case NODE_INDEX:
            copy->data.idx.list = deep_copy_ast(node->data.idx.list);
            copy->data.idx.index = deep_copy_ast(node->data.idx.index);
            break;
        case NODE_BINOP:
            copy->data.binop.op = node->data.binop.op;
            copy->data.binop.left = deep_copy_ast(node->data.binop.left);
            copy->data.binop.right = deep_copy_ast(node->data.binop.right);
            break;
        case NODE_UNARY:
            copy->data.unary.op = node->data.unary.op;
            copy->data.unary.operand = deep_copy_ast(node->data.unary.operand);
            break;
        case NODE_CALL:
            copy->data.call.name = node->data.call.name
                ? infernal_strdup(node->data.call.name) : NULL;
            copy->data.call.argc = node->data.call.argc;
            if (node->data.call.argc > 0) {
                copy->data.call.args = infernal_malloc(
                    (size_t)node->data.call.argc * sizeof(ASTNode *));
                for (int i = 0; i < node->data.call.argc; i++)
                    copy->data.call.args[i] = deep_copy_ast(node->data.call.args[i]);
            } else {
                copy->data.call.args = NULL;
            }
            break;
        case NODE_LIST:
            copy->data.list_lit.count = node->data.list_lit.count;
            if (node->data.list_lit.count > 0) {
                copy->data.list_lit.items = infernal_malloc(
                    (size_t)node->data.list_lit.count * sizeof(ASTNode *));
                for (int i = 0; i < node->data.list_lit.count; i++)
                    copy->data.list_lit.items[i] =
                        deep_copy_ast(node->data.list_lit.items[i]);
            } else {
                copy->data.list_lit.items = NULL;
            }
            break;
        case NODE_SLICE:
            copy->data.slice.mode = node->data.slice.mode;
            copy->data.slice.start = node->data.slice.start;
            copy->data.slice.end = node->data.slice.end;
            copy->data.slice.list = deep_copy_ast(node->data.slice.list);
            break;
        case NODE_MAP:
            copy->data.map.pair_count = node->data.map.pair_count;
            if (node->data.map.pair_count > 0) {
                copy->data.map.pairs = infernal_malloc(
                    (size_t)node->data.map.pair_count *
                    sizeof(*node->data.map.pairs));
                for (int i = 0; i < node->data.map.pair_count; i++) {
                    copy->data.map.pairs[i].key = node->data.map.pairs[i].key
                        ? infernal_strdup(node->data.map.pairs[i].key) : NULL;
                    copy->data.map.pairs[i].value_type =
                        node->data.map.pairs[i].value_type;
                    copy->data.map.pairs[i].value =
                        deep_copy_ast(node->data.map.pairs[i].value);
                }
            } else {
                copy->data.map.pairs = NULL;
            }
            break;
        case NODE_POST_INC:
        case NODE_POST_DEC:
            copy->data.post_op.var = deep_copy_ast(node->data.post_op.var);
            copy->data.post_op.statement_context =
                node->data.post_op.statement_context;
            break;
        case NODE_EMPTY_AMBIGUOUS:
            break;
        default:
            error(node->line,
                  "deep_copy_ast: tipo de nodo no soportado (%d)",
                  node->kind);
    }
    return copy;
}

void ast_free_all(void) {
    while (ast_registry) {
        ASTNode *node = ast_registry->node;
        ast_free_internal(node);
    }
    /* Libera NodeList temporales que nunca llegaron a ser propiedad de un AST. */
    nodelist_alloc_free_orphans();
    ast_ptr_set_clear();
}
