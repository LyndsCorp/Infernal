/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/eval_index.c
*/

#include "eval_index.h"
#include "eval_slice.h"
#include "helpers.h"
#include "evaluator.h"
#include "core/value.h"
#include "runtime/error.h"
#include "runtime/globals.h"
#include <string.h>
#include <stdlib.h>

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
            if (idx.type != VAL_INT) {
                if (idx.type == VAL_FLOAT) {
                    double f = idx.data.fval;
                    int i = (int)f;
                    if ((double)i == f) {
                        if (i < 1 || i > base.data.list.count)
                            error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                        if (base.data.list.items == NULL)
                            error(line, "Lista corrupta: items es NULL");
                        result = copy_value_secure(base.data.list.items[i-1]);
                        break;
                    } else {
                        error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                    }
                } else {
                    error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                }
            }
            int i = idx.data.ival;
            if (i < 1 || i > base.data.list.count)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            if (base.data.list.items == NULL)
                error(line, "Lista corrupta: items es NULL");
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
                            error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                        char character[2] = {base.data.sval[i-1], '\0'};
                        result = val_string(character);
                        break;
                    } else {
                        error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                    }
                } else {
                    error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                }
            }
            int position = idx.data.ival;
            size_t length = strlen(base.data.sval);
            if (position < 1 || (size_t)position > length)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            char character[2] = {base.data.sval[position - 1], '\0'};
            result = val_string(character);
            break;
        }
        case VAL_MAP: {
            if (idx.type != VAL_STRING) {
                error(line, "La clave de un mapa debe ser string");
            }
            result = val_map_get(base, idx.data.sval);
            break;
        }
        default:
            error(line, "No se puede indexar este tipo de valor");
    }
    value_free(&base);
    value_free(&idx);
    return result;
}
