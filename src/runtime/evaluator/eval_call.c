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
    if (!fobj) {
        const char *name = expr->data.call.name;
        const char *dot  = strrchr(name, '.');

        /* Un nombre con forma 'modulo.funcion' NO es una función suelta
         * que el usuario olvidó definir: es una llamada a una función de
         * un módulo. El error, por tanto, no es "función no definida"
         * sino "el módulo no está importado" o "el módulo no expone esa
         * función". Distinguimos ambos casos mirando si existe alguna
         * función ya registrada con el prefijo del módulo. */
        if (dot && dot != name && dot[1] != '\0') {
            char module[256];
            size_t module_len = (size_t)(dot - name);
            if (module_len >= sizeof(module)) module_len = sizeof(module) - 1;
            memcpy(module, name, module_len);
            module[module_len] = '\0';
            const char *func = dot + 1;
            size_t prefix_len = module_len + 1;   /* incluye el '.' */

            bool module_loaded = false;
            for (FuncEntry *e = func_table; e; e = e->next) {
                if (strncmp(e->name, name, prefix_len) == 0) {
                    module_loaded = true;
                    break;
                }
            }
            if (!module_loaded) {
                for (FuncEntry *e = super_func_table; e; e = e->next) {
                    if (strncmp(e->name, name, prefix_len) == 0) {
                        module_loaded = true;
                        break;
                    }
                }
            }

            if (module_loaded) {
                error(expr->line,
                      "El módulo '%s' no expone ninguna función llamada '%s'.\n"
                      "    '%s' se interpreta como una función del módulo '%s' porque contiene un punto.\n"
                      "    Revisa el nombre: puede estar mal escrito, o el módulo no exporta esa función.\n"
                      "    Si querías llamar a una función propia del script, no uses puntos en el nombre.",
                      module, func, name, module);
            } else {
                error(expr->line,
                      "El módulo '%s' no está importado, así que '%s' no existe.\n"
                      "    '%s' se interpreta como una función del módulo '%s' porque contiene un punto.\n"
                      "    Añade 'import %s' (o 'import %s as <alias>') antes de esta llamada.\n"
                      "    Si querías llamar a una función propia del script, no uses puntos en el nombre.",
                      module, name, name, module, module, module);
            }
        }

        error(expr->line, "Función no definida: %s", name);
    }

    if (fobj->kind == FUNC_BUILTIN) {
        int argc = expr->data.call.argc;
        int saved_line = current_eval_line;
        Value * volatile args = argc > 0 ? malloc(sizeof(Value) * (size_t)argc) : NULL;
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

        /* varexists(NOMBRE) y delvar(NOMBRE) reciben el NOMBRE como string,
         * no el valor de la variable. Si el argumento es un identificador
         * sin '$' (NODE_VAR con clone=false), pasamos su nombre como string
         * SIN evaluarlo. Esto permite comprobar/eliminar variables que
         * todavía no existen (el caso típico de varexists) sin que
         * eval_expr dispare "Variable 'X' no definida" antes de que el
         * builtin pueda hacer su trabajo.
         *
         *   varexists(NOMBRE)    -> comprueba si existe "NOMBRE"
         *   varexists($NOMBRE)   -> comprueba si existe la variable cuyo
         *                           nombre está guardado en $NOMBRE */
        const char *fname = expr->data.call.name;
        bool arg_is_name = (strcmp(fname, "varexists") == 0 ||
                            strcmp(fname, "delvar") == 0);

        for (int i = 0; i < argc; i++) {
            ASTNode *arg_node = expr->data.call.args[i];
            if (arg_is_name && arg_node->kind == NODE_VAR &&
                !arg_node->data.var.clone) {
                const char *vname = arg_node->data.var.name;
                if (vname[0] == '$') vname++;
                args[i] = val_string(vname);
                continue;
            }

            Value arg = eval_expr(arg_node);
            if (arg.type == VAL_REFERENCE) {
                /* resolve_reference() toma propiedad de la referencia y la libera
                 * incluso cuando lanza un error. Dejamos el slot en NULL durante
                 * la llamada para que el handler no intente liberarla dos veces. */
                Value reference = arg;
                args[i] = val_make_null();
                arg = resolve_reference(reference, expr->line);
            }

            /* NUEVO: rechazar 'nada' (void) como argumento a builtin. */
            if (arg.type == VAL_VOID) {
                error(expr->data.call.args[i]->line,
                      "El argumento %d de '%s' no devolvió ningún valor (void).\n"
                      "    No se puede pasar 'nada' a una función.",
                      i + 1, expr->data.call.name);
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
            ASTNode *arg_node = (i < expr->data.call.argc) ? expr->data.call.args[i] : NULL;
            int ptype = func->data.func.ptypes[i];
            Value arg;

            /* Si el argumento es un '[]' ambiguo y el parámetro tiene un tipo
             * explícito, resolvemos el literal vacío a partir del tipo del
             * parámetro. Así `f([])` funciona cuando f declara ese parámetro
             * como 'list' o 'map'. Si el parámetro es de tipo automático
             * (ptype == 0), no hay información para desambiguar y se mantiene
             * el error, pero con un mensaje que apunta al parámetro concreto. */
            if (arg_node && arg_node->kind == NODE_EMPTY_AMBIGUOUS) {
                if (ptype == TOK_LIST) {
                    arg = val_list_empty();
                } else if (ptype == TOK_MAP) {
                    arg = val_map_empty();
                } else {
                    error(arg_node->line,
                          "'[]' es ambiguo: el parámetro %d de '%s' no tiene un tipo declarado,\n"
                          "    así que no se puede saber si debería ser list o map.\n"
                          "    Declara el parámetro como 'list' o 'map' en la definición de '%s',\n"
                          "    o pasa explícitamente una colección (variable por ejemplo) vacía del tipo deseado.",
                          i + 1, expr->data.call.name, expr->data.call.name);
                }
            } else {
                arg = arg_node ? eval_expr(arg_node) : val_make_null();
            }

            /* NUEVO: rechazar 'nada' (void) como argumento. */
            if (arg.type == VAL_VOID) {
                error(arg_node ? arg_node->line : expr->line,
                      "El argumento %d de '%s' no devolvió ningún valor (void).\n"
                      "    No se puede pasar 'nada' a un parámetro.",
                      i + 1, expr->data.call.name);
            }

            scope_define(new_scope, func->data.func.params[i], ptype, arg);
        }
        return_value = val_make_null();
        control_flow = CF_NONE;
        function_state_active = true;
        exec_block(&func->data.func.body);

        Value ret = (control_flow == CF_RETURN) ? return_value : val_void();
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
