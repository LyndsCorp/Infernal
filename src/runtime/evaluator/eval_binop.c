/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/eval_binop.c
*/

#include "eval_binop.h"
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
#include <math.h>

Value eval_binop(ASTNode *expr) {
    const char *op_name = "?";
    switch (expr->data.binop.op) {
        case TOK_PLUS:  op_name = "+"; break;
        case TOK_MINUS: op_name = "-"; break;
        case TOK_STAR:  op_name = "*"; break;
        case TOK_SLASH: op_name = "/"; break;
        case TOK_PERCENT: op_name = "%"; break;
        case TOK_EEQ:   op_name = "=="; break;
        case TOK_NEQ:   op_name = "!="; break;
        case TOK_LT_OP: op_name = "<"; break;
        case TOK_GT_OP: op_name = ">"; break;
        case TOK_LE:    op_name = "<="; break;
        case TOK_GE:    op_name = ">="; break;
        case TOK_AND:   op_name = "&&"; break;
        case TOK_OR:    op_name = "||"; break;
        case TOK_POW:   op_name = "**"; break;
        default:        op_name = "desconocido";
    }
    DEBUG_INFO("NODE_BINOP: operador '%s'", op_name);

    Value left = eval_expr(expr->data.binop.left);
    DEBUG_INFO("Tipo de left (antes de resolver): %d", left.type);
    if (left.type == VAL_REFERENCE) {
        left = resolve_reference(left, expr->line);
        DEBUG_INFO("Resuelta referencia a lista, tipo: %d", left.type);
    }

    if (expr->data.binop.op == TOK_MINUS && left.type == VAL_LIST) {
        DEBUG_OP("=== ELIMINACIÓN DE LISTA DETECTADA ===");
        DEBUG_VAR("lista", left);
        ASTNode *right_node = expr->data.binop.right;
        DEBUG_INFO("Tipo del nodo derecho para eliminación: %d", right_node->kind);

        if (right_node->kind == NODE_SLICE) {
            Value result = remove_slice(left, right_node);
            return result;
        }

        int idx = -1;
        if (right_node->kind == NODE_INDEX) {
            idx = extract_integer_index(right_node, expr->line);
        } else if (right_node->kind == NODE_LIST && right_node->data.list_lit.count == 1) {
            idx = extract_integer_index(right_node->data.list_lit.items[0], expr->line);
        } else if (right_node->kind == NODE_LITERAL && right_node->data.lit.type == TOK_INT) {
            idx = right_node->data.lit.ival;
        } else {
            Value right_val = eval_expr(right_node);
            if (right_val.type == VAL_INT) idx = right_val.data.ival;
            else if (right_val.type == VAL_LIST && right_val.data.list.count == 1) {
                Value item = right_val.data.list.items[0];
                if (item.type == VAL_INT) idx = item.data.ival;
            }
        }

        if (idx != -1) {
            DEBUG_INFO("Índice extraído: %d", idx);
            Value new_list = val_list_empty();
            if (left.data.list.items == NULL && left.data.list.count > 0)
                error(expr->line, "Lista corrupta al eliminar elemento");
            for (int i = 0; i < left.data.list.count; i++) {
                if (i == idx - 1) continue;
                val_list_append(&new_list, copy_value_secure(left.data.list.items[i]));
            }
            DEBUG_VAR("lista resultado", new_list);
            return new_list;
        }

        error(expr->line, "No se puede eliminar de la lista con este tipo de especificación (nodo: %d)", right_node->kind);
    }

    if (left.type == VAL_LIST && expr->data.binop.op == TOK_PLUS &&
        expr->data.binop.right->kind == NODE_INDEX) {
        DEBUG_OP("=== INSERCIÓN EN LISTA DETECTADA ===");
    ASTNode *idx_node = expr->data.binop.right;
    Value base = eval_expr(idx_node->data.idx.list);
    Value index_val = eval_expr(idx_node->data.idx.index);

    int pos = -1;
    if (index_val.type == VAL_INT) {
        pos = index_val.data.ival;
    } else if (index_val.type == VAL_FLOAT) {
        double f = index_val.data.fval;
        if (f == (double)(int)f) {
            pos = (int)f;
        }
    }
    int len = left.data.list.count;
    if (pos < 1 || pos > len + 1) {
        pos = len + 1;
    }

    DEBUG_INFO("Insertando elemento en posición %d", pos);
    Value new_list = val_list_empty();
    for (int i = 0; i < left.data.list.count; i++) {
        val_list_append(&new_list, copy_value_secure(left.data.list.items[i]));
    }
    if (pos > new_list.data.list.count + 1) {
        pos = new_list.data.list.count + 1;
    }
    val_list_append(&new_list, val_make_null());
    for (int i = new_list.data.list.count - 1; i > pos - 1; i--) {
        new_list.data.list.items[i] = new_list.data.list.items[i - 1];
    }
    new_list.data.list.items[pos - 1] = copy_value_secure(base);
    DEBUG_VAR("nueva lista", new_list);
    DEBUG_INFO("Devolviendo lista (tipo %d)", new_list.type);
    return new_list;
        }

        if (left.type == VAL_LIST) {
            error(expr->line, "Operación no soportada con lista y operador '%s'", op_name);
        }

        Value right = eval_expr(expr->data.binop.right);
        DEBUG_INFO("Tipo de right: %d", right.type);

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
            expr->data.binop.op == TOK_LE || expr->data.binop.op == TOK_GE) {
            double lv = (left.type == VAL_INT) ? left.data.ival : (left.type == VAL_FLOAT) ? left.data.fval : 0.0;
        double rv = (right.type == VAL_INT) ? right.data.ival : (right.type == VAL_FLOAT) ? right.data.fval : 0.0;
        bool result = false;
        switch (expr->data.binop.op) {
            case TOK_LT_OP: result = (lv < rv); break;
            case TOK_GT_OP: result = (lv > rv); break;
            case TOK_LE:    result = (lv <= rv); break;
            case TOK_GE:    result = (lv >= rv); break;
            default: break;
        }
        return val_bool(result);
            }

            if (expr->data.binop.op == TOK_POW) {
                if (left.type == VAL_INT && right.type == VAL_INT && right.data.ival >= 0) {
                    long result = 1;
                    for (long i = 0; i < right.data.ival; i++) result *= left.data.ival;
                    return val_int((int)result);
                }
                double lv = (left.type == VAL_INT) ? left.data.ival : left.data.fval;
                double rv = (right.type == VAL_INT) ? right.data.ival : right.data.fval;
                return val_float(pow(lv, rv));
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
                Value result;
                switch (expr->data.binop.op) {
                    case TOK_PLUS:  result = val_int(lv + rv); break;
                    case TOK_MINUS: result = val_int(lv - rv); break;
                    case TOK_STAR:  result = val_int(lv * rv); break;
                    case TOK_SLASH: if (rv == 0) error(expr->line, "División por cero"); result = val_float((double)lv / rv); break;
                    case TOK_PERCENT: if (rv == 0) error(expr->line, "Módulo por cero"); result = val_int(lv % rv); break;
                    default: error(expr->line, "Operador no soportado");
                }
                return result;
            }

            double lv = (left.type == VAL_INT) ? left.data.ival :
            (left.type == VAL_FLOAT) ? left.data.fval :
            (left.type == VAL_BOOL) ? (left.data.bval ? 1.0 : 0.0) : 0.0;
            double rv = (right.type == VAL_INT) ? right.data.ival :
            (right.type == VAL_FLOAT) ? right.data.fval :
            (right.type == VAL_BOOL) ? (right.data.bval ? 1.0 : 0.0) : 0.0;
            Value result;
            switch (expr->data.binop.op) {
                case TOK_PLUS: result = val_float(lv + rv); break;
                case TOK_MINUS: result = val_float(lv - rv); break;
                case TOK_STAR: result = val_float(lv * rv); break;
                case TOK_SLASH: if (rv == 0) error(expr->line, "División por cero"); result = val_float(lv / rv); break;
                case TOK_PERCENT: if (rv == 0) error(expr->line, "Módulo por cero"); result = val_float((int)lv % (int)rv); break;
                default: error(expr->line, "Operador no soportado");
            }
            return result;
}
