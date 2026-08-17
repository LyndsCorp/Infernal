/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: core/ast.c
*/

#include <stdlib.h>
#include "ast.h"
#include "memory.h"

ASTNode *node_create(int kind, int line) {
    ASTNode *n = infernal_calloc(1, sizeof(ASTNode));
    n->kind = kind;
    n->line = line;
    return n;
}

void nodelist_add(NodeList *list, ASTNode *node) {
    if (list->count >= list->cap) {
        list->cap = list->cap == 0 ? 8 : list->cap * 2;
        list->stmts = infernal_realloc(list->stmts, list->cap * sizeof(ASTNode*));
    }
    list->stmts[list->count++] = node;
}


static void free_flag_spec(FlagSpec *spec) {
    if (!spec) return;
    for (int i = 0; i < spec->name_count; i++) free(spec->names[i]);
    free(spec->names);
    free(spec->var_name);
    free(spec->body_tokens);
}

void nodelist_free(NodeList *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) ast_free(list->stmts[i]);
    free(list->stmts);
    list->stmts = NULL; list->count = 0; list->cap = 0;
}

void ast_free(ASTNode *node) {
    if (!node) return;
    switch (node->kind) {
        case NODE_PROGRAM: nodelist_free(&node->data.prog.stmts); break;
        case NODE_EXPR_STMT: ast_free(node->data.expr_stmt.expr); break;
        case NODE_CMD_STMT: free(node->data.cmd_stmt.cmd); break;
        case NODE_SHELL_CMD: free(node->data.shell_cmd.cmd); break;
        case NODE_ASSIGN:
            free(node->data.assign.name); free(node->data.assign.cmd_str);
            ast_free(node->data.assign.value); ast_free(node->data.assign.lhs_index);
            break;
        case NODE_IF: ast_free(node->data.if_stmt.cond); nodelist_free(&node->data.if_stmt.then_block); nodelist_free(&node->data.if_stmt.else_block); break;
        case NODE_WHILE: ast_free(node->data.while_stmt.cond); nodelist_free(&node->data.while_stmt.body); break;
        case NODE_FOR: free(node->data.for_stmt.var); ast_free(node->data.for_stmt.init); ast_free(node->data.for_stmt.cond); ast_free(node->data.for_stmt.incr); nodelist_free(&node->data.for_stmt.body); break;
        case NODE_FUNC_DEF:
            free(node->data.func.name);
            for (int i = 0; i < node->data.func.param_count; i++) free(node->data.func.params[i]);
            free(node->data.func.params); free(node->data.func.ptypes);
            nodelist_free(&node->data.func.body);
            break;
        case NODE_RETURN: ast_free(node->data.ret.expr); break;
        case NODE_IMPORT: free(node->data.import.path); nodelist_free(&node->data.import.module_block); break;
        case NODE_TRY: nodelist_free(&node->data.try_stmt.try_block); nodelist_free(&node->data.try_stmt.catch_block); break;
        case NODE_VAR: free(node->data.var.name); break;
        case NODE_LITERAL: free(node->data.lit.sval); break;
        case NODE_BINOP: ast_free(node->data.binop.left); ast_free(node->data.binop.right); break;
        case NODE_CALL:
            free(node->data.call.name);
            for (int i = 0; i < node->data.call.argc; i++) ast_free(node->data.call.args[i]);
            free(node->data.call.args);
            break;
        case NODE_INDEX: ast_free(node->data.idx.list); ast_free(node->data.idx.index); break;
        case NODE_FLAGS:
            for (int i = 0; i < node->data.flags.spec_count; i++) free_flag_spec(&node->data.flags.specs[i]);
            free(node->data.flags.specs);
            break;
        case NODE_LIST:
            for (int i = 0; i < node->data.list_lit.count; i++) ast_free(node->data.list_lit.items[i]);
            free(node->data.list_lit.items);
            break;
        case NODE_FOR_IN: free(node->data.for_in.var); ast_free(node->data.for_in.list_expr); nodelist_free(&node->data.for_in.body); break;
        case NODE_PORTAL: free(node->data.portal.name); break;
        case NODE_REPEAT: ast_free(node->data.repeat.line_expr); free(node->data.repeat.portal_name); break;
        case NODE_SLICE: ast_free(node->data.slice.list); break;
        case NODE_EXECUTE:
            ast_free(node->data.execute.path_expr);
            for (int i = 0; i < node->data.execute.argc; i++) free(node->data.execute.args[i]);
            free(node->data.execute.args);
            break;
        case NODE_MAP:
            for (int i = 0; i < node->data.map.pair_count; i++) { ast_free(node->data.map.pairs[i].key); ast_free(node->data.map.pairs[i].value); }
            free(node->data.map.pairs);
            break;
        case NODE_UNARY: ast_free(node->data.unary.operand); break;
        case NODE_POST_INC: case NODE_POST_DEC: ast_free(node->data.post_op.var); break;
        case NODE_BREAK: case NODE_CONTINUE: break;
    }
    free(node);
}
