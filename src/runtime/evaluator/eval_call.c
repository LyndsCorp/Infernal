/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/eval_call.c
*/

#include "eval_call.h"
#include "evaluator.h"
#include "helpers.h"
#include "core/value.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include <stdlib.h>
#include <string.h>

Value eval_call(ASTNode *expr) {
    FuncObject *fobj = func_lookup(expr->data.call.name);
    if (!fobj) error(expr->line, "Función no definida: %s", expr->data.call.name);

    if (fobj->kind == FUNC_BUILTIN) {
        Value *args = malloc(sizeof(Value) * expr->data.call.argc);
        for (int i = 0; i < expr->data.call.argc; i++) {
            args[i] = eval_expr(expr->data.call.args[i]);
        }

        /* Establecer la línea actual para que los errores muestren la línea real */
        int saved_line = current_eval_line;
        current_eval_line = expr->line;

        Value ret = fobj->builtin(expr->data.call.argc, args);

        current_eval_line = saved_line;   /* restaurar */

        free(args);
        return ret;
    } else {
        /* Función de usuario */
        ASTNode *func = fobj->def;
        if (expr->data.call.argc != func->data.func.param_count) {
            error(expr->line, "La función '%s' espera %d argumento(s), recibió %d",
                  expr->data.call.name, func->data.func.param_count, expr->data.call.argc);
        }
        Scope *new_scope = scope_new(current_scope, expr->data.call.name);
        Scope *prev_scope = current_scope;
        current_scope = new_scope;
        for (int i = 0; i < func->data.func.param_count; i++) {
            Value arg = (i < expr->data.call.argc) ? eval_expr(expr->data.call.args[i]) : val_make_null();
            scope_define(new_scope, func->data.func.params[i], func->data.func.ptypes[i], arg);
        }
        int saved_cf = control_flow;
        Value saved_ret = return_value;
        control_flow = CF_NONE;
        exec_block(&func->data.func.body);
        Value ret = (control_flow == CF_RETURN) ? return_value : val_make_null();
        control_flow = saved_cf;
        return_value = saved_ret;
        current_scope = prev_scope;
        return ret;
    }
}
