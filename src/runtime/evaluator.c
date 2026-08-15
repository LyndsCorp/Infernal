/*
 * Infernal: orquestador de evaluación
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator.c
 *
 * Orquestador de evaluator
*/

#include "runtime/evaluator/evaluator.h"
#include "runtime/evaluator/eval_literal.h"
#include "runtime/evaluator/eval_slice.h"
#include "runtime/evaluator/eval_index.h"
#include "runtime/evaluator/eval_binop.h"
#include "runtime/evaluator/eval_call.h"
#include "runtime/evaluator/eval_unary.h"
#include "runtime/evaluator/eval_stmt.h"
#include "runtime/evaluator/eval_flag.h"
#include "runtime/evaluator/helpers.h"
#include "runtime/error.h"
#include "core/value.h"

/* --- eval_expr: orquestador principal --- */
Value eval_expr(ASTNode *expr) {
    switch (expr->kind) {
        case NODE_LITERAL:   return eval_literal(expr);
        case NODE_VAR:       return eval_var(expr);
        case NODE_LIST:      return eval_list(expr);
        case NODE_MAP:       return eval_map(expr);
        case NODE_SLICE:     return eval_slice(expr);
        case NODE_INDEX:     return eval_index(expr);
        case NODE_BINOP:     return eval_binop(expr);
        case NODE_CALL:      return eval_call(expr);
        case NODE_UNARY:     return eval_unary(expr);
        default:
            error(expr->line, "Expresión no implementada (tipo %d)", expr->kind);
    }
    return val_make_null();
}

/* --- Funciones públicas que delegan en implementaciones --- */
void exec_block(NodeList *block) {
    exec_block_impl(block);
}

void exec_block_from(NodeList *block, int start_index) {
    exec_block_from_impl(block, start_index);
}

void exec_flag_spec(FlagSpec *spec) {
    exec_flag_spec_impl(spec);
}
