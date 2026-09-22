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
#include <setjmp.h>

/* ====================================================================
 *  Helpers UTF-8 para manipulación cruda de strings
 * ====================================================================
 *
 * Infernal cuenta caracteres UTF-8, no bytes. Estas funciones replican
 * el comportamiento de stdlib/string.c para que -= y += operen sobre
 * caracteres humanos, no sobre bytes sueltos.
 */

/* Cuenta cuántos caracteres UTF-8 hay en una cadena. */
static size_t utf8_char_count(const char *s) {
    size_t n = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) s += 1;
        else if ((c & 0xE0) == 0xC0) s += 2;
        else if ((c & 0xF0) == 0xE0) s += 3;
        else if ((c & 0xF8) == 0xF0) s += 4;
        else s += 1;
        n++;
    }
    return n;
}

/* Avanza `n` caracteres UTF-8 y devuelve el puntero al byte correspondiente.
 * Si la cadena es más corta, devuelve el puntero al terminador nulo. */
static const char *utf8_advance(const char *s, size_t n) {
    while (*s && n > 0) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) s += 1;
        else if ((c & 0xE0) == 0xC0) s += 2;
        else if ((c & 0xF0) == 0xE0) s += 3;
        else if ((c & 0xF8) == 0xF0) s += 4;
        else s += 1;
        n--;
    }
    return s;
}

/* Traduce un NODE_SLICE de string a un rango [lo, hi] de caracteres
 * a eliminar (1-based, ambos inclusive). Si hi < lo, no se elimina nada.
 * Las conversiones son las mismas que en remove_slice() de eval_slice.c. */
static void slice_to_remove_range(ASTNode *slice_node, int total,
                                  int *lo, int *hi) {
    int mode  = slice_node->data.slice.mode;
    int start = slice_node->data.slice.start;
    int end   = slice_node->data.slice.end;

    switch (mode) {
        case 0: *lo = start;     *hi = start;   break;  /* [N]   */
        case 1: *lo = start;     *hi = end;     break;  /* [N:M] */
        case 2: *lo = start + 1; *hi = total;   break;  /* [N*]  */
        case 3: *lo = 1;         *hi = end - 1; break;  /* [*M]  */
        case 4: *lo = start;     *hi = total;   break;  /* [N**] */
        case 5: *lo = 1;         *hi = end;     break;  /* [**M] */
        case 6: *lo = 1;         *hi = total;   break;  /* [*]   */
        default: *lo = 1; *hi = 0; break;
    }
}

/* Elimina el rango [lo, hi] (1-based, inclusivo) de un string UTF-8.
 * Devuelve un buffer malloc'd con el resultado. */
static char *utf8_remove_range(const char *s, int lo, int hi, int line) {
    size_t total = utf8_char_count(s);
    if (lo < 1) lo = 1;
    if (hi > (int)total) hi = (int)total;

    if (lo > hi || lo > (int)total) {
        size_t len = strlen(s);
        char *dup = malloc(len + 1);
        if (!dup) error(line, "Memoria insuficiente al eliminar de string");
        memcpy(dup, s, len + 1);
        return dup;
    }

    const char *start_ptr = utf8_advance(s, (size_t)(lo - 1));
    const char *end_ptr   = utf8_advance(s, (size_t)hi);

    size_t prefix_len = (size_t)(start_ptr - s);
    size_t suffix_len = strlen(end_ptr);

    char *out = malloc(prefix_len + suffix_len + 1);
    if (!out) error(line, "Memoria insuficiente al eliminar de string");

    memcpy(out, s, prefix_len);
    memcpy(out + prefix_len, end_ptr, suffix_len);
    out[prefix_len + suffix_len] = '\0';
    return out;
}

/* Inserta `to_insert` (debe tener exactamente 1 carácter UTF-8) en la
 * posición `pos` (1-based) del string `s`. Devuelve un buffer malloc'd. */
static char *utf8_insert_at(const char *s, const char *to_insert,
                            int pos, int line) {
    size_t total = utf8_char_count(s);
    if (pos < 1) pos = 1;
    if (pos > (int)total + 1) pos = (int)total + 1;

    const char *insert_ptr = utf8_advance(s, (size_t)(pos - 1));
    size_t prefix_len = (size_t)(insert_ptr - s);
    size_t ins_len    = strlen(to_insert);
    size_t suffix_len = strlen(insert_ptr);

    char *out = malloc(prefix_len + ins_len + suffix_len + 1);
    if (!out) error(line, "Memoria insuficiente al insertar en string");

    memcpy(out, s, prefix_len);
    memcpy(out + prefix_len, to_insert, ins_len);
    memcpy(out + prefix_len + ins_len, insert_ptr, suffix_len);
    out[prefix_len + ins_len + suffix_len] = '\0';
    return out;
}

/* ====================================================================
 *  eval_binop
 * ==================================================================== */

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

    /* =================================================================
     *  ELIMINACIÓN DE ELEMENTOS EN LISTAS
     * ================================================================= */
    if (expr->data.binop.op == TOK_MINUS && left.type == VAL_LIST) {
        DEBUG_OP("=== ELIMINACIÓN DE LISTA DETECTADA ===");
        DEBUG_VAR("lista", left);
        ASTNode *right_node = expr->data.binop.right;
        DEBUG_INFO("Tipo del nodo derecho para eliminación: %d", right_node->kind);

        if (right_node->kind == NODE_SLICE) {
            Value result = remove_slice(left, right_node);
            value_free(&left);
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
            value_free(&right_val);
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
            value_free(&left);
            return new_list;
        }

        error(expr->line, "No se puede eliminar de la lista con este tipo de especificación (nodo: %d)", right_node->kind);
    }

    /* =================================================================
     *  INSERCIÓN EN LISTAS (lista + elemento[i])
     * ================================================================= */
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
            } else {
                value_free(&base);
                value_free(&index_val);
                value_free(&left);
                error(expr->line,
                  "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            }
        } else {
            value_free(&base);
            value_free(&index_val);
            value_free(&left);
            error(expr->line,
                "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
        }

    /* Cero y negativos: error explícito. Los índices que exceden la
     * longitud de la lista siguen acomodándose al final, como antes. */
    if (pos < 1) {
        value_free(&base);
        value_free(&index_val);
        value_free(&left);
        error(expr->line,
              "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
    }

    int len = left.data.list.count;
    if (pos > len + 1) {
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
        value_free(&base);
        value_free(&index_val);
        value_free(&left);
        DEBUG_VAR("nueva lista", new_list);
        DEBUG_INFO("Devolviendo lista (tipo %d)", new_list.type);
        return new_list;
    }

    /* =================================================================
     *  APPEND A LISTAS (lista + valor)
     * ================================================================= */
    if (left.type == VAL_LIST && expr->data.binop.op == TOK_PLUS) {
        Value right = eval_expr(expr->data.binop.right);
        if (right.type == VAL_REFERENCE)
            right = resolve_reference(right, expr->line);

        Value new_list = val_list_empty();
        for (int i = 0; i < left.data.list.count; i++) {
            val_list_append(&new_list, copy_value_secure(left.data.list.items[i]));
        }
        val_list_append(&new_list, right);   /* transferimos la propiedad de `right` */
        value_free(&left);
        return new_list;
    }

    if (left.type == VAL_LIST) {
        error(expr->line, "Operación no soportada con lista y operador '%s'", op_name);
    }

    /* =================================================================
     *  MANIPULACIÓN CRUDA DE STRINGS
     * =================================================================
     *
     * Los strings de Infernal son como listas de caracteres UTF-8 con
     * base 1. Se aplican las mismas operaciones que a las listas:
     *
     *     texto -= [2]         elimina el carácter 2
     *     texto -= [2:4]       elimina los caracteres 2..4
     *     texto -= [*]         elimina todo
     *
     *     texto += "c"[2]      inserta "c" en la posición 2
     *     texto + "c"[2]       idem
     *
     * El valor a insertar debe ser un string de exactamente 1 carácter.
     */

    /* --- Eliminación de caracteres en strings --- */
    if (expr->data.binop.op == TOK_MINUS && left.type == VAL_STRING) {
        ASTNode *right_node = expr->data.binop.right;

        /* Rango/slice: texto -= [2:4], [*], [2*], etc. */
        if (right_node->kind == NODE_SLICE) {
            int total = (int)utf8_char_count(left.data.sval);
            int lo, hi;
            slice_to_remove_range(right_node, total, &lo, &hi);
            char *new_str = utf8_remove_range(left.data.sval, lo, hi, expr->line);
            Value result = val_string(new_str);
            free(new_str);
            value_free(&left);
            return result;
        }

        /* Índice único: texto -= [2]  o  texto -= [i]
         *
         * Dependiendo de si dentro de los corchetes hay un número literal,
         * una variable o una expresión, el parser produce NODE_SLICE,
         * NODE_LIST con un elemento, NODE_INDEX o NODE_LITERAL. Cubrimos
         * los casos comunes. */
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
            value_free(&right_val);
        }

        if (idx == -1) {
            value_free(&left);
            error(expr->line,
                  "No se puede eliminar de un string con este tipo de especificación "
                  "(nodo: %d)", right_node->kind);
        }

        char *new_str = utf8_remove_range(left.data.sval, idx, idx, expr->line);
        Value result = val_string(new_str);
        free(new_str);
        value_free(&left);
        return result;
    }

    /* --- Inserción de caracteres en strings ---
     *
     * texto + "c"[2]      inserta "c" en la posición 2
     * texto += "c"[2]     idem
     *
     * El lado derecho debe ser NODE_INDEX (el parser lo produce tanto si
     * el valor a insertar es un literal como si es una variable o una
     * expresión). El índice es la posición donde se inserta. */
    if (left.type == VAL_STRING && expr->data.binop.op == TOK_PLUS &&
        expr->data.binop.right->kind == NODE_INDEX) {

        ASTNode *idx_node = expr->data.binop.right;

        Value base = eval_expr(idx_node->data.idx.list);
        if (base.type == VAL_REFERENCE)
            base = resolve_reference(base, expr->line);

        Value index_val = eval_expr(idx_node->data.idx.index);
        if (index_val.type == VAL_REFERENCE)
            index_val = resolve_reference(index_val, expr->line);

        /* El valor a insertar debe ser un string de exactamente 1 carácter. */
        if (base.type != VAL_STRING) {
            value_free(&base);
            value_free(&index_val);
            value_free(&left);
            error(expr->line,
                  "Solo se pueden insertar strings de 1 carácter en un string "
                  "(valor recibido de tipo '%s')",
                  value_type_name(base.type));
        }

        size_t ins_chars = utf8_char_count(base.data.sval);
        if (ins_chars != 1) {
            value_free(&base);
            value_free(&index_val);
            value_free(&left);
            error(expr->line,
                  "No se puede insertar en un string un valor de %zu carácter(es): "
                  "solo se admite 1 carácter a la vez",
                  ins_chars);
        }

        /* La posición debe ser un entero positivo. */
        int pos = -1;
        if (index_val.type == VAL_INT) {
            pos = index_val.data.ival;
        } else if (index_val.type == VAL_FLOAT) {
            double f = index_val.data.fval;
            if (f == (double)(int)f) pos = (int)f;
        }

        if (pos < 1) {
            value_free(&base);
            value_free(&index_val);
            value_free(&left);
            error(expr->line,
                  "La posición de inserción debe ser un entero positivo");
        }

        char *new_str = utf8_insert_at(left.data.sval, base.data.sval, pos, expr->line);
        Value result = val_string(new_str);
        free(new_str);
        value_free(&base);
        value_free(&index_val);
        value_free(&left);
        return result;
    }

    /* =================================================================
     *  RESTO DE OPERADORES (AND, OR, comparaciones, POW, concat, aritmética)
     * ================================================================= */

    jmp_buf saved_env;
    memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
    int saved_raised = exception_raised;
    if (setjmp(exception_env) != 0) {
        value_free(&left);
        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
        exception_raised = saved_raised;
        longjmp(exception_env, 1);
    }

    Value right = eval_expr(expr->data.binop.right);
    if (right.type == VAL_REFERENCE)
        right = resolve_reference(right, expr->line);
    memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
    exception_raised = saved_raised;
    DEBUG_INFO("Tipo de right: %d", right.type);

    if (expr->data.binop.op == TOK_AND || expr->data.binop.op == TOK_OR) {
        if (left.type != VAL_BOOL || right.type != VAL_BOOL) {
            value_free(&left);
            value_free(&right);
            error(expr->line, "Los operadores lógicos 'and'/'or' requieren valores bool");
        }

        bool result = (expr->data.binop.op == TOK_AND)
        ? (left.data.bval && right.data.bval)
        : (left.data.bval || right.data.bval);
        Value result_value = val_bool(result);
        value_free(&left);
        value_free(&right);
        return result_value;
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
        Value result = val_bool(expr->data.binop.op == TOK_EEQ ? equal : !equal);
        value_free(&left);
        value_free(&right);
        return result;
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
        Value result_value = val_bool(result);
        value_free(&left);
        value_free(&right);
        return result_value;
    }

    if (expr->data.binop.op == TOK_POW) {
        if (left.type == VAL_INT && right.type == VAL_INT && right.data.ival >= 0) {
            long result = 1;
            for (long i = 0; i < right.data.ival; i++) result *= left.data.ival;
            Value result_value = val_int((int)result);
            value_free(&left);
            value_free(&right);
            return result_value;
        }
        double lv = (left.type == VAL_INT) ? left.data.ival : left.data.fval;
        double rv = (right.type == VAL_INT) ? right.data.ival : right.data.fval;
        Value result_value = val_float(pow(lv, rv));
        value_free(&left);
        value_free(&right);
        return result_value;
    }

    if (left.type == VAL_STRING || right.type == VAL_STRING) {
        char lbuf[64], rbuf[64];
        const char *ls = left.type == VAL_STRING ? left.data.sval : lbuf;
        const char *rs = right.type == VAL_STRING ? right.data.sval : rbuf;
        if (left.type != VAL_STRING) {
            if (left.type == VAL_INT)
                snprintf(lbuf, sizeof(lbuf), "%d", left.data.ival);
            else if (left.type == VAL_FLOAT)
                snprintf(lbuf, sizeof(lbuf), "%.15g", left.data.fval);
            else if (left.type == VAL_BOOL)
                snprintf(lbuf, sizeof(lbuf), "%s", left.data.bval ? "true" : "false");
            else if (left.type == VAL_NULL)
                snprintf(lbuf, sizeof(lbuf), "null");
            else
                error(expr->line, "No se puede concatenar un valor de tipo %s con un string",
                      value_type_name(left.type));
        }
        if (right.type != VAL_STRING) {
            if (right.type == VAL_INT)
                snprintf(rbuf, sizeof(rbuf), "%d", right.data.ival);
            else if (right.type == VAL_FLOAT)
                snprintf(rbuf, sizeof(rbuf), "%.15g", right.data.fval);
            else if (right.type == VAL_BOOL)
                snprintf(rbuf, sizeof(rbuf), "%s", right.data.bval ? "true" : "false");
            else if (right.type == VAL_NULL)
                snprintf(rbuf, sizeof(rbuf), "null");
            else
                error(expr->line, "No se puede concatenar un valor de tipo %s con un string",
                      value_type_name(right.type));
        }
        size_t total = strlen(ls) + strlen(rs) + 1;
        char *buf = malloc(total);
        if (!buf) error(expr->line, "Memoria insuficiente al concatenar cadenas");
        snprintf(buf, total, "%s%s", ls, rs);
        Value result = val_string(buf);
        free(buf);
        value_free(&left);
        value_free(&right);
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
        value_free(&left);
        value_free(&right);
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
    value_free(&left);
    value_free(&right);
    return result;
}
