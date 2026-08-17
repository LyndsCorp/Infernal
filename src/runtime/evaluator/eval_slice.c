/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/eval_slice.c
*/

#include "eval_slice.h"
#include "helpers.h"
#include "evaluator.h"
#include "core/value.h"
#include "runtime/error.h"
#include "runtime/globals.h"
#include "developer/debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Value eval_slice(ASTNode *node) {
    DEBUG_INFO("eval_slice: entrada");
    if (node == NULL) error(0, "eval_slice: nodo es NULL");
    if (node->kind != NODE_SLICE) error(node->line, "eval_slice: nodo no es NODE_SLICE");

    if (node->data.slice.list == NULL) {
        error(current_eval_line, "eval_slice: el campo 'list' del nodo slice es NULL");
    }

    DEBUG_INFO("eval_slice: mode=%d, start=%d, end=%d, list node kind=%d",
               node->data.slice.mode, node->data.slice.start, node->data.slice.end,
               node->data.slice.list->kind);

    Value list = eval_expr(node->data.slice.list);
    DEBUG_INFO("eval_slice: lista evaluada, tipo=%d, count=%d", list.type, list.data.list.count);

    if (list.type != VAL_LIST)
        error(current_eval_line, "El slice solo se puede aplicar a listas (tipo %d)", list.type);

    int len = list.data.list.count;
    if (len == 0) {
        DEBUG_INFO("eval_slice: lista vacía, devolviendo lista vacía");
        value_free(&list);
        return val_list_empty();
    }

    if (list.data.list.items == NULL) {
        error(current_eval_line, "eval_slice: lista corrupta: items es NULL pero count=%d", len);
    }

    int mode = node->data.slice.mode;
    int start = node->data.slice.start;
    int end   = node->data.slice.end;

    Value result = val_list_empty();
    int line = get_node_line(node);
    if (line == 0) line = current_eval_line;

    switch (mode) {
        case 0: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            val_list_append(&result, copy_value_secure(list.data.list.items[start - 1]));
            break;
        }
        case 1: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            int real_end = (end > len) ? len : end;
            if (start > real_end) {
                break;
            }
            for (int i = start - 1; i < real_end; i++) {
                val_list_append(&result, copy_value_secure(list.data.list.items[i]));
            }
            break;
        }
        case 2: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            for (int i = start; i < len; i++) {
                val_list_append(&result, copy_value_secure(list.data.list.items[i]));
            }
            break;
        }
        case 3: {
            if (start != -1) {
                error(line, "Modo *start no implementado correctamente");
            }
            if (end < 1 || end > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            for (int i = 0; i < end - 1; i++) {
                val_list_append(&result, copy_value_secure(list.data.list.items[i]));
            }
            break;
        }
        case 4: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            for (int i = 0; i < len; i++) {
                if (i != start - 1)
                    val_list_append(&result, copy_value_secure(list.data.list.items[i]));
            }
            break;
        }
        case 5: {
            break;
        }
        default:
            error(line, "Modo de slice inválido: %d", mode);
    }

    DEBUG_INFO("eval_slice: resultado lista con %d elementos", result.data.list.count);
    value_free(&list);
    return result;
}

Value remove_slice(Value list, ASTNode *slice_node) {
    if (slice_node == NULL) error(0, "remove_slice: nodo slice es NULL");
    if (slice_node->kind != NODE_SLICE) error(slice_node->line, "remove_slice: nodo no es NODE_SLICE");

    if (list.type != VAL_LIST)
        error(current_eval_line, "La eliminación solo se puede aplicar a listas");

    int len = list.data.list.count;
    if (len == 0) {
        return val_list_empty();
    }

    if (list.data.list.items == NULL) {
        error(current_eval_line, "remove_slice: lista corrupta: items es NULL pero count=%d", len);
    }

    int mode = slice_node->data.slice.mode;
    int start = slice_node->data.slice.start;
    int end   = slice_node->data.slice.end;
    int line = get_node_line(slice_node);
    if (line == 0) line = current_eval_line;

    Value new_list = val_list_empty();

    switch (mode) {
        case 0: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            for (int i = 0; i < len; i++) {
                if (i == start - 1) continue;
                val_list_append(&new_list, copy_value_secure(list.data.list.items[i]));
            }
            break;
        }
        case 1: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            int real_end = (end > len) ? len : end;
            if (start > real_end) {
                for (int i = 0; i < len; i++)
                    val_list_append(&new_list, copy_value_secure(list.data.list.items[i]));
            } else {
                for (int i = 0; i < len; i++) {
                    if (i >= start - 1 && i <= real_end - 1) continue;
                    val_list_append(&new_list, copy_value_secure(list.data.list.items[i]));
                }
            }
            break;
        }
        case 2: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            for (int i = 0; i < start - 1; i++) {
                val_list_append(&new_list, copy_value_secure(list.data.list.items[i]));
            }
            break;
        }
        case 3: {
            if (start != -1) {
                error(line, "Modo *start no implementado correctamente para eliminación");
            }
            if (end < 1 || end > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            for (int i = end - 1; i < len; i++) {
                val_list_append(&new_list, copy_value_secure(list.data.list.items[i]));
            }
            break;
        }
        case 4: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            val_list_append(&new_list, copy_value_secure(list.data.list.items[start - 1]));
            break;
        }
        case 5: {
            break;
        }
        default:
            error(line, "Modo de slice inválido para eliminación: %d", mode);
    }

    return new_list;
}
