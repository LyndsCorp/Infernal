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
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/constants.h"
#include "runtime/command.h"
#include <stdlib.h>

/* --- Helper: aplica +/- 1 sobre un Value numérico --- */
static Value compute_incdec(Value base, int delta, int line) {
    if (base.type == VAL_INT)
        return val_int(base.data.ival + delta);
    if (base.type == VAL_FLOAT)
        return val_float(base.data.fval + (double)delta);
    error(line, delta > 0
                ? "Incremento solo aplicable a números"
                : "Decremento solo aplicable a números");
    return val_make_null();
}

/* --- eval_expr (orquestador de expresiones) --- */
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
        case NODE_EMBEDDED:  return eval_embedded(expr);
        case NODE_POST_INC:
        case NODE_POST_DEC: {
            int delta = (expr->kind == NODE_POST_INC) ? 1 : -1;
            ASTNode *target = expr->data.post_op.var;

            if (target->kind == NODE_VAR) {
                const char *raw_name = target->data.var.name;
                const char *name = raw_name;
                if (name[0] == '$') name++;
                if (name[0] == '?') {
                    error(expr->line,
                          "'?' no es un prefijo de variable en Infernal. "
                          "'?nombre' solo tiene sentido en un comando shell.");
                }

                if (constants_is_reserved(name))
                    error(expr->line, "La constante '%s' no se puede modificar", name);

                VarEntry *e = eval_var_find_entry(target, name);
                if (!e) {
                    if (target->data.var.clone || !expr->data.post_op.statement_context) {
                        error(expr->line, "Variable '%s' no definida", name);
                    }
                    char *command = NULL;
                    if (asprintf(&command, "%s%s", name,
                                 expr->kind == NODE_POST_INC ? "++" : "--") < 0 || !command) {
                        error(expr->line, "Memoria insuficiente al resolver '%s'", name);
                    }
                    int status = run_command_get_exit_code(command);
                    free(command);
                    if (status == 0) {
                        return val_make_null();
                    }
                    error(expr->line, "Variable '%s' no definida", name);
                }

                Value base = copy_value_secure(e->value);
                Value new_val = compute_incdec(base, delta, expr->line);
                value_free(&base);

                if (target->data.var.clone) {
                    return new_val;
                }

                value_free(&e->value);
                e->value = copy_value_secure(new_val);
                return new_val;
            }

            if (target->kind == NODE_INDEX) {
                ASTNode *base_node = target->data.idx.list;
                if (base_node->kind != NODE_VAR || base_node->data.var.clone) {
                    error(expr->line,
                          "Incremento/decremento en índice requiere un elemento "
                          "de una variable sin '$' como base");
                }
                const char *base_name = base_node->data.var.name;
                if (base_name[0] == '$') base_name++;
                if (constants_is_reserved(base_name))
                    error(expr->line, "La constante '%s' no se puede modificar", base_name);

                VarEntry *container_entry = scope_find(current_scope, base_name);
                if (!container_entry)
                    error(expr->line, "Variable '%s' no definida", base_name);

                if (container_entry->value.type == VAL_MAP) {
                    Value key = eval_expr(target->data.idx.index);
                    if (key.type == VAL_REFERENCE) key = resolve_reference(key, expr->line);
                    if (key.type != VAL_STRING) {
                        value_free(&key);
                        error(expr->line,
                              "El índice de un mapa debe ser string (se recibió '%s')",
                              value_type_name(key.type));
                    }

                    Value current;
                    if (val_map_has(container_entry->value, key.data.sval)) {
                        current = val_map_get(container_entry->value, key.data.sval);
                    } else {
                        current = val_int(0);
                    }

                    Value new_val = compute_incdec(current, delta, expr->line);
                    value_free(&current);

                    /* val_map_set ya copia internamente; el
                     * copy_value_secure extra se filtraba en
                     * cada x[i]++ sobre un mapa. */
                    val_map_set(&container_entry->value, key.data.sval, new_val);
                    value_free(&key);
                    return new_val;
                }

                if (container_entry->value.type == VAL_LIST) {
                    Value ref = eval_index(target);
                    if (ref.type != VAL_REFERENCE) {
                        error(expr->line,
                              "Incremento/decremento en lista requiere un elemento válido");
                    }
                    Value current = resolve_reference(ref, expr->line);
                    Value new_val = compute_incdec(current, delta, expr->line);
                    value_free(&current);

                    Value ref2 = eval_index(target);
                    /* assign_reference ya copia internamente; el
                     * copy_value_secure extra se filtraba en
                     * cada x[i]++ sobre una lista. */
                    assign_reference(&ref2, new_val, expr->line);
                    value_free(&ref2);
                    return new_val;
                }

                error(expr->line,
                      "'%s' es de tipo '%s' y no admite incremento/decremento con índice",
                      base_name, value_type_name(container_entry->value.type));
            }

            error(expr->line, "Incremento/decremento no soportado en este contexto");
        }
        case NODE_EMPTY_AMBIGUOUS:
            error(expr->line,
                  "'[]' es ambiguo en este contexto: no se puede saber si es list o map. "
                  "Usa 'list nombre = []' o 'map nombre = []' para declararlo con un tipo.");
        default:
            error(expr->line, "Se encontró una sentencia donde se esperaba una expresión. "
            "Revisa el incremento del bucle for: debe ser una expresión simple "
            "como 'i = i + 1' o 'i += 1'.");
            return val_make_null();
    }
}

/* --- exec_block (orquestador de bloques) --- */
void exec_block(NodeList *block) {
    exec_block_impl(block);
}

void exec_block_from(NodeList *block, int start_index) {
    exec_block_from_impl(block, start_index);
}

void exec_flag_spec(FlagSpec *spec) {
    exec_flag_spec_impl(spec);
}
