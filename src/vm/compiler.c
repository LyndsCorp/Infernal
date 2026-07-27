/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License.
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

typedef struct {
    Chunk *chunk;
    char *local_names[MAX_LOCALS];
    int local_types[MAX_LOCALS];   // guarda el tipo fijo (TOK_LIST, etc.) 0 = ninguno
    int local_count;
    bool in_function;
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
    c->local_types[c->local_count] = 0;   // sin tipo fijo por defecto
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
                    int slot = resolve_local(c, name);
                    if (slot >= 0) {
                        emit(c->chunk, OP_LOAD_VAR, slot);
                    } else {
                        int gidx = vm_find_global_index(name);
                        if (gidx >= 0) {
                            emit(c->chunk, OP_LOAD_GLOBAL, gidx);
                        } else {
                            int const_idx = add_constant(c, val_ptr(expr));
                            emit(c->chunk, OP_INTERPRET_NODE, const_idx);
                            c->chunk->code[c->chunk->code_count - 1].operand2 = 1;
                        }
                    }
                    break;
                }
                case NODE_BINOP: {
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
                            case NODE_INDEX: {
                                compile_expr(c, expr->data.idx.list);
                                compile_expr(c, expr->data.idx.index);
                                emit(c->chunk, OP_INDEX, 0);
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

static void compile_stmt(Compiler *c, ASTNode *stmt) {
    switch (stmt->kind) {
        case NODE_EXPR_STMT:
            compile_expr(c, stmt->data.expr_stmt.expr);
            emit(c->chunk, OP_POP, 0);
            break;
        case NODE_ASSIGN: {
            const char *name = stmt->data.assign.name;
            int slot = resolve_local(c, name);
            if (slot < 0) slot = add_local(c, name);
            // Guardar tipo fijo si existe (por ej. "list" en "list archivo = ls")
            if (stmt->data.assign.vtype != 0) {
                c->local_types[slot] = stmt->data.assign.vtype;
            }
            if (stmt->data.assign.is_cmd) {
                int const_idx = add_constant(c, val_string(stmt->data.assign.cmd_str));
                emit(c->chunk, OP_CMD_ASSIGN, const_idx);
                c->chunk->code[c->chunk->code_count - 1].operand2 = slot;
            } else {
                compile_expr(c, stmt->data.assign.value);
                emit(c->chunk, OP_STORE_VAR, slot);
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

Chunk *compile_program(NodeList *program) {
    Compiler c;
    c.chunk = calloc(1, sizeof(Chunk));
    c.local_count = 0;
    c.in_function = false;

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

    return c.chunk;
}

Chunk *compile_function(ASTNode *func_node) {
    if (func_node->kind != NODE_FUNC_DEF) return NULL;
    Compiler c;
    c.chunk = calloc(1, sizeof(Chunk));
    c.local_count = 0;
    c.in_function = true;

    // Parámetros como locales
    for (int i = 0; i < func_node->data.func.param_count; i++) {
        int slot = add_local(&c, func_node->data.func.params[i]);
        // Si el parámetro tiene tipo definido, se guarda
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
