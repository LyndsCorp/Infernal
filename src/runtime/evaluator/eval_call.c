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
#include <setjmp.h>

Value eval_call(ASTNode *expr) {
    FuncObject *fobj = func_lookup(expr->data.call.name);
    if (!fobj) error(expr->line, "Función no definida: %s", expr->data.call.name);

    if (fobj->kind == FUNC_BUILTIN) {
        int argc = expr->data.call.argc;
        int saved_line = current_eval_line;
        Value *args = argc > 0 ? malloc(sizeof(Value) * (size_t)argc) : NULL;
        if (argc > 0 && !args) error(expr->line, "Memoria insuficiente para argumentos");
        for (int i = 0; i < argc; i++) args[i] = val_make_null();

        jmp_buf saved_env;
        memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
        int saved_raised = exception_raised;
        if (setjmp(exception_env) != 0) {
            for (int i = 0; i < argc; i++) value_free(&args[i]);
            free(args);
            current_eval_line = saved_line;
            memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
            exception_raised = saved_raised;
            longjmp(exception_env, 1);
        }

        for (int i = 0; i < argc; i++) {
            Value arg = eval_expr(expr->data.call.args[i]);
            if (arg.type == VAL_REFERENCE) {
                /* resolve_reference() toma propiedad de la referencia y la libera
                 * incluso cuando lanza un error. Dejamos el slot en NULL durante
                 * la llamada para que el handler no intente liberarla dos veces. */
                Value reference = arg;
                args[i] = val_make_null();
                arg = resolve_reference(reference, expr->line);
            }
            args[i] = arg;
        }

        /* Establecer la línea actual para que los errores muestren la línea real */
        current_eval_line = expr->line;

        Value ret = fobj->builtin(expr->data.call.argc, args);

        current_eval_line = saved_line;   /* restaurar */

        for (int i = 0; i < argc; i++) value_free(&args[i]);
        free(args);
        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
        exception_raised = saved_raised;
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
        jmp_buf saved_env;
        memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
        int saved_raised = exception_raised;
        int saved_cf = control_flow;
        Value saved_ret = return_value;
        bool function_state_active = false;

        if (setjmp(exception_env) != 0) {
            current_scope = prev_scope;
            if (function_state_active) {
                value_free(&return_value);
                return_value = saved_ret;
                control_flow = saved_cf;
            }
            scope_free(new_scope);
            memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
            exception_raised = saved_raised;
            longjmp(exception_env, 1);
        }

        current_scope = new_scope;
        for (int i = 0; i < func->data.func.param_count; i++) {
            Value arg = (i < expr->data.call.argc) ? eval_expr(expr->data.call.args[i]) : val_make_null();
            scope_define(new_scope, func->data.func.params[i], func->data.func.ptypes[i], arg);
        }
        return_value = val_make_null();
        control_flow = CF_NONE;
        function_state_active = true;
        exec_block(&func->data.func.body);

        Value ret = (control_flow == CF_RETURN) ? return_value : val_make_null();
        if (control_flow == CF_RETURN) {
            return_value = val_make_null(); /* ownership transferred to ret */
        }

        control_flow = saved_cf;
        return_value = saved_ret;
        function_state_active = false;
        current_scope = prev_scope;
        scope_free(new_scope);
        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
        exception_raised = saved_raised;
        return ret;
    }
}
