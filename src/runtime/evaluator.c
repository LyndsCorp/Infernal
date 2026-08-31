/*
 * Infernal: orquestador de evaluación
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/evaluator.c
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
#include "runtime/command.h"
#include <stdlib.h>

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
        case NODE_POST_INC:
        case NODE_POST_DEC: {
            ASTNode *var_node = expr->data.post_op.var;
            if (var_node->kind != NODE_VAR)
                error(expr->line, "Incremento/decremento solo soportado para variables");
            const char *raw_name = var_node->data.var.name;
            const char *name = raw_name;
            if (name[0] == '$' || name[0] == '?') name++;
            VarEntry *e = scope_find(current_scope, name);
            if (!e) {
                if (var_node->data.var.clone) {
                    error(expr->line, "Variable '%s' no definida", name);
                }

                /* Un identificador seguido de ++ también puede ser un comando
                 * cuyo nombre contiene '+'. Solo si ese comando no existe,
                 * informamos de que no hay variable. */
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
                error(expr->line, "Variable '%s' no existe y tampoco existe el comando asociado", name);
            }

            Value base = copy_value_secure(e->value);
            Value new_val;
            if (expr->kind == NODE_POST_INC) {
                if (base.type == VAL_INT) new_val = val_int(base.data.ival + 1);
                else if (base.type == VAL_FLOAT) new_val = val_float(base.data.fval + 1.0);
                else {
                    value_free(&base);
                    error(expr->line, "Incremento solo aplicable a números");
                }
            } else {
                if (base.type == VAL_INT) new_val = val_int(base.data.ival - 1);
                else if (base.type == VAL_FLOAT) new_val = val_float(base.data.fval - 1.0);
                else {
                    value_free(&base);
                    error(expr->line, "Decremento solo aplicable a números");
                }
            }
            value_free(&base);

            if (var_node->data.var.clone) {
                /* $ es la máquina de clonación: la operación solo modifica la
                 * copia temporal y devuelve el resultado de esa copia. */
                return new_val;
            }

            /* Aunque la sintaxis sea x++, en Infernal la operación devuelve
             * el valor ya incrementado/decrementado. */
            value_free(&e->value);
            e->value = copy_value_secure(new_val);
            return new_val;
        }
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
