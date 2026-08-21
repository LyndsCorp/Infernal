/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/eval_stmt.c
 *
 * Implementación de la ejecución de sentencias.
*/

#include "eval_stmt.h"
#include "eval_flag.h"
#include "helpers.h"
#include "evaluator.h"
#include "core/value.h"
#include "core/ast.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/command.h"
#include "runtime/error.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "developer/debug.h"
#include "vm/vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

/* --- Funciones auxiliares de bloque --- */
void exec_block_impl(NodeList *block) {
    exec_block_from_impl(block, 0);
}

void exec_block_from_impl(NodeList *block, int start_index) {
    for (int i = start_index; i < block->count; i++) {
        if (control_flow != CF_NONE) break;
        ASTNode *stmt = block->stmts[i];
        current_eval_line = stmt->line;
        DEBUG_INFO("exec_block: ejecutando sentencia tipo %d en línea %d", stmt->kind, stmt->line);
        exec_stmt(stmt);
        if (control_flow != CF_NONE) break;
    }
}


static bool switch_values_equal(Value left, Value right) {
    if (left.type != right.type) return false;
    switch (left.type) {
        case VAL_NULL: return true;
        case VAL_BOOL: return left.data.bval == right.data.bval;
        case VAL_INT: return left.data.ival == right.data.ival;
        case VAL_FLOAT: return left.data.fval == right.data.fval;
        case VAL_STRING:
            return left.data.sval && right.data.sval && strcmp(left.data.sval, right.data.sval) == 0;
        default: return false;
    }
}

/* --- Implementación de exec_stmt --- */
void exec_stmt(ASTNode *stmt) {
    DEBUG_INFO("exec_stmt: kind=%d, line=%d", stmt->kind, stmt->line);

    switch (stmt->kind) {

        case NODE_EXPR_STMT: {
            Value result = eval_expr(stmt->data.expr_stmt.expr);
            value_free(&result);
            break;
        }

        case NODE_POST_INC:
        case NODE_POST_DEC: {
            Value result = eval_expr(stmt);
            value_free(&result);
            break;
        }

        case NODE_CMD_STMT: {
            char *expanded = expand_command(stmt->data.cmd_stmt.cmd);
            int ret = execute_embedded(expanded);
            if (ret == -1) {
                free(expanded);
                error(stmt->line, "Comando embebido no encontrado: %s", stmt->data.cmd_stmt.cmd);
            }
            free(expanded);
            break;
        }

        case NODE_SHELL_CMD: {
            char *expanded = expand_command(stmt->data.shell_cmd.cmd);
            int ret = run_shell_command(expanded);
            if (ret != 0)
                error(stmt->line, "falló: %s", stmt->data.shell_cmd.cmd);
            free(expanded);
            break;
        }

        case NODE_ASSIGN: {
            DEBUG_INFO("ASIGNACION: nombre='%s', is_cmd=%d", stmt->data.assign.name, stmt->data.assign.is_cmd);

            Value val = val_make_null();

            if (stmt->data.assign.is_cmd) {
                char *cmd = stmt->data.assign.cmd_str;
                int exit_code = 0;

                // Expandir variables en el comando
                char *expanded_cmd = expand_command(cmd);
                if (!expanded_cmd) {
                    error(stmt->line, "Error al expandir comando: %s", cmd);
                }

                // Determinar si es un comando embebido (entre !!)
                int is_embedded = (expanded_cmd[0] == '!' && expanded_cmd[strlen(expanded_cmd)-1] == '!');

                char *cmd_with_redir = NULL;
                if (is_embedded) {
                    // Los embebidos no se ejecutan a través del shell, no podemos redirigir con 2>&1
                    cmd_with_redir = strdup(expanded_cmd);
                } else {
                    // Redirigir stderr según el tipo de la variable
                    if (stmt->data.assign.vtype == TOK_BOOL) {
                        // Para booleanos: queremos silenciar completamente la salida
                        asprintf(&cmd_with_redir, "%s 2>/dev/null", expanded_cmd);
                    } else {
                        // Para otros tipos: capturamos también stderr (2>&1) para que no llegue a la terminal
                        asprintf(&cmd_with_redir, "%s 2>&1", expanded_cmd);
                    }
                }
                free(expanded_cmd);  // ya no necesitamos el original

                if (!cmd_with_redir) {
                    error(stmt->line, "Memoria insuficiente para redirigir comando");
                }

                FILE *fp = NULL;
                char *temp_path = NULL;

                if (is_embedded) {
                    // Los embebidos se manejan con popen_embedded_with_path
                    char *trimmed = strdup(cmd_with_redir + 1);
                    trimmed[strlen(trimmed)-1] = '\0';
                    fp = popen_embedded_with_path(trimmed, "r", &temp_path);
                    free(trimmed);
                } else {
                    fp = popen(cmd_with_redir, "r");
                }

                free(cmd_with_redir);  // ya podemos liberar la cadena con redirección

                if (!fp) {
                    error(stmt->line, "Error al ejecutar comando: %s", cmd);
                }

                // Si es booleano, solo nos interesa el código de salida
                if (stmt->data.assign.vtype == TOK_BOOL) {
                    char buf[1024];
                    while (fgets(buf, sizeof(buf), fp) != NULL) {} // descartar salida
                    int status = pclose(fp);
                    if (WIFEXITED(status)) {
                        exit_code = WEXITSTATUS(status);
                    } else {
                        exit_code = -1;
                    }
                    if (temp_path) {
                        unlink(temp_path);
                        free(temp_path);
                    }
                    val = val_bool(exit_code == 0);
                } else {
                    // Para otros tipos: capturar la salida (incluyendo stderr si se redirigió)
                    char buf[4096];
                    char *out = strdup("");
                    if (!out) {
                        pclose(fp);
                        if (temp_path) { unlink(temp_path); free(temp_path); }
                        error(stmt->line, "Memoria insuficiente para capturar salida");
                    }
                    while (fgets(buf, sizeof(buf), fp)) {
                        size_t old_len = strlen(out);
                        size_t add_len = strlen(buf);
                        if (old_len > SIZE_MAX - add_len - 1) {
                            free(out);
                            pclose(fp);
                            if (temp_path) { unlink(temp_path); free(temp_path); }
                            error(stmt->line, "Salida de comando demasiado grande");
                        }
                        char *tmp_out = realloc(out, old_len + add_len + 1);
                        if (!tmp_out) {
                            free(out);
                            pclose(fp);
                            if (temp_path) { unlink(temp_path); free(temp_path); }
                            error(stmt->line, "Memoria insuficiente para salida de comando");
                        }
                        out = tmp_out;
                        memcpy(out + old_len, buf, add_len + 1);
                    }
                    int status = pclose(fp);
                    if (status != 0 && status != -1) {
                        // El comando falló, pero aún queremos la salida (si la hay)
                        // No lanzamos error, simplemente asignamos lo que haya devuelto.
                        // Opcional: si se quiere lanzar error, descomentar:
                        // error(stmt->line, "Comando falló: %s", cmd);
                    }
                    if (temp_path) {
                        unlink(temp_path);
                        free(temp_path);
                    }
                    // Quitar salto de línea final
                    size_t len = strlen(out);
                    if (len > 0 && out[len-1] == '\n') out[len-1] = '\0';

                    // Asignar según el tipo esperado
                    if (stmt->data.assign.vtype == TOK_LIST) {
                        Value list = val_list_empty();
                        char *dup = strdup(out);
                        char *saveptr;
                        char *line = strtok_r(dup, "\n", &saveptr);
                        while (line) {
                            val_list_append(&list, val_string(line));
                            line = strtok_r(NULL, "\n", &saveptr);
                        }
                        free(dup);
                        free(out);
                        val = list;
                    } else {
                        // Para int, float, string, etc. se asigna como string (luego se convertirá)
                        val = val_string(out);
                        free(out);
                    }
                }
            } else {
                // Asignación normal (no comando)
                // ... (el código existente para asignaciones normales, sin cambios)
                if (stmt->data.assign.value != NULL && stmt->data.assign.lhs_index) {
                    // Asignación con índice (ej: lista[2] = 5)
                    VarEntry *var = scope_find(current_scope, stmt->data.assign.name);
                    if (!var) error(stmt->line, "Variable no definida: %s", stmt->data.assign.name);
                    Value idx_val = eval_expr(stmt->data.assign.lhs_index->data.idx.index);
                    if (var->value.type == VAL_LIST) {
                        if (idx_val.type != VAL_INT) error(stmt->line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                        int idx = idx_val.data.ival;
                        if (idx < 1 || idx > var->value.data.list.count) error(stmt->line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                        Value new_val = eval_expr(stmt->data.assign.value);
                        value_free(&var->value.data.list.items[idx - 1]);
                        var->value.data.list.items[idx - 1] = copy_value_secure(new_val);
                        value_free(&new_val);
                    } else if (var->value.type == VAL_MAP) {
                        if (idx_val.type != VAL_STRING) error(stmt->line, "Clave de mapa debe ser string");
                        Value new_val = eval_expr(stmt->data.assign.value);
                        val_map_set(&var->value, idx_val.data.sval, new_val);
                        value_free(&new_val);
                    } else {
                        value_free(&idx_val);
                        error(stmt->line, "No se puede indexar este tipo de variable");
                    }
                    value_free(&idx_val);
                    break;
                }
                // Asignación sin índice
                if (stmt->data.assign.value != NULL)
                    val = eval_expr(stmt->data.assign.value);
                else if (stmt->data.assign.vtype == TOK_INT)
                    val = val_int(0);
                else if (stmt->data.assign.vtype == TOK_FLOAT)
                    val = val_float(0.0);
                else if (stmt->data.assign.vtype == TOK_BOOL)
                    val = val_bool(false);
                else if (stmt->data.assign.vtype == TOK_STRING)
                    val = val_string("");
                else if (stmt->data.assign.vtype == TOK_LIST)
                    val = val_list_empty();
                else if (stmt->data.assign.vtype == TOK_MAP)
                    val = val_map_empty();
            }

            // --- Conversión de tipos (si hay tipo fijo) ---
            int vtype = stmt->data.assign.vtype;

            if (vtype == TOK_STRING && val.type == VAL_LIST) {
                if (!try_convert_value(&val, TOK_STRING)) {
                    error(stmt->line, "No se pudo convertir la lista a string en la asignación a '%s'",
                          stmt->data.assign.name);
                }
            }

            if (vtype != 0) {
                int actual_type = valtype_to_tokentype(val.type);
                if (vtype != actual_type) {
                    if (!try_convert_value(&val, vtype)) {
                        error(stmt->line, "Error de tipado fijo: se esperaba %s pero se obtuvo %s",
                              type_name(vtype), type_name(actual_type));
                    }
                }
            }

            DEBUG_INFO("NODE_ASSIGN: is_global=%d, is_local=%d, nombre='%s'",
                       stmt->data.assign.is_global, stmt->data.assign.is_local, stmt->data.assign.name);

            // --- Almacenar la variable en el ámbito correspondiente ---
            if (stmt->data.assign.is_global) {
                DEBUG_INFO("Definiendo global '%s' en super_global_scope", stmt->data.assign.name);
                scope_define(super_global_scope, stmt->data.assign.name, vtype, val);

                // Sincronizar con la VM para que las lecturas posteriores vean el valor
                int gidx = vm_find_global_index(stmt->data.assign.name);
                if (gidx < 0) {
                    gidx = vm_register_global(stmt->data.assign.name, GLOBAL_SUPER, vtype);
                }
                if (gidx >= 0) {
                    value_free(&vm_globals[gidx]);
                    vm_globals[gidx] = copy_value_secure(val);
                    if (vtype != 0) vm_global_types[gidx] = vtype;
                }
            } else if (stmt->data.assign.is_local) {
                DEBUG_INFO("Definiendo local '%s' en current_scope", stmt->data.assign.name);
                scope_define(current_scope, stmt->data.assign.name, vtype, val);
            } else {
                // Asignación sin calificador: buscar la variable en la cadena de ámbitos
                VarEntry *var = scope_find(current_scope, stmt->data.assign.name);
                if (var) {
                    DEBUG_INFO("Variable '%s' encontrada en ámbito %p, actualizando", stmt->data.assign.name, (void*)var);
                    Value copied = copy_value_secure(val);
                    value_free(&var->value);
                    var->value = copied;
                    value_free(&val); // la copia ya es propiedad de la variable
                    if (vtype != 0) {
                        var->vtype = vtype;
                    }
                } else {
                    // Si no existe, definir en global_scope (ámbito del script)
                    DEBUG_INFO("Variable '%s' no encontrada, definiendo en global_scope", stmt->data.assign.name);
                    scope_define(global_scope, stmt->data.assign.name, vtype, val);
                }
            }
            break;
        }

        case NODE_IF: {
            Value cond = eval_expr(stmt->data.if_stmt.cond);
            bool truthy = val_is_truthy(cond);
            if (truthy) {
                exec_block_impl(&stmt->data.if_stmt.then_block);
            } else {
                exec_block_impl(&stmt->data.if_stmt.else_block);
            }
            if (control_flow == CF_REPEAT_LINE) return;
            break;
        }

        case NODE_SWITCH: {
            Value selector = eval_expr(stmt->data.switch_stmt.expr);
            bool matched = false;

            for (int i = 0; i < stmt->data.switch_stmt.case_count && !matched; i++) {
                SwitchCase *swcase = &stmt->data.switch_stmt.cases[i];
                Value case_value = eval_expr(swcase->value);
                bool equal = switch_values_equal(selector, case_value);
                value_free(&case_value);   // liberar el valor del caso

                if (equal) {
                    matched = true;
                    if (swcase->body.count > 0) {
                        exec_block_impl(&swcase->body);
                        // Si el bloque tenía break, limpiar la bandera
                        if (control_flow == CF_BREAK) {
                            control_flow = CF_NONE;
                        }
                        // Salir del bucle (ya ejecutamos el primer caso con cuerpo)
                        break;
                    }
                    // Si el caso no tiene cuerpo, seguimos buscando el siguiente caso con cuerpo
                    // (pero sin evaluar más casos, porque matched ya es true)
                }
            }

            // Si no hubo coincidencia y existe default, ejecutarlo
            if (!matched && stmt->data.switch_stmt.has_default) {
                exec_block_impl(&stmt->data.switch_stmt.default_block);
                if (control_flow == CF_BREAK) {
                    control_flow = CF_NONE;
                }
            }

            // Liberar el selector una sola vez
            value_free(&selector);
            break;
        }

        case NODE_WHILE: {
            int iter_count = 0;
            while (1) {
                if (iter_count >= max_loop_iterations)
                    error(stmt->line, "Límite de iteraciones (%d) alcanzado en bucle while", max_loop_iterations);
                iter_count++;
                Value cond = eval_expr(stmt->data.while_stmt.cond);
                if (!val_is_truthy(cond)) break;
                Scope *block_scope = scope_new(current_scope, NULL);
                Scope *old_scope = current_scope;
                current_scope = block_scope;
                exec_block_impl(&stmt->data.while_stmt.body);
                current_scope = old_scope;
                scope_free(block_scope);
                if (control_flow == CF_BREAK) { control_flow = CF_NONE; break; }
                if (control_flow == CF_CONTINUE) { control_flow = CF_NONE; continue; }
                if (control_flow == CF_REPEAT_LINE) return;
                if (control_flow == CF_RETURN) return;
            }
            break;
        }

        case NODE_FOR: {
            DEBUG_INFO("=== EJECUTANDO NODE_FOR en línea %d ===", stmt->line);

            Scope *old_scope = current_scope;
            bool is_global_for = stmt->data.for_stmt.is_global;

            /*
             * 1) Evaluar la expresión inicial.
             *
             * NO ejecutamos el NODE_ASSIGN completo porque eso podría
             * crear la variable en el scope incorrecto.
             */
            Value init_val = val_make_null();

            if (stmt->data.for_stmt.init &&
                stmt->data.for_stmt.init->kind == NODE_ASSIGN) {

                ASTNode *init_expr =
                stmt->data.for_stmt.init->data.assign.value;

            if (init_expr) {
                current_scope = old_scope;
                init_val = eval_expr(init_expr);

                DEBUG_INFO(
                    "NODE_FOR: init_val = %d",
                    init_val.data.ival
                );
            }
                }

                /*
                 * 2) Elegir el scope.
                 *
                 * for global:
                 *     usa directamente super_global_scope
                 *
                 * for local / normal:
                 *     crea su propio scope temporal
                 */
                Scope *for_scope;

                if (is_global_for) {
                    for_scope = super_global_scope;

                    DEBUG_INFO(
                        "NODE_FOR: usando super_global_scope"
                    );
                } else {
                    for_scope = scope_new(old_scope, NULL);

                    DEBUG_INFO(
                        "NODE_FOR: creado for_scope"
                    );
                }

                /*
                 * 3) Definir la variable del for.
                 *
                 * Usamos los datos del NODE_FOR, no los del NODE_ASSIGN.
                 */
                const char *var_name =
                stmt->data.for_stmt.var;

                int vtype =
                stmt->data.for_stmt.vtype;

                /*
                 * Si ya existe una variable con ese nombre EN ESTE MISMO
                 * scope, actualizarla en lugar de crear otra.
                 *
                 * Esto es importante cuando el archivo ya utilizó 'i'
                 * anteriormente.
                 */
                VarEntry *existing =
                scope_find_current(for_scope, var_name);

                if (existing) {
                    Value copied = copy_value_secure(init_val);
                    value_free(&existing->value);
                    existing->value = copied;

                    if (vtype != 0) {
                        existing->vtype = vtype;
                    }

                    DEBUG_INFO(
                        "NODE_FOR: variable '%s' ya existía; actualizada",
                        var_name
                    );
                } else {
                    scope_define(
                        for_scope,
                            var_name,
                            vtype,
                            init_val
                    );

                    DEBUG_INFO(
                        "NODE_FOR: variable '%s' definida en %s con valor %d",
                        var_name,
                        is_global_for
                        ? "super_global_scope"
                        : "for_scope",
                        init_val.data.ival
                    );
                }

                current_scope = for_scope;

                int iter_count = 0;

                while (1) {

                    /*
                     * 4) Límite de seguridad.
                     */
                    if (iter_count >= max_loop_iterations) {
                        error(
                            stmt->line,
                            "Límite de iteraciones (%d) alcanzado en bucle for",
                              max_loop_iterations
                        );
                    }

                    iter_count++;

                    /*
                     * 5) Evaluar condición.
                     */
                    current_scope = for_scope;

                    Value cond =
                    eval_expr(stmt->data.for_stmt.cond);

                    if (!val_is_truthy(cond)) {
                        break;
                    }

                    /*
                     * 6) Ejecutar cuerpo en un scope hijo.
                     */
                    Scope *body_scope =
                    scope_new(for_scope, NULL);

                    Scope *old_body =
                    current_scope;

                    current_scope =
                    body_scope;

                    exec_block_impl(
                        &stmt->data.for_stmt.body
                    );

                    current_scope =
                    old_body;

                    scope_free(body_scope);

                    /*
                     * 7) Control de flujo.
                     */
                    if (control_flow == CF_BREAK) {
                        control_flow = CF_NONE;
                        break;
                    }

                    if (control_flow == CF_CONTINUE) {
                        control_flow = CF_NONE;
                    }

                    if (control_flow == CF_REPEAT_LINE ||
                        control_flow == CF_RETURN) {

                        current_scope = old_scope;

                    /*
                     * Nunca liberar super_global_scope.
                     */
                    if (!is_global_for) {
                        scope_free(for_scope);
                    }

                    return;
                        }

                        /*
                         * 8) Incremento.
                         */
                        if (stmt->data.for_stmt.incr) {
                            current_scope = for_scope;

                            exec_stmt(
                                stmt->data.for_stmt.incr
                            );
                        }
                }

                /*
                 * 9) Restaurar scope anterior.
                 */
                current_scope = old_scope;

                /*
                 * Un for global usa super_global_scope,
                 * así que NO se libera.
                 */
                if (!is_global_for) {
                    scope_free(for_scope);
                }

                break;
        }

        case NODE_FOR_IN: {
            Value list_val = eval_expr(stmt->data.for_in.list_expr);
            if (list_val.type != VAL_LIST) error(stmt->line, "Se esperaba una lista en for-in");
            for (int i = 0; i < list_val.data.list.count; i++) {
                Scope *iter_scope = scope_new(current_scope, NULL);
                Scope *old_scope = current_scope;
                current_scope = iter_scope;
                scope_define(iter_scope, stmt->data.for_in.var, 0, copy_value_secure(list_val.data.list.items[i]));
                exec_block_impl(&stmt->data.for_in.body);
                current_scope = old_scope;
                scope_free(iter_scope);
                if (control_flow == CF_BREAK) { control_flow = CF_NONE; break; }
                if (control_flow == CF_CONTINUE) { control_flow = CF_NONE; continue; }
                if (control_flow == CF_REPEAT_LINE) { value_free(&list_val); return; }
                if (control_flow == CF_RETURN) { value_free(&list_val); return; }
            }
            value_free(&list_val);
            break;
        }

        case NODE_FUNC_DEF:
            break;

        case NODE_RETURN: {
            Value result = stmt->data.ret.expr ? eval_expr(stmt->data.ret.expr) : val_make_null();

            if (stmt->data.ret.rtype != 0) {
                int expected = stmt->data.ret.rtype;
                if (expected == TOK_STRING && result.type == VAL_LIST) {
                    if (!try_convert_value(&result, expected)) {
                        value_free(&result);
                        error(stmt->line, "No se pudo convertir el valor de retorno a %s", type_name(expected));
                    }
                }

                int actual = valtype_to_tokentype(result.type);
                if (actual != expected) {
                    value_free(&result);
                    error(stmt->line, "Tipado del retorno: se esperaba %s, se recibió %s",
                          type_name(expected), type_name(actual));
                }
            }

            value_free(&return_value);
            return_value = result;
            control_flow = CF_RETURN;
            return;
        }

        case NODE_BREAK:
            control_flow = CF_BREAK;
            return;

        case NODE_CONTINUE:
            control_flow = CF_CONTINUE;
            return;

        case NODE_PORTAL: {
            const char *name = stmt->data.portal.name;
            bool is_local = stmt->data.portal.is_local;
            Scope *target_scope = is_local ? current_scope : global_scope;
            if (portal_find_in_scope(target_scope, name)) {
                error(stmt->line, "Portal '%s' ya existe en este ámbito", name);
            }
            int next_line = stmt->line + 1;
            portal_define(target_scope, name, next_line);
            break;
        }

        case NODE_REPEAT: {
            if (stmt->data.repeat.portal_name) {
                PortalEntry *p = portal_find(current_scope, stmt->data.repeat.portal_name);
                if (!p) error(stmt->line, "Portal '%s' no encontrado", stmt->data.repeat.portal_name);
                repeat_line_target = p->line;
            } else {
                Value line_val = eval_expr(stmt->data.repeat.line_expr);
                if (line_val.type != VAL_INT)
                    error(stmt->line, "repeat line requiere un número entero");
                repeat_line_target = line_val.data.ival;
            }
            control_flow = CF_REPEAT_LINE;
            return;
        }

        case NODE_IMPORT: {
            Scope *old_scope = current_scope; current_scope = global_scope;
            exec_block_impl(&stmt->data.import.module_block);
            current_scope = old_scope;
            if (control_flow == CF_REPEAT_LINE) return;
            break;
        }

        case NODE_TRY: {
            jmp_buf saved_env; memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
            int saved_raised = exception_raised; exception_raised = 0;
            if (!setjmp(exception_env)) {
                exec_block_impl(&stmt->data.try_stmt.try_block);
            } else {
                exception_raised = 0;
                exec_block_impl(&stmt->data.try_stmt.catch_block);
            }
            memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
            exception_raised = saved_raised;
            if (control_flow == CF_REPEAT_LINE) return;
            break;
        }

        case NODE_EXECUTE: {
            Value path_val = eval_expr(stmt->data.execute.path_expr);
            if (stmt->data.execute.path_expr && stmt->data.execute.path_expr->kind == NODE_VAR) {
                const char *var_name = stmt->data.execute.path_expr->data.var.name;
                if (strchr(var_name, '/') != NULL) {
                    error(stmt->line,
                          "Uso incorrecto de variable con barra: '%s'. Si intentabas concatenar, usa '+': "
                          "$%s + '/resto'. La barra '/' directa solo es válida en comandos shell, no en execute.",
                          var_name, var_name);
                }
            }
            if (path_val.type != VAL_STRING) {
                error(stmt->line, "La ruta del script debe ser una cadena");
            }
            char *raw_path = path_val.data.sval;
            char *expanded_path = expand_command(raw_path);
            free(raw_path);
            if (!expanded_path) {
                error(stmt->line, "Error al expandir la ruta del script");
            }

            int expanded_argc = stmt->data.execute.argc;
            char **expanded_args = NULL;
            if (expanded_argc > 0) {
                expanded_args = malloc(expanded_argc * sizeof(char*));
                for (int i = 0; i < expanded_argc; i++) {
                    expanded_args[i] = expand_command(stmt->data.execute.args[i]);
                    if (!expanded_args[i]) expanded_args[i] = strdup("");
                }
            }

            int saved_argc = script_argc;
            char **saved_argv = script_argv;
            int saved_flags_arg_index = flags_arg_index;

            char **new_argv = malloc((expanded_argc + 2) * sizeof(char*));
            new_argv[0] = expanded_path;
            for (int i = 0; i < expanded_argc; i++) {
                new_argv[i + 1] = expanded_args[i];
            }
            new_argv[expanded_argc + 1] = NULL;

            script_argc = expanded_argc + 1;
            script_argv = new_argv;
            flags_arg_index = 1;

            FILE *fp = fopen(expanded_path, "r");
            if (!fp) {
                error(stmt->line, "No se pudo abrir el script '%s'", expanded_path);
            }

            TokenStream saved_ts = ts;
            ts_init();
            tokenize_file(fp);
            fclose(fp);

            NodeList script_block = parse_block(NULL);
            ts = saved_ts;

            Scope *child_scope = scope_new(current_scope, NULL);
            Scope *old_scope = current_scope;
            current_scope = child_scope;

            exec_block_impl(&script_block);

            current_scope = old_scope;
            scope_free(child_scope);

            script_argc = saved_argc;
            script_argv = saved_argv;
            flags_arg_index = saved_flags_arg_index;

            free(expanded_path);
            for (int i = 0; i < expanded_argc; i++) free(expanded_args[i]);
            free(expanded_args);
            free(new_argv);

            break;
        }

        case NODE_FLAGS: {
            exec_flags(stmt);
            break;
        }

        default:
            DEBUG_ERROR("Nodo desconocido en exec_stmt: kind=%d, line=%d", stmt->kind, stmt->line);
            error(stmt->line, "Error interno: nodo no reconocido (tipo %d). Asegúrate de que la sintaxis sea correcta.", stmt->kind);
    }
}
