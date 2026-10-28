/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/eval_unary.c
 */

#include "eval_unary.h"
#include "core/value.h"
#include "evaluator.h"
#include "helpers.h"
#include "runtime/error.h"

Value eval_unary(ASTNode *expr) {
    if (expr->data.unary.op == TOK_NOT) {
        Value v = eval_expr(expr->data.unary.operand);

        /* NUEVO: 'not' sobre void no tiene sentido lógico:
         * 'nada' no es ni verdadero ni falso. */
        if (v.type == VAL_VOID) {
            error(expr->line, "No se puede aplicar 'not' a una expresión sin valor (void).");
        }

        Value result = val_bool(!val_is_truthy(v));
        value_free(&v);
        return result;
    }
    error(expr->line, "Operador unario no implementado");
    return val_make_null();
}
