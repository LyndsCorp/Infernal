/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: vm/compiler.c
*/

#include <stdlib.h>
#include <string.h>
#include "compiler.h"
#include "core/ast.h"
#include "core/value.h"
#include "lexer/lexer.h"
#include "runtime/error.h"
#include "runtime/globals.h"
#include "vm/vm.h"

#define MAX_LOCALS 256

/* ---- Estructura para portales durante la compilación ---- */
typedef struct CompilePortalEntry {
    char *name;
    int offset;        // índice de instrucción donde comienza el portal (-1 si aún no compilado)
} CompilePortalEntry;

typedef struct {
    Chunk *chunk;
    char *local_names[MAX_LOCALS];
    int local_types[MAX_LOCALS];
    int local_count;
    bool in_function;
    bool top_level;
    CompilePortalEntry *portals;   // tabla de portales recolectados
    int portal_count;
} Compiler;

static int add_constant(Compiler *c, Value v) {
    Chunk *ch = c->chunk;
    if (ch->const_count >= ch->const_cap) {
        ch->const_cap = ch->const_cap == 0 ? 8 : ch->const_cap * 2;
        ch->constants = realloc(ch->constants, ch->const_cap * sizeof(Value));
    }
    ch->constants[ch->const_count] = v;
    return ch->const_count++;
}

static int add_local(Compiler *c, const char *name) {
    if (c->local_count >= MAX_LOCALS) error(0, "Demasiadas variables locales");
    c->local_names[c->local_count] = strdup(name);
    c->local_types[c->local_count] = 0;
    return c->local_count++;
}

static int resolve_local(Compiler *c, const char *name) {
    for (int i = c->local_count - 1; i >= 0; i--) {
        if (strcmp(c->local_names[i], name) == 0) return i;
    }
    return -1;
}

static void emit(Chunk *ch, OpCode op, int operand) {
    if (ch->code_count >= ch->code_cap) {
        ch->code_cap = ch->code_cap == 0 ? 256 : ch->code_cap * 2;
        ch->code = realloc(ch->code, ch->code_cap * sizeof(Instruction));
    }
    ch->code[ch->code_count].op = op;
    ch->code[ch->code_count].operand = operand;
    ch->code[ch->code_count].operand2 = 0;
    ch->code_count++;
}

static int emit_jump(Chunk *ch, OpCode op) {
    emit(ch, op, 0);
    return ch->code_count - 1;
}

static void patch_jump(Chunk *ch, int offset, int target) {
    ch->code[offset].operand = target - offset;
}

/* ---- Recolección de portales en el AST ---- */
static void collect_portals_rec(ASTNode *node, Compiler *c) {
    if (!node) return;
    if (node->kind == NODE_PORTAL) {
        c->portals = realloc(c->portals, (c->portal_count + 1) * sizeof(CompilePortalEntry));
        c->portals[c->portal_count].name = strdup(node->data.portal.name);
        c->portals[c->portal_count].offset = -1;
        c->portal_count++;
        return;
    }
    switch (node->kind) {
        case NODE_PROGRAM:
            for (int i = 0; i < node->data.prog.stmts.count; i++)
                collect_portals_rec(node->data.prog.stmts.stmts[i], c);
        break;
        case NODE_EXPR_STMT:
            collect_portals_rec(node->data.expr_stmt.expr, c);
            break;
        case NODE_CMD_STMT:
        case NODE_SHELL_CMD:
        case NODE_RETURN:
        case NODE_BREAK:
        case NODE_CONTINUE:
        case NODE_REPEAT:
            break;
        case NODE_ASSIGN:
            collect_portals_rec(node->data.assign.value, c);
            if (node->data.assign.lhs_index)
                collect_portals_rec(node->data.assign.lhs_index, c);
        break;
        case NODE_IF:
            collect_portals_rec(node->data.if_stmt.cond, c);
            for (int i = 0; i < node->data.if_stmt.then_block.count; i++)
                collect_portals_rec(node->data.if_stmt.then_block.stmts[i], c);
        for (int i = 0; i < node->data.if_stmt.else_block.count; i++)
            collect_portals_rec(node->data.if_stmt.else_block.stmts[i], c);
        break;
        case NODE_WHILE:
            collect_portals_rec(node->data.while_stmt.cond, c);
            for (int i = 0; i < node->data.while_stmt.body.count; i++)
                collect_portals_rec(node->data.while_stmt.body.stmts[i], c);
        break;
        case NODE_FOR:
            collect_portals_rec(node->data.for_stmt.init, c);
            collect_portals_rec(node->data.for_stmt.cond, c);
            collect_portals_rec(node->data.for_stmt.incr, c);
            for (int i = 0; i < node->data.for_stmt.body.count; i++)
                collect_portals_rec(node->data.for_stmt.body.stmts[i], c);
        break;
        case NODE_FOR_IN:
            collect_portals_rec(node->data.for_in.list_expr, c);
            for (int i = 0; i < node->data.for_in.body.count; i++)
                collect_portals_rec(node->data.for_in.body.stmts[i], c);
        break;
        case NODE_FUNC_DEF:
            break;
        case NODE_IMPORT:
            for (int i = 0; i < node->data.import.module_block.count; i++)
                collect_portals_rec(node->data.import.module_block.stmts[i], c);
        break;
        case NODE_TRY:
            for (int i = 0; i < node->data.try_stmt.try_block.count; i++)
                collect_portals_rec(node->data.try_stmt.try_block.stmts[i], c);
        for (int i = 0; i < node->data.try_stmt.catch_block.count; i++)
            collect_portals_rec(node->data.try_stmt.catch_block.stmts[i], c);
        break;
        case NODE_FLAGS:
            break;
        case NODE_LIST:
            for (int i = 0; i < node->data.list_lit.count; i++)
                collect_portals_rec(node->data.list_lit.items[i], c);
        break;
        /* ---- MAPA: solo recorrer pares, sin emitir ---- */
        case NODE_MAP:
            for (int i = 0; i < node->data.map.pair_count; i++) {
                collect_portals_rec(node->data.map.pairs[i].key, c);
                collect_portals_rec(node->data.map.pairs[i].value, c);
            }
            break;
        case NODE_CALL:
            for (int i = 0; i < node->data.call.argc; i++)
                collect_portals_rec(node->data.call.args[i], c);
        break;
        case NODE_INDEX:
            collect_portals_rec(node->data.idx.list, c);
            collect_portals_rec(node->data.idx.index, c);
            break;
        case NODE_VAR:
        case NODE_LITERAL:
            break;
        case NODE_BINOP:
            collect_portals_rec(node->data.binop.left, c);
            collect_portals_rec(node->data.binop.right, c);
            break;
            /* ---- NUEVO: operador unario ---- */
            case NODE_UNARY:
                collect_portals_rec(node->data.unary.operand, c);
                break;
            default:
                break;
    }
}

/* ---- Compilación de expresiones ---- */
static void compile_expr(Compiler *c, ASTNode *expr);
static void compile_block(Compiler *c, NodeList *block);

static void compile_expr(Compiler *c, ASTNode *expr) {
    switch (expr->kind) {
        case NODE_LITERAL:
            switch (expr->data.lit.type) {
                case TOK_INT:    emit(c->chunk, OP_PUSH_INT, add_constant(c, val_int(expr->data.lit.ival))); break;
                case TOK_FLOAT:  emit(c->chunk, OP_PUSH_FLOAT, add_constant(c, val_float(expr->data.lit.fval))); break;
                case TOK_BOOL:   emit(c->chunk, OP_PUSH_BOOL, add_constant(c, val_bool(expr->data.lit.bval))); break;
                case TOK_STRING: emit(c->chunk, OP_PUSH_STRING, add_constant(c, val_string(expr->data.lit.sval))); break;
            }
            break;
                case NODE_VAR: {
                    const char *name = expr->data.var.name;
                    if (name[0] == '$') {
                        int const_idx = add_constant(c, val_ptr(expr));
                        emit(c->chunk, OP_INTERPRET_NODE, const_idx);
                        c->chunk->code[c->chunk->code_count - 1].operand2 = 1;
                        break;
                    }
                    const char *clean_name = (name[0] == '?') ? name + 1 : name;
                    int gidx = vm_find_global_index(clean_name);
                    if (gidx >= 0) {
                        emit(c->chunk, OP_LOAD_GLOBAL, gidx);
                        break;
                    }
                    int slot = resolve_local(c, clean_name);
                    if (slot >= 0) {
                        emit(c->chunk, OP_LOAD_VAR, slot);
                        break;
                    }
                    int const_idx = add_constant(c, val_ptr(expr));
                    emit(c->chunk, OP_INTERPRET_NODE, const_idx);
                    c->chunk->code[c->chunk->code_count - 1].operand2 = 1;
                    break;
                }
                case NODE_BINOP: {
                    if (expr->data.binop.op == TOK_PLUS &&
                        expr->data.binop.right != NULL &&
                        expr->data.binop.right->kind == NODE_INDEX) {
                        int const_idx = add_constant(c, val_ptr(expr));
                    emit(c->chunk, OP_INTERPRET_NODE, const_idx);
                    c->chunk->code[c->chunk->code_count - 1].operand2 = 1;
                    break;
                        }

                        if (expr->data.binop.op == TOK_MINUS &&
                            (expr->data.binop.right->kind == NODE_SLICE ||
                            expr->data.binop.right->kind == NODE_INDEX ||
                            (expr->data.binop.right->kind == NODE_LIST &&
                            expr->data.binop.right->data.list_lit.count == 1))) {
                            int const_idx = add_constant(c, val_ptr(expr));
                        emit(c->chunk, OP_INTERPRET_NODE, const_idx);
                        c->chunk->code[c->chunk->code_count - 1].operand2 = 1;
                        break;
                            }

                            if (expr->data.binop.op == TOK_AND) {
                                compile_expr(c, expr->data.binop.left);
                                compile_expr(c, expr->data.binop.right);
                                emit(c->chunk, OP_AND, 0);
                            } else if (expr->data.binop.op == TOK_OR) {
                                compile_expr(c, expr->data.binop.left);
                                compile_expr(c, expr->data.binop.right);
                                emit(c->chunk, OP_OR, 0);
                            } else {
                                compile_expr(c, expr->data.binop.left);
                                compile_expr(c, expr->data.binop.right);
                                switch (expr->data.binop.op) {
                                    case TOK_PLUS:  emit(c->chunk, OP_ADD, 0); break;
                                    case TOK_MINUS: emit(c->chunk, OP_SUB, 0); break;
                                    case TOK_STAR:  emit(c->chunk, OP_MUL, 0); break;
                                    case TOK_SLASH: emit(c->chunk, OP_DIV, 0); break;
                                    case TOK_PERCENT: emit(c->chunk, OP_MOD, 0); break;
                                    case TOK_EEQ:   emit(c->chunk, OP_EQ, 0); break;
                                    case TOK_NEQ:   emit(c->chunk, OP_NEQ, 0); break;
                                    case TOK_LT_OP: emit(c->chunk, OP_LT, 0); break;
                                    case TOK_GT_OP: emit(c->chunk, OP_GT, 0); break;
                                    case TOK_LE:    emit(c->chunk, OP_LE, 0); break;
                                    case TOK_GE:    emit(c->chunk, OP_GE, 0); break;
                                    default: {
                                        int const_idx = add_constant(c, val_ptr(expr));
                                        emit(c->chunk, OP_INTERPRET_NODE, const_idx);
                                        c->chunk->code[c->chunk->code_count - 1].operand2 = 1;
                                    }
                                }
                            }
                            break;
                }
                                    case NODE_CALL: {
                                        for (int i = 0; i < expr->data.call.argc; i++)
                                            compile_expr(c, expr->data.call.args[i]);
                                        int builtin_idx = vm_find_builtin_index(expr->data.call.name);
                                        if (builtin_idx >= 0) {
                                            int call_pos = c->chunk->code_count;
                                            emit(c->chunk, OP_CALL_BUILTIN, builtin_idx);
                                            c->chunk->code[call_pos].operand2 = expr->data.call.argc;
                                        } else {
                                            FuncObject *fobj = func_lookup(expr->data.call.name);
                                            if (fobj && fobj->kind == FUNC_USER && fobj->code != NULL) {
                                                int const_idx = add_constant(c, val_ptr(expr));
                                                emit(c->chunk, OP_INTERPRET_NODE, const_idx);
                                                c->chunk->code[c->chunk->code_count - 1].operand2 = 1;
                                            } else {
                                                int const_idx = add_constant(c, val_ptr(expr));
                                                emit(c->chunk, OP_INTERPRET_NODE, const_idx);
                                                c->chunk->code[c->chunk->code_count - 1].operand2 = 1;
                                            }
                                        }
                                        break;
                                    }
                                    case NODE_LIST:
                                        emit(c->chunk, OP_NEW_LIST, 0);
                                        for (int i = 0; i < expr->data.list_lit.count; i++) {
                                            compile_expr(c, expr->data.list_lit.items[i]);
                                            emit(c->chunk, OP_LIST_APPEND, 0);
                                        }
                                        break;
                                    case NODE_MAP: {
                                        /* Delegamos la construcción del mapa al evaluador */
                                        int const_idx = add_constant(c, val_ptr(expr));
                                        emit(c->chunk, OP_INTERPRET_NODE, const_idx);
                                        c->chunk->code[c->chunk->code_count - 1].operand2 = 1;
                                        break;
                                    }
                                    case NODE_INDEX: {
                                        compile_expr(c, expr->data.idx.list);
                                        compile_expr(c, expr->data.idx.index);
                                        emit(c->chunk, OP_INDEX, 0);
                                        break;
                                    }
                                    case NODE_SLICE: {
                                        if (expr->data.slice.list == NULL) {
                                            error(expr->line, "Compilador: nodo slice sin lista asignada");
                                        }
                                        int const_idx = add_constant(c, val_ptr(expr));
                                        emit(c->chunk, OP_INTERPRET_NODE, const_idx);
                                        c->chunk->code[c->chunk->code_count - 1].operand2 = 1;
                                        break;
                                    }
                                    /* ---- NUEVO: operador unario ---- */
                                    case NODE_UNARY: {
                                        if (expr->data.unary.op == TOK_NOT) {
                                            compile_expr(c, expr->data.unary.operand);
                                            emit(c->chunk, OP_NOT, 0);
                                        }
                                        break;
                                    }
                                    default: {
                                        int const_idx = add_constant(c, val_ptr(expr));
                                        emit(c->chunk, OP_INTERPRET_NODE, const_idx);
                                        c->chunk->code[c->chunk->code_count - 1].operand2 = 1;
                                        break;
                                    }
    }
}

/* ---- Compilación de sentencias ---- */
static void compile_stmt(Compiler *c, ASTNode *stmt) {
    switch (stmt->kind) {
        case NODE_EXPR_STMT:
            compile_expr(c, stmt->data.expr_stmt.expr);
            emit(c->chunk, OP_POP, 0);
            break;
        case NODE_ASSIGN: {
            const char *name = stmt->data.assign.name;
            int vtype = stmt->data.assign.vtype;

            if (stmt->data.assign.lhs_index) {
                int const_idx = add_constant(c, val_ptr(stmt));
                emit(c->chunk, OP_INTERPRET_NODE, const_idx);
                c->chunk->code[c->chunk->code_count - 1].operand2 = 0;
                break;
            }

            if (stmt->data.assign.is_global) {
                int gidx = vm_find_global_index(name);
                if (gidx < 0) {
                    gidx = vm_register_global(name, GLOBAL_SUPER, vtype);
                } else if (vtype != 0) {
                    vm_global_types[gidx] = vtype;
                }
                if (stmt->data.assign.is_cmd) {
                    int const_node = add_constant(c, val_ptr(stmt));
                    emit(c->chunk, OP_INTERPRET_NODE, const_node);
                    c->chunk->code[c->chunk->code_count - 1].operand2 = 0;
                } else {
                    compile_expr(c, stmt->data.assign.value);
                    emit(c->chunk, OP_STORE_GLOBAL, gidx);
                    c->chunk->code[c->chunk->code_count - 1].operand2 = GLOBAL_SUPER;
                }
            } else if (stmt->data.assign.is_local) {
                int slot = resolve_local(c, name);
                if (slot < 0) slot = add_local(c, name);
                if (vtype != 0) c->local_types[slot] = vtype;
                if (stmt->data.assign.is_cmd) {
                    int const_idx = add_constant(c, val_string(stmt->data.assign.cmd_str));
                    emit(c->chunk, OP_CMD_ASSIGN, const_idx);
                    c->chunk->code[c->chunk->code_count - 1].operand2 = slot;
                } else {
                    compile_expr(c, stmt->data.assign.value);
                    emit(c->chunk, OP_STORE_VAR, slot);
                }
            } else {
                if (stmt->data.assign.is_cmd) {
                    int const_node = add_constant(c, val_ptr(stmt));
                    emit(c->chunk, OP_INTERPRET_NODE, const_node);
                    c->chunk->code[c->chunk->code_count - 1].operand2 = 0;
                } else {
                    int gidx = vm_find_global_index(name);
                    if (gidx < 0) {
                        gidx = vm_register_global(name, GLOBAL_SCRIPT, vtype);
                    } else if (vtype != 0) {
                        vm_global_types[gidx] = vtype;
                    }
                    compile_expr(c, stmt->data.assign.value);
                    emit(c->chunk, OP_STORE_GLOBAL, gidx);
                    c->chunk->code[c->chunk->code_count - 1].operand2 = GLOBAL_SCRIPT;
                }
            }
            break;
        }
        case NODE_IF: {
            compile_expr(c, stmt->data.if_stmt.cond);
            int jump_false = emit_jump(c->chunk, OP_JUMP_IF_FALSE);
            compile_block(c, &stmt->data.if_stmt.then_block);
            if (stmt->data.if_stmt.else_block.count > 0) {
                int jump_end = emit_jump(c->chunk, OP_JUMP);
                patch_jump(c->chunk, jump_false, c->chunk->code_count);
                compile_block(c, &stmt->data.if_stmt.else_block);
                patch_jump(c->chunk, jump_end, c->chunk->code_count);
            } else {
                patch_jump(c->chunk, jump_false, c->chunk->code_count);
            }
            break;
        }
        case NODE_WHILE: {
            int loop_start = c->chunk->code_count;
            compile_expr(c, stmt->data.while_stmt.cond);
            int jump_exit = emit_jump(c->chunk, OP_JUMP_IF_FALSE);
            compile_block(c, &stmt->data.while_stmt.body);
            emit(c->chunk, OP_JUMP, loop_start - c->chunk->code_count);
            patch_jump(c->chunk, jump_exit, c->chunk->code_count);
            break;
        }
        case NODE_CMD_STMT:
            emit(c->chunk, OP_EMBEDDED_CMD, add_constant(c, val_string(stmt->data.cmd_stmt.cmd)));
            break;
        case NODE_SHELL_CMD:
            emit(c->chunk, OP_SHELL_CMD, add_constant(c, val_string(stmt->data.shell_cmd.cmd)));
            break;
        case NODE_RETURN:
            if (stmt->data.ret.expr) compile_expr(c, stmt->data.ret.expr);
            else emit(c->chunk, OP_PUSH_NULL, 0);
            emit(c->chunk, OP_RETURN, 0);
        break;
        case NODE_FUNC_DEF: {
            Chunk *func_chunk = compile_function(stmt);
            if (func_chunk) {
                int idx = vm_register_user_function(stmt->data.func.name, func_chunk);
                (void)idx;
            }
            break;
        }
        case NODE_PORTAL: {
            for (int i = 0; i < c->portal_count; i++) {
                if (strcmp(c->portals[i].name, stmt->data.portal.name) == 0) {
                    c->portals[i].offset = c->chunk->code_count;
                    break;
                }
            }
            break;
        }
        case NODE_REPEAT: {
            if (stmt->data.repeat.portal_name) {
                int target_offset = -1;
                for (int i = 0; i < c->portal_count; i++) {
                    if (strcmp(c->portals[i].name, stmt->data.repeat.portal_name) == 0) {
                        target_offset = c->portals[i].offset;
                        break;
                    }
                }
                if (target_offset == -1) {
                    error(stmt->line, "Portal '%s' no definido antes de usar en repeat", stmt->data.repeat.portal_name);
                }
                int current = c->chunk->code_count;
                int jump_offset = target_offset - current;
                emit(c->chunk, OP_JUMP, jump_offset);
            } else {
                int const_idx = add_constant(c, val_ptr(stmt));
                emit(c->chunk, OP_INTERPRET_NODE, const_idx);
                c->chunk->code[c->chunk->code_count - 1].operand2 = 0;
            }
            break;
        }
        default: {
            int const_idx = add_constant(c, val_ptr(stmt));
            emit(c->chunk, OP_INTERPRET_NODE, const_idx);
            c->chunk->code[c->chunk->code_count - 1].operand2 = 0;
            break;
        }
    }
}

static void compile_block(Compiler *c, NodeList *block) {
    for (int i = 0; i < block->count; i++) {
        compile_stmt(c, block->stmts[i]);
    }
}

/* ---- Compilación del programa principal ---- */
Chunk *compile_program(NodeList *program) {
    Compiler c;
    c.chunk = calloc(1, sizeof(Chunk));
    c.local_count = 0;
    c.in_function = false;
    c.top_level = true;
    c.portals = NULL;
    c.portal_count = 0;

    for (int i = 0; i < program->count; i++) {
        collect_portals_rec(program->stmts[i], &c);
    }

    compile_block(&c, program);
    emit(c.chunk, OP_RETURN, 0);

    c.chunk->local_count = c.local_count;
    if (c.local_count > 0) {
        c.chunk->local_names = malloc(c.local_count * sizeof(char*));
        c.chunk->local_types = malloc(c.local_count * sizeof(int));
        for (int i = 0; i < c.local_count; i++) {
            c.chunk->local_names[i] = c.local_names[i];
            c.chunk->local_types[i] = c.local_types[i];
        }
    }

    for (int i = 0; i < c.portal_count; i++) free(c.portals[i].name);
    free(c.portals);

    return c.chunk;
}

Chunk *compile_function(ASTNode *func_node) {
    if (func_node->kind != NODE_FUNC_DEF) return NULL;
    Compiler c;
    c.chunk = calloc(1, sizeof(Chunk));
    c.local_count = 0;
    c.in_function = true;
    c.top_level = false;
    c.portals = NULL;
    c.portal_count = 0;

    for (int i = 0; i < func_node->data.func.param_count; i++) {
        int slot = add_local(&c, func_node->data.func.params[i]);
        if (func_node->data.func.ptypes && func_node->data.func.ptypes[i] != 0) {
            c.local_types[slot] = func_node->data.func.ptypes[i];
        }
    }

    compile_block(&c, &func_node->data.func.body);
    emit(c.chunk, OP_RETURN, 0);

    c.chunk->local_count = c.local_count;
    if (c.local_count > 0) {
        c.chunk->local_names = malloc(c.local_count * sizeof(char*));
        c.chunk->local_types = malloc(c.local_count * sizeof(int));
        for (int i = 0; i < c.local_count; i++) {
            c.chunk->local_names[i] = c.local_names[i];
            c.chunk->local_types[i] = c.local_types[i];
        }
    }

    return c.chunk;
}
