/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/eval_index.c
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "eval_index.h"
#include "eval_slice.h"
#include "helpers.h"
#include "evaluator.h"
#include "core/value.h"
#include "runtime/error.h"
#include "runtime/globals.h"

static void eval_index_error(Value *base, Value *idx, int line, const char *fmt, ...) {
    char message[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    value_free(base);
    value_free(idx);
    error(line, "%s", message);
}

Value eval_index(ASTNode *expr) {
    if (expr->data.idx.index->kind == NODE_SLICE) {
        ASTNode *slice = expr->data.idx.index;
        slice->data.slice.list = expr->data.idx.list;
        return eval_slice(slice);
    }
    Value base = eval_expr(expr->data.idx.list);
    Value idx = eval_expr(expr->data.idx.index);
    Value result = val_make_null();

    int line = get_node_line(expr);
    if (line == 0) line = current_eval_line;
    if (line == 0) line = get_node_line(expr->data.idx.index);
    if (line == 0) line = get_node_line(expr->data.idx.list);

    switch (base.type) {
        case VAL_LIST: {
            if (expr->data.idx.list && expr->data.idx.list->kind == NODE_VAR &&
                !expr->data.idx.list->data.var.clone) {
                if (idx.type == VAL_FLOAT) {
                    double f = idx.data.fval;
                    int i = (int)f;
                    if ((double)i != f) eval_index_error(&base, &idx, line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                    idx.type = VAL_INT;
                    idx.data.ival = i;
                }
                if (idx.type != VAL_INT || idx.data.ival < 1 || idx.data.ival > base.data.list.count)
                    eval_index_error(&base, &idx, line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                const char *name = expr->data.idx.list->data.var.name;
                result = val_reference(name, idx.data.ival);
                value_free(&base);
                value_free(&idx);
                return result;
            }
            if (idx.type != VAL_INT) {
                if (idx.type == VAL_FLOAT) {
                    double f = idx.data.fval;
                    int i = (int)f;
                    if ((double)i == f) {
                        if (i < 1 || i > base.data.list.count)
                            eval_index_error(&base, &idx, line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                        if (base.data.list.items == NULL)
                            eval_index_error(&base, &idx, line, "Lista corrupta: items es NULL");
                        result = copy_value_secure(base.data.list.items[i-1]);
                        break;
                    } else {
                        eval_index_error(&base, &idx, line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                    }
                } else {
                    eval_index_error(&base, &idx, line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                }
            }
            int i = idx.data.ival;
            if (i < 1 || i > base.data.list.count)
                eval_index_error(&base, &idx, line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            if (base.data.list.items == NULL)
                eval_index_error(&base, &idx, line, "Lista corrupta: items es NULL");
            result = copy_value_secure(base.data.list.items[i-1]);
            break;
        }
        case VAL_STRING: {
            if (idx.type != VAL_INT) {
                if (idx.type == VAL_FLOAT) {
                    double f = idx.data.fval;
                    int i = (int)f;
                    if ((double)i == f) {
                        size_t length = strlen(base.data.sval);
                        if (i < 1 || (size_t)i > length)
                            eval_index_error(&base, &idx, line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                        char character[2] = {base.data.sval[i-1], '\0'};
                        result = val_string(character);
                        break;
                    } else {
                        eval_index_error(&base, &idx, line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                    }
                } else {
                    eval_index_error(&base, &idx, line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                }
            }
            int position = idx.data.ival;
            size_t length = strlen(base.data.sval);
            if (position < 1 || (size_t)position > length)
                eval_index_error(&base, &idx, line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            char character[2] = {base.data.sval[position - 1], '\0'};
            result = val_string(character);
            break;
        }
        case VAL_MAP: {
            if (idx.type != VAL_STRING) {
                eval_index_error(&base, &idx, line, "La clave de un mapa debe ser string");
            }
            // Comprobar si la clave existe
            if (!val_map_has(base, idx.data.sval)) {
                char map_name[256] = "";
                if (expr->data.idx.list->kind == NODE_VAR) {
                    const char *name = expr->data.idx.list->data.var.name;
                    if (name[0] == '$' || name[0] == '?') name++;
                    snprintf(map_name, sizeof(map_name), " '%s'", name);
                }
                eval_index_error(&base, &idx, line, "Clave '%s' no encontrada en el mapa%s", idx.data.sval, map_name);
            }
            if (expr->data.idx.list && expr->data.idx.list->kind == NODE_VAR &&
                !expr->data.idx.list->data.var.clone) {
                result = val_map_reference(expr->data.idx.list->data.var.name, idx.data.sval);
            } else {
                result = val_map_get(base, idx.data.sval);
            }
            break;
        }
        default:
            eval_index_error(&base, &idx, line, "No se puede indexar este tipo de valor");
    }
    value_free(&base);
    value_free(&idx);
    return result;
}
