/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: core/ast.c
*/

#include <stdlib.h>
#include "ast.h"
#include "memory.h"

typedef struct ASTRegistryEntry {
    ASTNode *node;
    struct ASTRegistryEntry *next;
} ASTRegistryEntry;

static ASTRegistryEntry *ast_registry = NULL;

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
    ASTRegistryEntry *entry = infernal_malloc(sizeof(*entry));
    if (!entry) return;
    entry->node = node;
    entry->next = ast_registry;
    ast_registry = entry;
}

static void ast_registry_remove(ASTNode *node) {
    ASTRegistryEntry **link = &ast_registry;
    while (*link) {
        if ((*link)->node == node) {
            ASTRegistryEntry *entry = *link;
            *link = entry->next;
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

    free(spec->body_tokens);
    spec->body_tokens = NULL;
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
    for (ASTRegistryEntry *entry = ast_registry; entry; entry = entry->next) {
        if (entry->node == node) return true;
    }
    return false;
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

void ast_free_all(void) {
    while (ast_registry) {
        ASTNode *node = ast_registry->node;
        ast_free_internal(node);
    }
    /* Libera NodeList temporales que nunca llegaron a ser propiedad de un AST. */
    nodelist_alloc_free_orphans();
}
