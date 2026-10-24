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
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include "eval_index.h"
#include "eval_slice.h"
#include "helpers.h"
#include "evaluator.h"
#include "core/value.h"
#include "runtime/error.h"
#include "runtime/globals.h"

static void eval_index_error(Value *base, Value *idx, int line, const char *fmt, ...) {
    char message[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    value_free(base);
    value_free(idx);
    error(line, "%s", message);
}

/* Devuelve el carácter UTF-8 de índice 1-based como string independiente.
 * Las secuencias malformadas se tratan byte a byte para no leer fuera de la
 * cadena ni devolver fragmentos de un carácter multibyte. */
static char *utf8_char_at(const char *s, size_t wanted, size_t *char_count) {
    size_t index = 0;
    const unsigned char *p = (const unsigned char *)s;

    while (*p) {
        const unsigned char *start = p;
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

        index++;
        if (index == wanted) {
            char *out = malloc(width + 1);
            if (!out) error(current_eval_line, "Memoria insuficiente al indexar string");
            memcpy(out, start, width);
            out[width] = '\0';
            if (char_count) *char_count = index;
            return out;
        }
        p += width;
    }

    if (char_count) *char_count = index;
    return NULL;
}

Value eval_index(ASTNode *expr) {
    if (expr->data.idx.index->kind == NODE_SLICE) {
        ASTNode *slice = expr->data.idx.index;
        slice->data.slice.list = expr->data.idx.list;
        return eval_slice(slice);
    }

    int line = get_node_line(expr);
    if (line == 0) line = current_eval_line;
    if (line == 0) line = get_node_line(expr->data.idx.index);
    if (line == 0) line = get_node_line(expr->data.idx.list);

    Value base = eval_expr(expr->data.idx.list);

    /* Si el contenedor viene de una indexación previa, lo que obtenemos
     * es una REFERENCIA (por ejemplo, `pages[web]` devuelve una referencia
     * a `pages[web]` para que se pueda modificar). Pero aquí lo que
     * queremos es leer el elemento de dentro, no modificar el contenedor,
     * así que resolvemos la referencia ANTES de aplicar el índice.
     *
     * Sin esto, cadenas del tipo `pages[web][pagina]["clave"]` fallan
     * con "No se puede indexar este tipo de valor" porque `base` llega
     * con type == VAL_REFERENCE. */
    if (base.type == VAL_REFERENCE)
        base = resolve_reference(base, line);

    Value idx = eval_expr(expr->data.idx.index);
    /* Un índice que venga de otra indexación (p. ej. a[b[c]]) llega como
     * VAL_REFERENCE. Igual que hacemos con `base`, lo resolvemos antes de
     * usarlo. Sin esto, a[b[c]] falla diciendo que el índice no es int
     * aunque b[c] sí lo sea. */
    if (idx.type == VAL_REFERENCE)
        idx = resolve_reference(idx, line);
    Value result = val_make_null();

    switch (base.type) {
        case VAL_LIST: {
            /* --- Validación del índice ---
             *
             * Antes de tocar la lista, damos un error específico según lo
             * que realmente falló. Así el usuario no ve el mensaje genérico
             * "Índice fuera de rango..." cuando en realidad pasó un string,
             * un bool, una lista o un decimal como índice. */

            /* Fase 1: tipo del índice. Solo int y float son válidos. */
            if (idx.type != VAL_INT && idx.type != VAL_FLOAT) {
                eval_index_error(&base, &idx, line,
                                 "El índice de una lista debe ser un número entero (int), "
                                 "pero se recibió un valor de tipo '%s'.\n"
                                 "    Para acceder a un elemento por su posición usa un entero:\n"
                                 "        mi_lista[1]       (primer elemento)\n"
                                 "        mi_lista[$i]      (donde $i es una variable int)\n"
                                 "    Recuerda que los índices de list en Infernal empiezan en 1.",
                                 value_type_name(idx.type));
            }

            /* Fase 2: si es float, debe ser finito y sin parte decimal. */
            if (idx.type == VAL_FLOAT) {
                double f = idx.data.fval;
                if (!isfinite(f)) {
                    eval_index_error(&base, &idx, line,
                                     "El índice de una lista debe ser un número entero (int), "
                                     "pero se recibió un valor no finito (%g).",
                                     f);
                }
                if (trunc(f) != f) {
                    eval_index_error(&base, &idx, line,
                                     "El índice de una lista debe ser un número entero (int), "
                                     "pero se recibió el decimal %g.\n"
                                     "    Los índices de list no admiten parte decimal.",
                                     f);
                }
                /* Float entero: lo pasamos a int para unificar la validación
                 * de rango y el resto de la lógica. */
                idx.type = VAL_INT;
                idx.data.ival = (int)f;
            }

            /* Fase 3: rango. Los índices de list van de 1 a count. */
            if (idx.data.ival < 1 || idx.data.ival > base.data.list.count) {
                if (idx.data.ival < 1) {
                    eval_index_error(&base, &idx, line,
                                     "Índice fuera de rango: %d. Los índices de una lista "
                                     "empiezan en 1, así que no puede ser 0 ni negativo.",
                                     idx.data.ival);
                } else {
                    eval_index_error(&base, &idx, line,
                                     "Índice fuera de rango: %d. La lista tiene %d elemento(s) "
                                     "(los índices válidos van de 1 a %d).",
                                     idx.data.ival, base.data.list.count, base.data.list.count);
                }
            }

            /* Devolvemos referencia si la lista base es una variable sin '$'
             * (para que `files[i] = x` pueda modificar la lista original). */
            if (expr->data.idx.list && expr->data.idx.list->kind == NODE_VAR &&
                !expr->data.idx.list->data.var.clone) {
                const char *name = expr->data.idx.list->data.var.name;
            result = val_reference(name, idx.data.ival);
            value_free(&base);
            value_free(&idx);
            return result;
                }

                if (base.data.list.items == NULL)
                    eval_index_error(&base, &idx, line, "Lista corrupta: items es NULL");
            result = copy_value_secure(base.data.list.items[idx.data.ival - 1]);
            break;
        }
        case VAL_STRING: {
            int position;
            if (idx.type == VAL_INT) {
                position = idx.data.ival;
            } else if (idx.type == VAL_FLOAT) {
                double f = idx.data.fval;
                if (!isfinite(f) || f < (double)INT_MIN || f > (double)INT_MAX)
                    eval_index_error(&base, &idx, line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                position = (int)f;
                if ((double)position != f)
                    eval_index_error(&base, &idx, line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            } else {
                eval_index_error(&base, &idx, line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                return val_make_null();
            }

            if (position < 1)
                eval_index_error(&base, &idx, line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");

            size_t count = 0;
            char *character = utf8_char_at(base.data.sval, (size_t)position, &count);
            if (!character)
                eval_index_error(&base, &idx, line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            result = val_string(character);
            free(character);
            break;
        }
        case VAL_MAP: {
            if (idx.type != VAL_STRING) {
                eval_index_error(&base, &idx, line,
                                 "La clave de un mapa debe ser un texto (string), pero usaste un valor de tipo '%s'.\n"
                                 "    Un mapa guarda valores asociados a claves de texto, así que se accede con corchetes\n"
                                 "    y una cadena entre comillas, por ejemplo: mi_mapa[\"clave\"].",
                                 value_type_name(idx.type));
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
        default: {
            /* El mensaje explica qué es "indexar", qué tipos lo permiten
             * y qué tipo tiene realmente el valor sobre el que se intentó.
             * Así el error se entiende sin saber cómo funciona el intérprete
             * por dentro. */
            char msg[1024];
            snprintf(msg, sizeof(msg),
                     "No se puede usar '[...]' sobre un valor de tipo '%s'.\n"
                     "    Usar '[...]' (indexar) sirve para sacar un elemento de dentro de algo:\n"
                     "        · una lista:   mi_lista[1]        → devuelve el elemento en la posición 1\n"
                     "        · un mapa:     mi_mapa[\"clave\"]  → devuelve el valor guardado bajo esa clave\n"
                     "        · un texto:    mi_texto[1]        → devuelve una letra (como string de 1 carácter)\n"
                     "    El valor que tienes a la izquierda de los corchetes es de tipo '%s'\n"
                     "    y ese tipo no contiene elementos que se puedan sacar con corchetes.\n"
                     "    Revisa qué variable estás indexando: probablemente sea un número, un bool o null,\n"
                     "    o el resultado de una operación que no devolvió ni lista, ni mapa, ni texto.",
                     value_type_name(base.type),
                     value_type_name(base.type));
            eval_index_error(&base, &idx, line, "%s", msg);
        }
    }
    value_free(&base);
    value_free(&idx);
    return result;
}
