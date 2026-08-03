/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License.
 * Código fuente de Infernal: runtime/evaluator.c
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include "evaluator.h"
#include "core/value.h"
#include "core/ast.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/command.h"
#include "runtime/error.h"
#include "lexer/lexer.h"
#include "parser/parser.h"

static bool val_is_truthy(Value v) {
    switch (v.type) {
        case VAL_BOOL:   return v.data.bval;
        case VAL_INT:    return v.data.ival != 0;
        case VAL_FLOAT:  return v.data.fval != 0.0;
        case VAL_STRING: return v.data.sval != NULL && strlen(v.data.sval) > 0;
        case VAL_LIST:   return v.data.list.count > 0;
        default:         return false;
    }
}

static const char *type_name(int tok_type) {
    switch (tok_type) {
        case TOK_INT:    return "int";
        case TOK_FLOAT:  return "float";
        case TOK_BOOL:   return "bool";
        case TOK_STRING: return "string";
        case TOK_LIST:   return "list";
        default:         return "desconocido";
    }
}

static bool try_convert_value(Value *val, int target_tok_type) {
    if (val->type == VAL_STRING) {
        const char *s = val->data.sval;

        if (target_tok_type == TOK_INT) {
            char *end;
            long n = strtol(s, &end, 10);
            if (*end == '\0' && end != s) {
                free(val->data.sval);
                val->type = VAL_INT;
                val->data.ival = (int)n;
                return true;
            }
            return false;
        }

        if (target_tok_type == TOK_FLOAT) {
            char *normalized = strdup(s);
            for (char *p = normalized; *p; p++) {
                if (*p == ',') *p = '.';
            }
            char *end;
            double f = strtod(normalized, &end);
            if (*end == '\0' && end != normalized) {
                free(val->data.sval);
                val->type = VAL_FLOAT;
                val->data.fval = f;
                free(normalized);
                return true;
            }
            free(normalized);
            return false;
        }

        if (target_tok_type == TOK_BOOL) {
            if (strcasecmp(s, "true") == 0 || strcmp(s, "1") == 0) {
                free(val->data.sval);
                val->type = VAL_BOOL;
                val->data.bval = true;
                return true;
            }
            if (strcasecmp(s, "false") == 0 || strcmp(s, "0") == 0) {
                free(val->data.sval);
                val->type = VAL_BOOL;
                val->data.bval = false;
                return true;
            }
            return false;
        }
        return false;
    }

    if (val->type == VAL_INT && target_tok_type == TOK_FLOAT) {
        val->type = VAL_FLOAT;
        val->data.fval = (double)val->data.ival;
        return true;
    }
    if (val->type == VAL_FLOAT && target_tok_type == TOK_INT) {
        val->type = VAL_INT;
        val->data.ival = (int)val->data.fval;
        return true;
    }
    return false;
}

void exec_flag_spec(FlagSpec *spec) {
    TokenStream saved_ts = ts;
    ts.tokens = spec->body_tokens;
    ts.count = spec->body_count;
    ts.pos = 0;
    NodeList flag_body = parse_block(NULL);
    exec_block(&flag_body);
    ts = saved_ts;
}

Value eval_expr(ASTNode *expr) {
    switch (expr->kind) {
        case NODE_LITERAL: {
            if (expr->data.lit.type == TOK_INT)    return val_int(expr->data.lit.ival);
            if (expr->data.lit.type == TOK_FLOAT)  return val_float(expr->data.lit.fval);
            if (expr->data.lit.type == TOK_BOOL)   return val_bool(expr->data.lit.bval);
            if (expr->data.lit.type == TOK_STRING) return val_string(expr->data.lit.sval);
            return val_make_null();
        }
        case NODE_VAR: {
            const char *name = expr->data.var.name;
            if (name[0] == '$') {
                name++;
                if (*name == '\0') error(expr->line, "Nombre de variable vacío tras $");
            }
            // Buscar en la cadena de ámbitos (local → global → superglobal)
            VarEntry *e = scope_find(current_scope, name);
            if (!e) error(expr->line, "Variable no definida: %s", name);
            // Si es una referencia a una lista, resolverla
            if (e->value.type == VAL_REFERENCE) {
                VarEntry *list_var = scope_find(current_scope, e->value.data.ref.list_name);
                if (!list_var || list_var->value.type != VAL_LIST) {
                    error(expr->line, "La referencia apunta a una lista inexistente '%s'",
                          e->value.data.ref.list_name);
                } else {
                    int idx = e->value.data.ref.index;
                    if (idx < 1 || idx > list_var->value.data.list.count)
                        error(expr->line, "Índice de referencia fuera de rango");
                    return list_var->value.data.list.items[idx - 1];
                }
            }
            return e->value;
        }
        case NODE_LIST: {
            Value list = val_list_empty();
            for (int i = 0; i < expr->data.list_lit.count; i++) {
                Value item = eval_expr(expr->data.list_lit.items[i]);
                val_list_append(&list, item);
            }
            return list;
        }
        case NODE_BINOP: {
            if (expr->data.binop.op == TOK_AND) {
                Value left = eval_expr(expr->data.binop.left);
                if (!val_is_truthy(left)) return val_bool(false);
                Value right = eval_expr(expr->data.binop.right);
                return val_bool(val_is_truthy(right));
            }
            if (expr->data.binop.op == TOK_OR) {
                Value left = eval_expr(expr->data.binop.left);
                if (val_is_truthy(left)) return val_bool(true);
                Value right = eval_expr(expr->data.binop.right);
                return val_bool(val_is_truthy(right));
            }

            Value left = eval_expr(expr->data.binop.left);

            if (left.type == VAL_LIST && expr->data.binop.op == TOK_PLUS &&
                expr->data.binop.right->kind == NODE_INDEX) {
                ASTNode *idx_node = expr->data.binop.right;
            Value base = eval_expr(idx_node->data.idx.list);
            Value index_val = eval_expr(idx_node->data.idx.index);
            int pos = (index_val.type == VAL_INT) ? index_val.data.ival : 1;
            Value new_list = val_list_copy(&left);
            if (pos < 1 || pos > new_list.data.list.count + 1) {
                pos = new_list.data.list.count + 1;
            }
            val_list_append(&new_list, val_make_null());
            for (int i = new_list.data.list.count - 1; i > pos - 1; i--) {
                new_list.data.list.items[i] = new_list.data.list.items[i - 1];
            }
            new_list.data.list.items[pos-1] = base;
            return new_list;
                }

                Value right = eval_expr(expr->data.binop.right);

                if (left.type == VAL_LIST && expr->data.binop.op == TOK_MINUS && right.type == VAL_INT) {
                    int pos = right.data.ival;
                    Value new_list = val_list_copy(&left);
                    if (pos < 1 || pos > new_list.data.list.count)
                        error(expr->line, "Índice fuera de rango para eliminación: %d", pos);
                    for (int i = pos-1; i < new_list.data.list.count-1; i++)
                        new_list.data.list.items[i] = new_list.data.list.items[i+1];
                    new_list.data.list.count--;
                    return new_list;
                }

                if (expr->data.binop.op == TOK_EEQ || expr->data.binop.op == TOK_NEQ) {
                    bool equal = false;
                    if (left.type == right.type) {
                        switch (left.type) {
                            case VAL_NULL:   equal = true; break;
                            case VAL_BOOL:   equal = (left.data.bval == right.data.bval); break;
                            case VAL_INT:    equal = (left.data.ival == right.data.ival); break;
                            case VAL_FLOAT:  equal = (left.data.fval == right.data.fval); break;
                            case VAL_STRING: equal = (strcmp(left.data.sval, right.data.sval) == 0); break;
                            default: equal = false;
                        }
                    }
                    return val_bool(expr->data.binop.op == TOK_EEQ ? equal : !equal);
                }

                if (expr->data.binop.op == TOK_LT_OP || expr->data.binop.op == TOK_GT_OP ||
                    expr->data.binop.op == TOK_LE    || expr->data.binop.op == TOK_GE) {
                    double lv = (left.type == VAL_INT) ? left.data.ival :
                    (left.type == VAL_FLOAT) ? left.data.fval : 0.0;
                double rv = (right.type == VAL_INT) ? right.data.ival :
                (right.type == VAL_FLOAT) ? right.data.fval : 0.0;
                switch (expr->data.binop.op) {
                    case TOK_LT_OP: return val_bool(lv < rv);
                    case TOK_GT_OP: return val_bool(lv > rv);
                    case TOK_LE:    return val_bool(lv <= rv);
                    case TOK_GE:    return val_bool(lv >= rv);
                    default: break;
                }
                    }

                    if (left.type == VAL_STRING || right.type == VAL_STRING) {
                        char lbuf[64], rbuf[64];
                        const char *ls = left.type == VAL_STRING ? left.data.sval : lbuf;
                        const char *rs = right.type == VAL_STRING ? right.data.sval : rbuf;
                        if (left.type != VAL_STRING)
                            snprintf(lbuf, sizeof(lbuf), "%d", left.type == VAL_INT ? left.data.ival :
                            left.type == VAL_FLOAT ? (int)left.data.fval : left.data.bval ? 1 : 0);
                        if (right.type != VAL_STRING)
                            snprintf(rbuf, sizeof(rbuf), "%d", right.type == VAL_INT ? right.data.ival :
                            right.type == VAL_FLOAT ? (int)right.data.fval : right.data.bval ? 1 : 0);
                        size_t total = strlen(ls) + strlen(rs) + 1;
                        char *buf = malloc(total);
                        if (!buf) error(expr->line, "Memoria insuficiente al concatenar cadenas");
                        snprintf(buf, total, "%s%s", ls, rs);
                        Value result = val_string(buf);
                        free(buf);
                        return result;
                    }

                    if (left.type == VAL_INT && right.type == VAL_INT) {
                        int lv = left.data.ival, rv = right.data.ival;
                        switch (expr->data.binop.op) {
                            case TOK_PLUS:  return val_int(lv + rv);
                            case TOK_MINUS: return val_int(lv - rv);
                            case TOK_STAR:  return val_int(lv * rv);
                            case TOK_SLASH: if (rv == 0) error(expr->line, "División por cero"); return val_float((double)lv / rv);
                            case TOK_PERCENT: if (rv == 0) error(expr->line, "Módulo por cero"); return val_int(lv % rv);
                            default: error(expr->line, "Operador no soportado");
                        }
                    }

                    double lv = (left.type == VAL_INT) ? left.data.ival :
                    (left.type == VAL_FLOAT) ? left.data.fval :
                    (left.type == VAL_BOOL) ? (left.data.bval ? 1.0 : 0.0) : 0.0;
                    double rv = (right.type == VAL_INT) ? right.data.ival :
                    (right.type == VAL_FLOAT) ? right.data.fval :
                    (right.type == VAL_BOOL) ? (right.data.bval ? 1.0 : 0.0) : 0.0;
                    switch (expr->data.binop.op) {
                        case TOK_PLUS: return val_float(lv + rv);
                        case TOK_MINUS: return val_float(lv - rv);
                        case TOK_STAR: return val_float(lv * rv);
                        case TOK_SLASH: if (rv == 0) error(expr->line, "División por cero"); return val_float(lv / rv);
                        case TOK_PERCENT: if (rv == 0) error(expr->line, "Módulo por cero"); return val_float((int)lv % (int)rv);
                        default: error(expr->line, "Operador no soportado");
                    }
                    break;
        }
                        case NODE_CALL: {
                            FuncObject *fobj = func_lookup(expr->data.call.name);
                            if (!fobj) error(expr->line, "Función no definida: %s", expr->data.call.name);

                            if (fobj->kind == FUNC_BUILTIN) {
                                Value *args = malloc(sizeof(Value) * expr->data.call.argc);
                                for (int i = 0; i < expr->data.call.argc; i++)
                                    args[i] = eval_expr(expr->data.call.args[i]);
                                Value ret = fobj->builtin(expr->data.call.argc, args);
                                free(args);
                                return ret;
                            } else {
                                ASTNode *func = fobj->def;
                                if (expr->data.call.argc != func->data.func.param_count) {
                                    error(expr->line, "La función '%s' espera %d argumento(s), recibió %d",
                                          expr->data.call.name, func->data.func.param_count,
                                          expr->data.call.argc);
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
                        case NODE_INDEX: {
                            Value base = eval_expr(expr->data.idx.list);
                            Value idx = eval_expr(expr->data.idx.index);
                            if (base.type == VAL_LIST) {
                                int i = (idx.type == VAL_INT) ? idx.data.ival : 1;
                                if (i < 1 || i > base.data.list.count)
                                    error(expr->line, "Índice fuera de rango");
                                return base.data.list.items[i-1];
                            } else if (base.type == VAL_STRING) {
                                if (idx.type != VAL_INT)
                                    error(expr->line, "El índice de string debe ser un entero");
                                int position = idx.data.ival;
                                size_t length = strlen(base.data.sval);
                                if (position < 1 || (size_t)position > length)
                                    error(expr->line, "Índice de string fuera de rango");
                                char character[2] = {base.data.sval[position - 1], '\0'};
                                return val_string(character);
                            }
                            error(expr->line, "No se puede indexar este tipo de valor");
                            return val_make_null();
                        }
                        default:
                            error(expr->line, "Expresión no implementada");
    }
    return val_make_null();
}

void exec_block(NodeList *block) {
    exec_block_from(block, 0);
}

void exec_block_from(NodeList *block, int start_index) {
    for (int i = start_index; i < block->count; i++) {
        if (control_flow != CF_NONE) break;

        ASTNode *stmt = block->stmts[i];
        switch (stmt->kind) {
            case NODE_EXPR_STMT:
                eval_expr(stmt->data.expr_stmt.expr);
                break;
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
                int ret = system(expanded);
                if (ret != 0) {
                    free(expanded);
                    error(stmt->line, "falló: %s", stmt->data.shell_cmd.cmd);
                }
                free(expanded);
                break;
            }
            case NODE_ASSIGN: {
                Value val;
                if (stmt->data.assign.is_cmd) {
                    char *cmd = stmt->data.assign.cmd_str;
                    int exit_code = 0;

                    // Si es bool, ejecutamos sin mostrar salida
                    if (stmt->data.assign.vtype == TOK_BOOL) {
                        char *expanded_cmd = expand_command(cmd);
                        FILE *fp = popen(expanded_cmd, "r");
                        if (!fp) {
                            error(stmt->line, "Error al ejecutar comando: %s", expanded_cmd);
                        }
                        // Leer y descartar la salida para evitar bloqueos del proceso hijo
                        char buf[1024];
                        while (fgets(buf, sizeof(buf), fp) != NULL) {
                            // descartar
                        }
                        int status = pclose(fp);
                        if (WIFEXITED(status)) {
                            exit_code = WEXITSTATUS(status);
                        } else {
                            exit_code = -1;
                        }
                        free(expanded_cmd);
                        val = val_bool(exit_code == 0);
                    } else {
                        // Para otros tipos (int, string, list) capturamos la salida
                        char *expanded_cmd = expand_command(cmd);
                        FILE *fp = NULL;
                        char *temp_path = NULL;
                        int is_embedded = 0;

                        if (expanded_cmd[0] == '!' && expanded_cmd[strlen(expanded_cmd)-1] == '!') {
                            is_embedded = 1;
                            char *trimmed = strdup(expanded_cmd + 1);
                            trimmed[strlen(trimmed)-1] = '\0';
                            fp = popen_embedded_with_path(trimmed, "r", &temp_path);
                            free(trimmed);
                        } else {
                            fp = popen(expanded_cmd, "r");
                        }

                        if (!fp) {
                            if (is_embedded)
                                error(stmt->line, "Comando embebido no encontrado: %s", expanded_cmd);
                            else
                                error(stmt->line, "Error al ejecutar comando: %s", expanded_cmd);
                        }
                        char buf[4096];
                        char *out = strdup("");
                        while (fgets(buf, sizeof(buf), fp)) {
                            out = realloc(out, strlen(out) + strlen(buf) + 1);
                            strcat(out, buf);
                        }
                        int status = pclose(fp);
                        if (status != 0) {
                            error(stmt->line, "Comando falló: %s", expanded_cmd);
                        }

                        if (temp_path) {
                            unlink(temp_path);
                            free(temp_path);
                        }

                        size_t len = strlen(out);
                        if (len > 0 && out[len-1] == '\n') out[len-1] = '\0';

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
                            val = val_string(out);
                            free(out);
                        }
                        free(expanded_cmd);
                    }
                } else {
                    // Asignación normal de expresión
                    if (stmt->data.assign.lhs_index) {
                        VarEntry *var = scope_find(current_scope, stmt->data.assign.name);
                        if (!var || var->value.type != VAL_LIST)
                            error(stmt->line, "Se esperaba una lista en '%s'", stmt->data.assign.name);
                        Value idx_val = eval_expr(stmt->data.assign.lhs_index);
                        if (idx_val.type != VAL_INT)
                            error(stmt->line, "El índice debe ser un entero");
                        int idx = idx_val.data.ival;
                        if (idx < 1 || idx > var->value.data.list.count)
                            error(stmt->line, "Índice fuera de rango");
                        val = eval_expr(stmt->data.assign.value);
                        var->value.data.list.items[idx - 1] = val;
                        break;
                    }
                    val = eval_expr(stmt->data.assign.value);
                }

                // Aplicar conversión de tipos (si el valor no es del tipo esperado)
                int vtype = stmt->data.assign.vtype;
                if (vtype != 0) {
                    int actual_type = valtype_to_tokentype(val.type);
                    if (vtype != actual_type) {
                        if (!try_convert_value(&val, vtype)) {
                            error(stmt->line, "Error de tipado fijo: se esperaba %s pero se obtuvo %s",
                                  type_name(vtype), type_name(actual_type));
                        }
                    }
                }

                // Definir/Asignar en el ámbito correspondiente
                if (stmt->data.assign.is_global) {
                    scope_define(super_global_scope, stmt->data.assign.name, vtype, val);
                    char env_name[512];
                    snprintf(env_name, sizeof(env_name), "INFERNAL_VAR_%s", stmt->data.assign.name);
                    char *str_val = NULL;
                    switch (val.type) {
                        case VAL_INT:   asprintf(&str_val, "i:%d", val.data.ival); break;
                        case VAL_FLOAT: asprintf(&str_val, "f:%g", val.data.fval); break;
                        case VAL_BOOL:  asprintf(&str_val, "b:%s", val.data.bval ? "true" : "false"); break;
                        case VAL_STRING: asprintf(&str_val, "s:%s", val.data.sval); break;
                        default:        str_val = strdup("s:");
                    }
                    setenv(env_name, str_val, 1);
                    free(str_val);
                } else if (stmt->data.assign.is_local) {
                    scope_define(current_scope, stmt->data.assign.name, vtype, val);
                } else {
                    VarEntry *var = scope_find(global_scope, stmt->data.assign.name);
                    if (var) {
                        scope_assign(global_scope, stmt->data.assign.name, val, stmt->line);
                        if (var->vtype == 0 && vtype != 0) {
                            var->vtype = vtype;
                        }
                    } else {
                        scope_define(global_scope, stmt->data.assign.name, vtype, val);
                    }
                }
                break;
            }
                        case NODE_IF: {
                            Value cond = eval_expr(stmt->data.if_stmt.cond);
                            bool truthy = val_is_truthy(cond);
                            Scope *block_scope = scope_new(current_scope, NULL);
                            Scope *old_scope = current_scope;
                            current_scope = block_scope;
                            if (truthy)
                                exec_block(&stmt->data.if_stmt.then_block);
                            else
                                exec_block(&stmt->data.if_stmt.else_block);
                            current_scope = old_scope;
                            if (control_flow == CF_REPEAT_LINE) return;
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
                                exec_block(&stmt->data.while_stmt.body);
                                current_scope = old_scope;
                                if (control_flow == CF_BREAK) { control_flow = CF_NONE; break; }
                                if (control_flow == CF_CONTINUE) { control_flow = CF_NONE; continue; }
                                if (control_flow == CF_REPEAT_LINE) return;
                                if (control_flow == CF_RETURN) return;
                            }
                            break;
                        }
                        case NODE_FOR: {
                            Value init_val = eval_expr(stmt->data.for_stmt.init);
                            Scope *for_scope = scope_new(current_scope, NULL);
                            Scope *old_scope = current_scope;
                            current_scope = for_scope;
                            scope_define(for_scope, stmt->data.for_stmt.var, stmt->data.for_stmt.vtype, init_val);
                            int iter_count = 0;
                            while (1) {
                                if (iter_count >= max_loop_iterations)
                                    error(stmt->line, "Límite de iteraciones (%d) alcanzado en bucle for", max_loop_iterations);
                                iter_count++;
                                Value cond = eval_expr(stmt->data.for_stmt.cond);
                                if (!val_is_truthy(cond)) break;
                                exec_block(&stmt->data.for_stmt.body);
                                if (control_flow == CF_BREAK) { control_flow = CF_NONE; break; }
                                if (control_flow == CF_CONTINUE) { control_flow = CF_NONE; }
                                if (control_flow == CF_REPEAT_LINE) { current_scope = old_scope; return; }
                                if (control_flow == CF_RETURN) { current_scope = old_scope; return; }
                                eval_expr(stmt->data.for_stmt.incr);
                            }
                            current_scope = old_scope;
                            break;
                        }
                        case NODE_FOR_IN: {
                            Value list_val = eval_expr(stmt->data.for_in.list_expr);
                            if (list_val.type != VAL_LIST) error(stmt->line, "Se esperaba una lista en for-in");
                            for (int i = 0; i < list_val.data.list.count; i++) {
                                Scope *iter_scope = scope_new(current_scope, NULL);
                                Scope *old_scope = current_scope;
                                current_scope = iter_scope;
                                scope_define(iter_scope, stmt->data.for_in.var, 0, list_val.data.list.items[i]);
                                exec_block(&stmt->data.for_in.body);
                                current_scope = old_scope;
                                if (control_flow == CF_BREAK) { control_flow = CF_NONE; break; }
                                if (control_flow == CF_CONTINUE) { control_flow = CF_NONE; continue; }
                                if (control_flow == CF_REPEAT_LINE) return;
                                if (control_flow == CF_RETURN) return;
                            }
                            break;
                        }
                        case NODE_FUNC_DEF: break;
                        case NODE_RETURN:
                            return_value = stmt->data.ret.expr ? eval_expr(stmt->data.ret.expr) : val_make_null();
                            control_flow = CF_RETURN;
                            return;
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
                            for (int j = i + 1; j < block->count; j++) {
                                if (block->stmts[j]->kind != NODE_PORTAL) {
                                    next_line = block->stmts[j]->line;
                                    break;
                                }
                            }
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
                            exec_block(&stmt->data.import.module_block);
                            current_scope = old_scope;
                            if (control_flow == CF_REPEAT_LINE) return;
                            break;
                        }
                        case NODE_TRY: {
                            jmp_buf saved_env; memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
                            int saved_raised = exception_raised; exception_raised = 0;
                            if (!setjmp(exception_env)) {
                                exec_block(&stmt->data.try_stmt.try_block);
                            } else {
                                exception_raised = 0;
                                exec_block(&stmt->data.try_stmt.catch_block);
                            }
                            memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
                            exception_raised = saved_raised;
                            if (control_flow == CF_REPEAT_LINE) return;
                            break;
                        }
                        case NODE_FLAGS: {
                            int mode = stmt->data.flags.mode;
                            bool *handled = calloc(script_argc, sizeof(bool));
                            FlagSpec *catch_all = NULL;
                            for (int s = 0; s < stmt->data.flags.spec_count; s++) {
                                if (stmt->data.flags.specs[s].catch_all) {
                                    catch_all = &stmt->data.flags.specs[s];
                                    break;
                                }
                            }

                            int total_matched = 0;

                            if (mode == 1) {
                                int arg_idx = 2;
                                for (int s = 0; s < stmt->data.flags.spec_count; s++) {
                                    FlagSpec *spec = &stmt->data.flags.specs[s];
                                    if (spec->catch_all) continue;
                                    if (arg_idx >= script_argc)
                                        error(stmt->line, "Falta el argumento para el flag posicional %d", arg_idx - 1);
                                    if (spec->vtype && spec->var_name) {
                                        char *val_str = script_argv[arg_idx];
                                        char cleaned[512]; int c = 0;
                                        if (val_str[0] == '"' || val_str[0] == '\'') {
                                            char quote = val_str[0];
                                            for (int j=1; val_str[j] && val_str[j] != quote; j++) cleaned[c++] = val_str[j];
                                            cleaned[c] = '\0';
                                        } else {
                                            strncpy(cleaned, val_str, sizeof(cleaned));
                                            cleaned[sizeof(cleaned)-1] = '\0';
                                        }
                                        if (spec->vtype == TOK_FLOAT) {
                                            char *coma = strchr(cleaned, ',');
                                            if (coma) *coma = '.';
                                        }
                                        Value v;
                                        switch (spec->vtype) {
                                            case TOK_INT: v = val_int(atoi(cleaned)); break;
                                            case TOK_FLOAT: v = val_float(atof(cleaned)); break;
                                            case TOK_BOOL: v = val_bool(strcmp(cleaned,"0")!=0 && strlen(cleaned)>0); break;
                                            case TOK_STRING: v = val_string(cleaned); break;
                                            default: v = val_string(cleaned);
                                        }
                                        scope_define(current_scope, spec->var_name, spec->vtype, v);
                                    }
                                    exec_flag_spec(spec);
                                    handled[arg_idx] = true;
                                    total_matched++;
                                    arg_idx++;
                                }
                                if (catch_all) {
                                    for (int a = 2; a < script_argc; a++)
                                        if (!handled[a]) {
                                            scope_define(current_scope, "_", 0, val_string(script_argv[a]));
                                            exec_flag_spec(catch_all);
                                            total_matched++;
                                        }
                                }
                            } else {
                                for (int a = 2; a < script_argc; a++) {
                                    char *arg = script_argv[a];
                                    char *arg_dup = strdup(arg);
                                    char *eq_pos = strchr(arg_dup, '=');
                                    if (eq_pos) *eq_pos = '\0';
                                    bool matched = false;
                                    for (int s = 0; s < stmt->data.flags.spec_count; s++) {
                                        FlagSpec *spec = &stmt->data.flags.specs[s];
                                        if (spec->catch_all) continue;
                                        for (int n = 0; n < spec->name_count; n++) {
                                            if (strcmp(arg_dup, spec->names[n]) == 0) {
                                                if (spec->vtype && spec->var_name) {
                                                    if (!eq_pos && a + 1 >= script_argc)
                                                        error(stmt->line, "Falta el valor para el flag '%s'", arg_dup);
                                                    char *val_str = eq_pos ? eq_pos + 1 : script_argv[++a];
                                                    char cleaned[512]; int c = 0;
                                                    if (val_str[0] == '"' || val_str[0] == '\'') {
                                                        char quote = val_str[0];
                                                        for (int j=1; val_str[j] && val_str[j] != quote; j++) cleaned[c++] = val_str[j];
                                                        cleaned[c] = '\0';
                                                    } else {
                                                        strncpy(cleaned, val_str, sizeof(cleaned));
                                                        cleaned[sizeof(cleaned)-1] = '\0';
                                                    }
                                                    if (spec->vtype == TOK_FLOAT) {
                                                        char *coma = strchr(cleaned, ',');
                                                        if (coma) *coma = '.';
                                                    }
                                                    Value v;
                                                    switch (spec->vtype) {
                                                        case TOK_INT: v = val_int(atoi(cleaned)); break;
                                                        case TOK_FLOAT: v = val_float(atof(cleaned)); break;
                                                        case TOK_BOOL: v = val_bool(strcmp(cleaned,"0")!=0 && strlen(cleaned)>0); break;
                                                        case TOK_STRING: v = val_string(cleaned); break;
                                                        default: v = val_string(cleaned);
                                                    }
                                                    scope_define(current_scope, spec->var_name, spec->vtype, v);
                                                }
                                                exec_flag_spec(spec);
                                                handled[a] = true;
                                                matched = true;
                                                total_matched++;
                                                break;
                                            }
                                        }
                                        if (matched) break;
                                    }
                                    if (!matched && arg_dup[0] == '-' && arg_dup[1] != '-' && strlen(arg_dup) > 2) {
                                        for (int c = 1; arg_dup[c]; c++) {
                                            char sn[3] = {'-', arg_dup[c], '\0'};
                                            bool found = false;
                                            for (int s = 0; s < stmt->data.flags.spec_count; s++) {
                                                FlagSpec *spec = &stmt->data.flags.specs[s];
                                                if (spec->catch_all) continue;
                                                for (int n = 0; n < spec->name_count; n++) {
                                                    if (strcmp(sn, spec->names[n]) == 0) {
                                                        exec_flag_spec(spec);
                                                        found = true;
                                                        total_matched++;
                                                        break;
                                                    }
                                                }
                                                if (found) break;
                                            }
                                            if (!found && catch_all) {
                                                scope_define(current_scope, "_", 0, val_string(sn));
                                                exec_flag_spec(catch_all);
                                                total_matched++;
                                            }
                                        }
                                        handled[a] = true;
                                    } else if (!matched && catch_all) {
                                        scope_define(current_scope, "_", 0, val_string(arg));
                                        exec_flag_spec(catch_all);
                                        handled[a] = true;
                                        total_matched++;
                                    }
                                    free(arg_dup);
                                }
                            }
                            if (total_matched == 0 && catch_all != NULL) exec_flag_spec(catch_all);
                            free(handled);
                            break;
                        }
                                                        default: error(stmt->line, "Sentencia no implementada");
        }

        if (control_flow != CF_NONE) break;
    }
}
