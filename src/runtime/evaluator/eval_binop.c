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
#include <stdint.h>
#include <limits.h>
#include <errno.h>
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
        const unsigned char *p = (const unsigned char *)s;
        size_t width = 1;
        if (*p < 0x80) {
            width = 1;
        } else if ((*p & 0xE0) == 0xC0 && p[1] &&
                   (p[1] & 0xC0) == 0x80) {
            width = 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2] &&
                   (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            width = 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3] &&
                   (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
                   (p[3] & 0xC0) == 0x80) {
            width = 4;
        }
        s += width;
        n++;
    }
    return n;
}

/* Avanza `n` caracteres UTF-8 y devuelve el puntero al byte correspondiente.
 * Si la cadena es más corta, devuelve el puntero al terminador nulo. */
static const char *utf8_advance(const char *s, size_t n) {
    while (*s && n > 0) {
        const unsigned char *p = (const unsigned char *)s;
        size_t width = 1;
        if (*p < 0x80) {
            width = 1;
        } else if ((*p & 0xE0) == 0xC0 && p[1] &&
                   (p[1] & 0xC0) == 0x80) {
            width = 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2] &&
                   (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            width = 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3] &&
                   (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
                   (p[3] & 0xC0) == 0x80) {
            width = 4;
        }
        s += width;
        n--;
    }
    return s;
}

static bool value_is_numeric(Value v) {
    return v.type == VAL_INT || v.type == VAL_UINT || v.type == VAL_FLOAT || v.type == VAL_BOOL;
}

/* Aísla el punto de recuperación en un helper para que eval_binop no tenga
 * que preservar todos sus registros en el caso común. Los literales se
 * evalúan sin protección: su evaluación no puede lanzar errores del lenguaje. */
static Value eval_binop_right_protected(ASTNode *right_node, int line,
                                        Value *left) {
    jmp_buf saved_env;
    memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
    int saved_raised = exception_raised;
    if (setjmp(exception_env) != 0) {
        value_free(left);
        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
        exception_raised = saved_raised;
        longjmp(exception_env, 1);
    }

    Value right = eval_expr(right_node);
    if (right.type == VAL_REFERENCE)
        right = resolve_reference(right, line);
    memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
    exception_raised = saved_raised;
    return right;
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
 *  Extracción de clave para eliminación de mapas
 * ====================================================================
 *
 * La forma `mapa - [clave]` necesita convertir el operando derecho en
 * una clave string. Acepta:
 *
 *     ["clave"]   → string literal
 *     [clave]     → si `clave` es una variable string, su valor;
 *                   si no está definida, el identificador literal
 *                   "clave" (útil para trabajar con mapas sin declarar
 *                   una variable por cada clave)
 *     "clave"     → string literal directo
 *
 * Devuelve un buffer malloc'd (liberar con free) o NULL si no se puede
 * obtener una clave string. */
static char *extract_map_key(ASTNode *node) {
    if (!node) return NULL;

    if (node->kind == NODE_LIST && node->data.list_lit.count == 1) {
        ASTNode *item = node->data.list_lit.items[0];

        if (item->kind == NODE_LITERAL && item->data.lit.type == TOK_STRING) {
            return item->data.lit.payload.sval ? strdup(item->data.lit.payload.sval) : NULL;
        }

        if (item->kind == NODE_VAR && !item->data.var.clone) {
            const char *name = item->data.var.name;
            if (name[0] == '$') name++;
            VarEntry *e = scope_find(current_scope, name);
            if (!e) {
                /* Identificador sin definir: se trata como clave literal. */
                return strdup(name);
            }
            if (e->value.type == VAL_STRING && e->value.data.sval) {
                return strdup(e->value.data.sval);
            }
            return NULL;
        }

        Value v = eval_expr(item);
        if (v.type == VAL_STRING && v.data.sval) {
            char *r = strdup(v.data.sval);
            value_free(&v);
            return r;
        }
        value_free(&v);
        return NULL;
    }

    if (node->kind == NODE_LITERAL && node->data.lit.type == TOK_STRING) {
        return node->data.lit.payload.sval ? strdup(node->data.lit.payload.sval) : NULL;
    }

    Value v = eval_expr(node);
    if (v.type == VAL_STRING && v.data.sval) {
        char *r = strdup(v.data.sval);
        value_free(&v);
        return r;
    }
    value_free(&v);
    return NULL;
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

    /* NUEVO: void no es un operando válido. */
    if (left.type == VAL_VOID) {
        error(expr->line,
              "El operando izquierdo de '%s' no tiene valor (void): "
              "una función o expresión no devolvió nada.",
              op_name);
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
        } else if (right_node->kind == NODE_LITERAL && right_node->data.lit.type == TOK_UINT &&
                   right_node->data.lit.uval <= INT_MAX) {
            idx = (int)right_node->data.lit.uval;
        } else {
            Value right_val = eval_expr(right_node);
            if (right_val.type == VAL_INT) idx = right_val.data.ival;
            else if (right_val.type == VAL_UINT && right_val.data.uval <= INT_MAX) idx = (int)right_val.data.uval;
            else if (right_val.type == VAL_LIST && right_val.data.list.count == 1) {
                Value item = right_val.data.list.items[0];
                if (item.type == VAL_INT) idx = item.data.ival;
                else if (item.type == VAL_UINT && item.data.uval <= INT_MAX) idx = (int)item.data.uval;
            }
            value_free(&right_val);
        }

        if (idx != -1) {
            DEBUG_INFO("Índice extraído: %d", idx);
            if (idx < 1 || idx > left.data.list.count) {
                value_free(&left);
                error(expr->line,
                      "Índice fuera de rango: %d. La lista tiene %d elemento(s).",
                      idx, left.data.list.count);
            }
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

        value_free(&left);
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

        if (base.type == VAL_VOID) {
            value_free(&base);
            value_free(&index_val);
            value_free(&left);
            error(expr->line,
                  "El valor a insertar en la lista no tiene valor (void): "
                  "una función o expresión no devolvió nada.");
        }

        int pos = -1;

        if (index_val.type == VAL_INT) {
            pos = index_val.data.ival;
        } else if (index_val.type == VAL_UINT && index_val.data.uval <= INT_MAX) {
            pos = (int)index_val.data.uval;
        } else if (index_val.type == VAL_FLOAT) {
            double f = index_val.data.fval;
            if (isfinite(f) && f >= 1.0 && f <= (double)INT_MAX && trunc(f) == f) {
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

        if (right.type == VAL_VOID) {
            value_free(&right);
            value_free(&left);
            error(expr->line,
                  "El operando derecho de '+' no tiene valor (void): "
                  "una función o expresión no devolvió nada.");
        }

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
     *  ELIMINACIÓN DE CLAVES EN MAPAS
     * =================================================================
     *
     * Un `map` es una caja de variables: cada clave es como una
     * variable con tipo fijo. Para "cambiarle el tipo" a una clave
     * hay que eliminar el par y volver a asignarlo; `mapa -= [clave]`
     * es la forma natural de hacerlo.
     *
     *   $mapa -= ["web"]
     *   $mapa -= [web]        (web variable string, o clave literal "web")
     */
    if (expr->data.binop.op == TOK_MINUS && left.type == VAL_MAP) {
        jmp_buf map_saved_env;
        memcpy(&map_saved_env, &exception_env, sizeof(jmp_buf));
        int map_saved_raised = exception_raised;
        if (setjmp(exception_env) != 0) {
            value_free(&left);
            memcpy(&exception_env, &map_saved_env, sizeof(jmp_buf));
            exception_raised = map_saved_raised;
            longjmp(exception_env, 1);
        }

        char *key = extract_map_key(expr->data.binop.right);
        if (!key) {
            value_free(&left);
            memcpy(&exception_env, &map_saved_env, sizeof(jmp_buf));
            exception_raised = map_saved_raised;
            error(expr->line,
                  "No se puede eliminar de un mapa: se esperaba una clave string.\n"
                  "    Ejemplos válidos:\n"
                  "        $mapa -= [\"clave\"]   (elimina la clave literal 'clave')\n"
                  "        $mapa -= [$var]         (elimina la clave cuyo nombre está en $var)\n"
                  "        $mapa -= [clave]        (elimina 'clave'; si 'clave' es una variable\n"
                  "                                 string, se usa su valor)");
        }

        Value new_map = val_map_copy(&left);
        val_map_delete(&new_map, key);
        free(key);
        value_free(&left);

        memcpy(&exception_env, &map_saved_env, sizeof(jmp_buf));
        exception_raised = map_saved_raised;
        return new_map;
    }

    /* Un `map` no admite otras operaciones aritméticas. Sin este check,
     * `mapa + 5` caería en la aritmética general y devolvería 5.0 de
     * forma silenciosa. */
    if (left.type == VAL_MAP) {
        value_free(&left);
        error(expr->line,
              "Operación no soportada con mapa y operador '%s'", op_name);
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
            size_t total_sz = utf8_char_count(left.data.sval);
            if (total_sz > (size_t)INT_MAX) {
                value_free(&left);
                error(expr->line,
                      "El string es demasiado largo para aplicar slices");
            }
            int total = (int)total_sz;
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
        } else if (right_node->kind == NODE_LITERAL && right_node->data.lit.type == TOK_UINT &&
                   right_node->data.lit.uval <= INT_MAX) {
            idx = (int)right_node->data.lit.uval;
        } else {
            Value right_val = eval_expr(right_node);
            if (right_val.type == VAL_INT) idx = right_val.data.ival;
            else if (right_val.type == VAL_UINT && right_val.data.uval <= INT_MAX) idx = (int)right_val.data.uval;
            else if (right_val.type == VAL_LIST && right_val.data.list.count == 1) {
                Value item = right_val.data.list.items[0];
                if (item.type == VAL_INT) idx = item.data.ival;
                else if (item.type == VAL_UINT && item.data.uval <= INT_MAX) idx = (int)item.data.uval;
            }
            value_free(&right_val);
        }

        if (idx == -1) {
            value_free(&left);
            error(expr->line,
                  "No se puede eliminar de un string con este tipo de especificación "
                  "(nodo: %d)", right_node->kind);
        }

        {
            size_t total_sz = utf8_char_count(left.data.sval);
            if (idx < 1 || (size_t)idx > total_sz) {
                value_free(&left);
                error(expr->line,
                      "Índice fuera de rango: %d. El string tiene %zu carácter(es).",
                      idx, total_sz);
            }
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
     * SOLO es inserción de carácter cuando el contenedor que se indexa
     * es él mismo un string:
     *
     *     $texto + "x"[2]         →  base es string → inserta 'x' en pos 2
     *     $texto + $letra[1]      →  base es string → inserta el carácter
     *
     * Si el contenedor es una lista o un mapa, NO es inserción, es
     * concatenación normal. El valor indexado se evalúa y se concatena
     * como cualquier otro operando derecho:
     *
     *     $texto + otro[2]        →  base es list  → concatena otro[2]
     *     $texto + $mapa["k"]     →  base es map   → concatena mapa["k"]
     *
     * Así Infernal sabe de dónde viene el valor y no confunde un string
     * de una lista (que puede tener cualquier longitud) con un carácter
     * de un string. */
    if (left.type == VAL_STRING && expr->data.binop.op == TOK_PLUS &&
        expr->data.binop.right->kind == NODE_INDEX) {

        ASTNode *idx_node = expr->data.binop.right;

        Value base = eval_expr(idx_node->data.idx.list);
        if (base.type == VAL_REFERENCE)
            base = resolve_reference(base, expr->line);

        /* Si el contenedor indexado NO es un string, no es inserción:
         * liberamos `base` y dejamos caer el flujo al bloque general de
         * concatenación, que evaluará `expr->data.binop.right` (el
         * NODE_INDEX completo) como operando derecho. */
        if (base.type != VAL_STRING) {
            value_free(&base);
        } else {
            Value index_val = eval_expr(idx_node->data.idx.index);
            if (index_val.type == VAL_REFERENCE)
                index_val = resolve_reference(index_val, expr->line);

            /* El valor a insertar debe ser un string de exactamente 1 carácter. */
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
            } else if (index_val.type == VAL_UINT && index_val.data.uval <= INT_MAX) {
                pos = (int)index_val.data.uval;
            } else if (index_val.type == VAL_FLOAT) {
                double f = index_val.data.fval;
                if (isfinite(f) && f >= 1.0 && f <= (double)INT_MAX && trunc(f) == f)
                    pos = (int)f;
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
        }

    /* =================================================================
     *  RESTO DE OPERADORES (AND, OR, comparaciones, POW, concat, aritmética)
     * ================================================================= */

    /* and/or son de cortocircuito: si el lado izquierdo ya determina el
     * resultado, no se evalúa el derecho. Además evita ejecutar efectos
     * secundarios o lanzar errores innecesarios en la rama inalcanzable. */
    if (expr->data.binop.op == TOK_AND || expr->data.binop.op == TOK_OR) {
        if (left.type != VAL_BOOL) {
            value_free(&left);
            error(expr->line, "Los operadores lógicos 'and'/'or' requieren valores bool");
        }
        bool short_circuit =
            (expr->data.binop.op == TOK_AND && !left.data.bval) ||
            (expr->data.binop.op == TOK_OR && left.data.bval);
        if (short_circuit) {
            bool result = left.data.bval;
            value_free(&left);
            return val_bool(result);
        }
    }

    ASTNode *right_node = expr->data.binop.right;
    Value right;
    if (right_node->kind == NODE_LITERAL) {
        right = eval_expr(right_node);
    } else {
        right = eval_binop_right_protected(right_node, expr->line, &left);
    }
    DEBUG_INFO("Tipo de right: %d", right.type);

    /* NUEVO: void no es un operando válido. */
    if (right.type == VAL_VOID) {
        value_free(&left);
        error(expr->line,
              "El operando derecho de '%s' no tiene valor (void): "
              "una función o expresión no devolvió nada.",
              op_name);
    }

    if (expr->data.binop.op == TOK_AND || expr->data.binop.op == TOK_OR) {
        if (right.type != VAL_BOOL) {
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
                case VAL_INT: case VAL_UINT:
                    equal = value_integer_compare(left, right) == 0;
                    break;
                case VAL_FLOAT:  equal = (left.data.fval == right.data.fval); break;
                case VAL_STRING: equal = (strcmp(left.data.sval, right.data.sval) == 0); break;
                default: equal = false;
            }
        }
        /* int vs float: comparación numérica (como Python). */
        else if (left.type == VAL_INT && right.type == VAL_FLOAT) {
            equal = (value_integer_to_double(left) == right.data.fval);
        }
        else if (left.type == VAL_FLOAT && right.type == VAL_INT) {
            equal = (left.data.fval == value_integer_to_double(right));
        }
        else if (left.type == VAL_UINT && right.type == VAL_INT) {
            equal = value_integer_compare(left, right) == 0;
        }
        else if (left.type == VAL_INT && right.type == VAL_UINT) {
            equal = value_integer_compare(left, right) == 0;
        }
        else if (left.type == VAL_UINT && right.type == VAL_FLOAT) {
            equal = (value_integer_to_double(left) == right.data.fval);
        }
        else if (left.type == VAL_FLOAT && right.type == VAL_UINT) {
            equal = (left.data.fval == value_integer_to_double(right));
        }
        Value result = val_bool(expr->data.binop.op == TOK_EEQ ? equal : !equal);
        value_free(&left);
        value_free(&right);
        return result;
    }

    if (expr->data.binop.op == TOK_LT_OP || expr->data.binop.op == TOK_GT_OP ||
        expr->data.binop.op == TOK_LE || expr->data.binop.op == TOK_GE) {

        /* --- Conversión automática string → número -----------------------
         *
         * Si un operando es un string y el otro es numérico (int, uint,
         * float o bool), intentamos convertir el string al tipo del operando
         * numérico. Esto permite escribir cosas como:
         *
         *     local n = input("> ")       # input() devuelve string
         *     if n < 3 then ...           # "3" se convierte a int
         *
         * sin tener que anotar el tipo a mano en cada comparación. Es el
         * mismo criterio que ya se aplica en asignaciones tipadas
         * (`int n = input("> ")`) y en el switch.
         *
         * Si el string no representa un número válido, damos un mensaje
         * que explica el problema y las alternativas. El caso "otro lado
         * es int pero el string tiene decimales" se resuelve probando
         * float como alternativa razonable, para no ser más estrictos
         * aquí que en cualquier otra operación numérica. */
        if (left.type == VAL_STRING && value_is_numeric(right)) {
            int target = (right.type == VAL_FLOAT) ? TOK_FLOAT :
                         (right.type == VAL_UINT) ? TOK_UINT : TOK_INT;
            if (!try_convert_value(&left, target)) {
                if (target == TOK_INT && try_convert_value(&left, TOK_FLOAT)) {
                    /* El string tenía decimales; comparamos como float. */
                } else {
                    char bad[256];
                    snprintf(bad, sizeof(bad), "%.200s",
                             left.data.sval ? left.data.sval : "");
                    value_free(&left);
                    value_free(&right);
                    error(expr->line,
                          "No se puede comparar un string con un número: el string \"%s\" "
                          "no representa un valor numérico.\n"
                          "    Para comparar el texto como tal, usa '==' o '!=' con comillas.\n"
                          "    Para comparar como número, escribe un valor que se pueda interpretar\n"
                          "    como int o float, por ejemplo \"3\", \"3.14\" o \"-1\".\n"
                          "    Si querías comparar el número como texto, conviértelo con:\n"
                          "        \"\" + $numero",
                          bad);
                }
            }
        } else if (right.type == VAL_STRING && value_is_numeric(left)) {
            int target = (left.type == VAL_FLOAT) ? TOK_FLOAT :
                         (left.type == VAL_UINT) ? TOK_UINT : TOK_INT;
            if (!try_convert_value(&right, target)) {
                if (target == TOK_INT && try_convert_value(&right, TOK_FLOAT)) {
                    /* El string tenía decimales; comparamos como float. */
                } else {
                    char bad[256];
                    snprintf(bad, sizeof(bad), "%.200s",
                             right.data.sval ? right.data.sval : "");
                    value_free(&left);
                    value_free(&right);
                    error(expr->line,
                          "No se puede comparar un número con un string: el string \"%s\" "
                          "no representa un valor numérico.\n"
                          "    Para comparar el texto como tal, usa '==' o '!=' con comillas.\n"
                          "    Para comparar como número, escribe un valor que se pueda interpretar\n"
                          "    como int o float, por ejemplo \"3\", \"3.14\" o \"-1\".\n"
                          "    Si querías comparar el número como texto, conviértelo con:\n"
                          "        \"\" + $numero",
                          bad);
                }
            }
        }

        if (!value_is_numeric(left) || !value_is_numeric(right)) {
            const char *lt = value_type_name(left.type);
            const char *rt = value_type_name(right.type);
            value_free(&left);
            value_free(&right);
            error(expr->line, "Los operadores '<', '>', '<=' y '>=' requieren que valores sean numéricos como int o float (se recibió %s y %s)",
                  lt, rt);
        }
        bool integer_comparison = (left.type == VAL_INT || left.type == VAL_UINT) &&
                                  (right.type == VAL_INT || right.type == VAL_UINT);
        int integer_order = 0;
        if (integer_comparison) {
            integer_order = value_integer_compare(left, right);
        }
        double lv = (left.type == VAL_INT || left.type == VAL_UINT) ? value_integer_to_double(left)
        : (left.type == VAL_FLOAT) ? left.data.fval
        : (left.data.bval ? 1.0 : 0.0);
        double rv = (right.type == VAL_INT || right.type == VAL_UINT) ? value_integer_to_double(right)
        : (right.type == VAL_FLOAT) ? right.data.fval
        : (right.data.bval ? 1.0 : 0.0);
        bool result = false;
        switch (expr->data.binop.op) {
            case TOK_LT_OP: result = integer_comparison ? integer_order < 0 : lv < rv; break;
            case TOK_GT_OP: result = integer_comparison ? integer_order > 0 : lv > rv; break;
            case TOK_LE:    result = integer_comparison ? integer_order <= 0 : lv <= rv; break;
            case TOK_GE:    result = integer_comparison ? integer_order >= 0 : lv >= rv; break;
            default: break;
        }
        Value result_value = val_bool(result);
        value_free(&left);
        value_free(&right);
        return result_value;
        }

    if (expr->data.binop.op == TOK_POW) {
        if (!value_is_numeric(left) || !value_is_numeric(right)) {
            const char *lt = value_type_name(left.type);
            const char *rt = value_type_name(right.type);
            value_free(&left);
            value_free(&right);
            error(expr->line,
                  "El operador '**' requiere operandos numéricos (int, uint, float o bool); "
                  "se recibió %s y %s", lt, rt);
        }
        if ((left.type == VAL_INT || left.type == VAL_UINT) &&
            (right.type == VAL_INT || right.type == VAL_UINT)) {
            InfernalBigInt exponent_value = value_as_bigint(right);
            uint64_t exponent = 0;
            InfernalBigInt powered;
            bool unsigned_power = left.type == VAL_UINT;
            if (exponent_value.negative || !bigint_to_u64(exponent_value, &exponent) ||
                exponent > UINT32_MAX ||
                !bigint_pow(value_as_bigint(left), (uint32_t)exponent, &powered)) {
                value_free(&left); value_free(&right);
                error(expr->line, "Desbordamiento o exponente no válido en '**' (máximo: 64 bytes)");
            }
            if (unsigned_power && powered.negative) {
                value_free(&left); value_free(&right);
                error(expr->line, "Un valor uint no puede ser negativo");
            }
            Value result = val_bigint(powered, unsigned_power);
            value_free(&left); value_free(&right);
            return result;
        }
        double lv = (left.type == VAL_INT || left.type == VAL_UINT) ? value_integer_to_double(left) : left.data.fval;
        double rv = (right.type == VAL_INT || right.type == VAL_UINT) ? value_integer_to_double(right) : right.data.fval;
        errno = 0;
        double powered = pow(lv, rv);
        if (!isfinite(powered) && isfinite(lv) && isfinite(rv)) {
            value_free(&left);
            value_free(&right);
            error(expr->line, "Resultado no finito en '**'");
        }
        Value result_value = val_float(powered);
        value_free(&left);
        value_free(&right);
        return result_value;
    }

    if (left.type == VAL_STRING || right.type == VAL_STRING) {
        if (expr->data.binop.op != TOK_PLUS) {
            const char *lt = value_type_name(left.type);
            const char *rt = value_type_name(right.type);
            value_free(&left);
            value_free(&right);
            error(expr->line,
                  "El operador '%s' no se puede aplicar entre %s y %s.\n"
                  "    Solo '+' concatena cuando hay un string; para el resto, "
                  "convierte explícitamente ambos lados a número.",
                  op_name, lt, rt);
        }
        char lbuf[160], rbuf[160];
        const char *ls = left.type == VAL_STRING ? left.data.sval : lbuf;
        const char *rs = right.type == VAL_STRING ? right.data.sval : rbuf;
        if (left.type != VAL_STRING) {
            if (left.type == VAL_INT || left.type == VAL_UINT) {
                char *number = bigint_to_string(value_as_bigint(left));
                snprintf(lbuf, sizeof(lbuf), "%s", number ? number : "0");
                free(number);
            }
            else if (left.type == VAL_FLOAT)
                snprintf(lbuf, sizeof(lbuf), "%.15g", left.data.fval);
            else if (left.type == VAL_BOOL)
                snprintf(lbuf, sizeof(lbuf), "%s", left.data.bval ? "true" : "false");
            else if (left.type == VAL_NULL)
                snprintf(lbuf, sizeof(lbuf), "null");
            else {
                const char *ln = value_type_name(left.type);
                value_free(&left);
                value_free(&right);
                error(expr->line, "No se puede concatenar un valor de tipo %s con un string", ln);
            }
        }
        if (right.type != VAL_STRING) {
            if (right.type == VAL_INT || right.type == VAL_UINT) {
                char *number = bigint_to_string(value_as_bigint(right));
                snprintf(rbuf, sizeof(rbuf), "%s", number ? number : "0");
                free(number);
            }
            else if (right.type == VAL_FLOAT)
                snprintf(rbuf, sizeof(rbuf), "%.15g", right.data.fval);
            else if (right.type == VAL_BOOL)
                snprintf(rbuf, sizeof(rbuf), "%s", right.data.bval ? "true" : "false");
            else if (right.type == VAL_NULL)
                snprintf(rbuf, sizeof(rbuf), "null");
            else {
                const char *rn = value_type_name(right.type);
                value_free(&left);
                value_free(&right);
                error(expr->line, "No se puede concatenar un valor de tipo %s con un string", rn);
            }
        }
        size_t ls_len = strlen(ls);
        size_t rs_len = strlen(rs);
        size_t total = ls_len + rs_len + 1;
        char *buf = malloc(total);
        if (!buf) error(expr->line, "Memoria insuficiente al concatenar cadenas");
        memcpy(buf, ls, ls_len);
        memcpy(buf + ls_len, rs, rs_len + 1);
        /* Construimos el Value directamente: val_string() haría un
         * strdup() extra del buffer que acabamos de crear. */
        Value result = {0};
        result.type = VAL_STRING;
        result.data.sval = buf;
        value_free(&left);
        value_free(&right);
        return result;
    }

    if ((left.type == VAL_UINT || left.type == VAL_INT) &&
        (right.type == VAL_UINT || right.type == VAL_INT)) {
        if (!VALUE_HAS_WIDE_INTEGER(left) && !VALUE_HAS_WIDE_INTEGER(right)) {
            bool unsigned_result = left.type == VAL_UINT || right.type == VAL_UINT;
            int64_t a = left.type == VAL_UINT ? (int64_t)left.data.uval : left.data.ival;
            int64_t b = right.type == VAL_UINT ? (int64_t)right.data.uval : right.data.ival;
            int64_t result = 0;
            bool overflow = false;
            switch (expr->data.binop.op) {
                case TOK_PLUS:
                case TOK_STAR:
                    if (left.type == VAL_UINT && right.type == VAL_UINT) {
                        uint64_t n = expr->data.binop.op == TOK_PLUS
                            ? (uint64_t)left.data.uval + right.data.uval
                            : (uint64_t)left.data.uval * right.data.uval;
                        value_free(&left); value_free(&right);
                        return n <= UINT32_MAX ? val_uint((uint32_t)n)
                                               : val_bigint(bigint_from_u64(n), true);
                    }
                    overflow = expr->data.binop.op == TOK_PLUS
                        ? __builtin_add_overflow(a, b, &result)
                        : __builtin_mul_overflow(a, b, &result);
                    break;
                case TOK_MINUS:
                    overflow = __builtin_sub_overflow(a, b, &result);
                    break;
                case TOK_PERCENT:
                    if (!b) { value_free(&left); value_free(&right); error(expr->line, "Módulo por cero"); }
                    result = a % b;
                    break;
                case TOK_SLASH:
                    if (!b) { value_free(&left); value_free(&right); error(expr->line, "División por cero"); }
                    value_free(&left); value_free(&right);
                    return val_float((double)a / (double)b);
                default:
                    value_free(&left); value_free(&right);
                    error(expr->line, "Operador no soportado");
            }
            if (!overflow) {
                if (unsigned_result && result < 0) {
                    Value negative = val_bigint(bigint_from_i64(result), false);
                    if (!try_convert_value(&negative, TOK_UINT)) {
                        value_free(&negative); value_free(&left); value_free(&right);
                        error(expr->line, "El resultado de '%s' es negativo para uint", op_name);
                    }
                    value_free(&left); value_free(&right);
                    return negative;
                }
                if (!unsigned_result && result >= INT_MIN && result <= INT_MAX) {
                    value_free(&left); value_free(&right);
                    return val_int((int)result);
                }
                if (unsigned_result && (uint64_t)result <= UINT32_MAX) {
                    value_free(&left); value_free(&right);
                    return val_uint((uint32_t)result);
                }
                Value widened = unsigned_result
                    ? val_bigint(bigint_from_u64((uint64_t)result), true)
                    : val_bigint(bigint_from_i64(result), false);
                value_free(&left); value_free(&right);
                return widened;
            }
        }
        InfernalBigInt a = value_as_bigint(left), b = value_as_bigint(right), result;
        bool unsigned_result = left.type == VAL_UINT || right.type == VAL_UINT;
        bool ok = true;
        switch (expr->data.binop.op) {
            case TOK_PLUS: ok = bigint_add(a, b, &result); break;
            case TOK_MINUS: ok = bigint_sub(a, b, &result); break;
            case TOK_STAR: ok = bigint_mul(a, b, &result); break;
            case TOK_PERCENT: {
                InfernalBigInt quotient;
                ok = bigint_divmod(a, b, &quotient, &result);
                if (!ok) { value_free(&left); value_free(&right); error(expr->line, "Módulo por cero"); }
                break;
            }
            case TOK_SLASH: {
                if (bigint_is_zero(b)) { value_free(&left); value_free(&right); error(expr->line, "División por cero"); }
                double lv = bigint_to_double(a), rv = bigint_to_double(b);
                value_free(&left); value_free(&right);
                return val_float(lv / rv);
            }
            default: ok = false; break;
        }
        if (!ok) { value_free(&left); value_free(&right); error(expr->line, "Operación entera fuera del límite de 64 bytes o no soportada"); }
        if (unsigned_result && result.negative) {
            Value negative = val_bigint(result, false);
            if (!try_convert_value(&negative, TOK_UINT)) {
                value_free(&negative); value_free(&left); value_free(&right);
                error(expr->line, "El resultado de '%s' es negativo para uint", op_name);
            }
            value_free(&left); value_free(&right);
            return negative;
        }
        Value value = val_bigint(result, unsigned_result);
        value_free(&left); value_free(&right);
        return value;
    }

    double lv = (left.type == VAL_INT || left.type == VAL_UINT) ? value_integer_to_double(left) :
    (left.type == VAL_FLOAT) ? left.data.fval :
    (left.type == VAL_BOOL) ? (left.data.bval ? 1.0 : 0.0) : 0.0;
    double rv = (right.type == VAL_INT || right.type == VAL_UINT) ? value_integer_to_double(right) :
    (right.type == VAL_FLOAT) ? right.data.fval :
    (right.type == VAL_BOOL) ? (right.data.bval ? 1.0 : 0.0) : 0.0;
    Value result;
    switch (expr->data.binop.op) {
        case TOK_PLUS: result = val_float(lv + rv); break;
        case TOK_MINUS: result = val_float(lv - rv); break;
        case TOK_STAR: result = val_float(lv * rv); break;
        case TOK_SLASH:
            if (rv == 0) {
                value_free(&left);
                value_free(&right);
                error(expr->line, "División por cero");
            }
            result = val_float(lv / rv);
            break;
        case TOK_PERCENT:
            if (rv == 0.0) {
                value_free(&left);
                value_free(&right);
                error(expr->line, "Módulo por cero");
            }
            result = val_float(fmod(lv, rv));
            break;
        default:
            value_free(&left);
            value_free(&right);
            error(expr->line, "Operador no soportado");
    }
    value_free(&left);
    value_free(&right);
    return result;
}
