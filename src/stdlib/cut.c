/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: stdlib/cut.c
 *
 * Funciones para cortar strings.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include "cut.h"
#include "core/value.h"
#include "runtime/error.h"
#include "runtime/globals.h"
#include "vm/vm.h"


/* ================================================
 *  Ayudantes UTF‑8
 * ================================================ */

/* --- devolver el inicio del siguiente caracter UTF-8 --- */
static const char* utf8_next(const char *p) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x80) return p + 1;
    if ((c & 0xE0) == 0xC0) return p + 2;
    if ((c & 0xF0) == 0xE0) return p + 3;
    if ((c & 0xF8) == 0xF0) return p + 4;
    return p + 1;
}

/* --- representa un caracter UTF-8 dentro de la cadena --- */
typedef struct {
    const char *start;
    int         len;
} CharSegment;

/* --- combierte una cadena en un array de segmentos UTF-8 --- */
static CharSegment* utf8_to_segments(const char *s, int *count) {
    int cap = 8;
    CharSegment *segs = malloc(cap * sizeof(CharSegment));
    if (!segs) return NULL;

    int n = 0;
    const char *p = s;
    while (*p) {
        const char *start = p;
        p = utf8_next(p);
        if (n >= cap) {
            cap *= 2;
            CharSegment *tmp = realloc(segs, cap * sizeof(CharSegment));
            if (!tmp) { free(segs); return NULL; }
            segs = tmp;
        }
        segs[n].start = start;
        segs[n].len   = (int)(p - start);
        n++;
    }
    *count = n;
    return segs;
}

/* --- comprueba si 2 caracteres UTF-8 son iguales --- */
static bool seg_equal(const CharSegment *a, const CharSegment *b) {
    if (a->len != b->len) return false;
    return memcmp(a->start, b->start, a->len) == 0;
}


/* ================================================
 *  Funciones de corte (API interna)
 * ================================================ */

/* Devuelve una subcadena UTF-8 desde el segmento `start` hasta el segmento
 * `end` (exclusivo). Los índices son posiciones de segmento, no de byte.
 * El resultado se aloja en memoria dinámica y debe liberarse con free().
 * Si start > end o start < 0 o end > total, devuelve NULL.
 */
static char* utf8_slice(const CharSegment *segs, int total, int start, int end) {
    if (start < 0 || end < start || end > total) return NULL;

    int byte_len = 0;
    for (int i = start; i < end; i++) {
        byte_len += segs[i].len;
    }
    char *result = malloc(byte_len + 1);
    if (!result) return NULL;

    char *dst = result;
    for (int i = start; i < end; i++) {
        memcpy(dst, segs[i].start, segs[i].len);
        dst += segs[i].len;
    }
    *dst = '\0';
    return result;
}

/* Encuentra la primera aparición de `needle` dentro de `haystack`.
 * Devuelve el índice del primer segmento de `needle` en `haystack` o -1
 * si no se encuentra. Ambos deben ser arrays de segmentos ya preparados.
 */
static int utf8_find_first(const CharSegment *haystack, int hay_count,
                           const CharSegment *needle, int needle_count) {
    if (needle_count == 0) return 0;
    if (needle_count > hay_count) return -1;

    for (int i = 0; i <= hay_count - needle_count; i++) {
        bool match = true;
        for (int j = 0; j < needle_count; j++) {
            if (!seg_equal(&haystack[i + j], &needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return i;
    }
    return -1;
                           }

                           /* Encuentra la última aparición de `needle` dentro de `haystack`.
                            * Devuelve el índice del primer segmento de `needle` en `haystack` o -1
                            * si no se encuentra. Ambos deben ser arrays de segmentos ya preparados.
                            */
                           static int utf8_find_last(const CharSegment *haystack, int hay_count,
                                                     const CharSegment *needle, int needle_count) {
                               if (needle_count == 0) return hay_count;
                               if (needle_count > hay_count) return -1;

                               for (int i = hay_count - needle_count; i >= 0; i--) {
                                   bool match = true;
                                   for (int j = 0; j < needle_count; j++) {
                                       if (!seg_equal(&haystack[i + j], &needle[j])) {
                                           match = false;
                                           break;
                                       }
                                   }
                                   if (match) return i;
                               }
                               return -1;
                                                     }

                                                     /* cutAfter: devuelve la subcadena desde el inicio hasta el final de la
                                                      * primera aparición de `sep`, incluyendo `sep`. Si no se encuentra `sep`,
                                                      * devuelve una copia completa de `s`.
                                                      */
                                                     static char* cut_after(const char *s, const char *sep) {
                                                         int s_count, sep_count;
                                                         CharSegment *s_segs = utf8_to_segments(s, &s_count);
                                                         if (!s_segs) return NULL;
                                                         CharSegment *sep_segs = utf8_to_segments(sep, &sep_count);
                                                         if (!sep_segs) {
                                                             free(s_segs);
                                                             return NULL;
                                                         }

                                                         char *result = NULL;
                                                         int pos = utf8_find_first(s_segs, s_count, sep_segs, sep_count);
                                                         if (pos == -1) {
                                                             // no se encontró separador: devolver toda la cadena
                                                             result = utf8_slice(s_segs, s_count, 0, s_count);
                                                         } else {
                                                             // incluir el separador: desde 0 hasta pos + sep_count
                                                             result = utf8_slice(s_segs, s_count, 0, pos + sep_count);
                                                         }

                                                         free(s_segs);
                                                         free(sep_segs);
                                                         return result;
                                                     }

                                                     /* cutAfterLast: devuelve la subcadena desde el inicio hasta el final de la
                                                      * última aparición de `sep`, incluyendo `sep`. Si no se encuentra `sep`,
                                                      * devuelve una copia completa de `s`.
                                                      */
                                                     static char* cut_after_last(const char *s, const char *sep) {
                                                         int s_count, sep_count;
                                                         CharSegment *s_segs = utf8_to_segments(s, &s_count);
                                                         if (!s_segs) return NULL;
                                                         CharSegment *sep_segs = utf8_to_segments(sep, &sep_count);
                                                         if (!sep_segs) {
                                                             free(s_segs);
                                                             return NULL;
                                                         }

                                                         char *result = NULL;
                                                         int pos = utf8_find_last(s_segs, s_count, sep_segs, sep_count);
                                                         if (pos == -1) {
                                                             // no se encontró separador: devolver toda la cadena
                                                             result = utf8_slice(s_segs, s_count, 0, s_count);
                                                         } else {
                                                             // incluir el separador: desde 0 hasta pos + sep_count
                                                             result = utf8_slice(s_segs, s_count, 0, pos + sep_count);
                                                         }

                                                         free(s_segs);
                                                         free(sep_segs);
                                                         return result;
                                                     }

                                                     /* cutBefore: devuelve la subcadena desde la primera aparición de `sep`
                                                      * hasta el final, incluyendo `sep`. Si no se encuentra, devuelve cadena vacía.
                                                      */
                                                     static char* cut_before(const char *s, const char *sep) {
                                                         int s_count, sep_count;
                                                         CharSegment *s_segs = utf8_to_segments(s, &s_count);
                                                         if (!s_segs) return NULL;
                                                         CharSegment *sep_segs = utf8_to_segments(sep, &sep_count);
                                                         if (!sep_segs) {
                                                             free(s_segs);
                                                             return NULL;
                                                         }

                                                         char *result = NULL;
                                                         int pos = utf8_find_first(s_segs, s_count, sep_segs, sep_count);
                                                         if (pos == -1) {
                                                             // no encontrado: cadena vacía
                                                             result = malloc(1);
                                                             if (result) result[0] = '\0';
                                                         } else {
                                                             // desde pos hasta el final (incluyendo separador)
                                                             result = utf8_slice(s_segs, s_count, pos, s_count);
                                                         }

                                                         free(s_segs);
                                                         free(sep_segs);
                                                         return result;
                                                     }

                                                     /* cutBeforeLast: devuelve la subcadena desde la última aparición de `sep`
                                                      * hasta el final, incluyendo `sep`. Si no se encuentra, devuelve cadena vacía.
                                                      */
                                                     static char* cut_before_last(const char *s, const char *sep) {
                                                         int s_count, sep_count;
                                                         CharSegment *s_segs = utf8_to_segments(s, &s_count);
                                                         if (!s_segs) return NULL;
                                                         CharSegment *sep_segs = utf8_to_segments(sep, &sep_count);
                                                         if (!sep_segs) {
                                                             free(s_segs);
                                                             return NULL;
                                                         }

                                                         char *result = NULL;
                                                         int pos = utf8_find_last(s_segs, s_count, sep_segs, sep_count);
                                                         if (pos == -1) {
                                                             // no encontrado: cadena vacía
                                                             result = malloc(1);
                                                             if (result) result[0] = '\0';
                                                         } else {
                                                             // desde pos hasta el final (incluyendo separador)
                                                             result = utf8_slice(s_segs, s_count, pos, s_count);
                                                         }

                                                         free(s_segs);
                                                         free(sep_segs);
                                                         return result;
                                                     }

                                                     /* cutHead: elimina los primeros `n` caracteres UTF-8.
                                                      * Devuelve la subcadena desde el índice n hasta el final.
                                                      * Si n >= longitud total, devuelve cadena vacía.
                                                      * Si n < 0, devuelve NULL (error).
                                                      */
                                                     static char* cut_head(const char *s, int n) {
                                                         if (n < 0) return NULL;
                                                         int count;
                                                         CharSegment *segs = utf8_to_segments(s, &count);
                                                         if (!segs) return NULL;

                                                         char *result;
                                                         if (n >= count) {
                                                             result = malloc(1);
                                                             if (result) result[0] = '\0';
                                                         } else {
                                                             result = utf8_slice(segs, count, n, count);
                                                         }

                                                         free(segs);
                                                         return result;
                                                     }

                                                     /* cutTail: elimina los últimos `n` caracteres UTF-8.
                                                      * Devuelve la subcadena desde el inicio hasta longitud - n.
                                                      * Si n >= longitud total, devuelve cadena vacía.
                                                      * Si n < 0, devuelve NULL (error).
                                                      */
                                                     static char* cut_tail(const char *s, int n) {
                                                         if (n < 0) return NULL;
                                                         int count;
                                                         CharSegment *segs = utf8_to_segments(s, &count);
                                                         if (!segs) return NULL;

                                                         char *result;
                                                         int end = count - n;
                                                         if (end <= 0) {
                                                             result = malloc(1);
                                                             if (result) result[0] = '\0';
                                                         } else {
                                                             result = utf8_slice(segs, count, 0, end);
                                                         }

                                                         free(segs);
                                                         return result;
                                                     }


                                                     /* ================================================
                                                      *  Builtins para la VM / intérprete
                                                      * ================================================ */

                                                     static Value builtin_cutAfter(int argc, Value *args) {
                                                         if (argc != 2) error(current_eval_line, "cutAfter() espera exactamente 2 argumentos");
                                                         if (args[0].type != VAL_STRING) error(current_eval_line, "cutAfter() espera un string como primer argumento");
                                                         if (args[1].type != VAL_STRING) error(current_eval_line, "cutAfter() espera un string como segundo argumento");

                                                         const char *s = args[0].data.sval;
                                                         const char *sep = args[1].data.sval;

                                                         char *res = cut_after(s, sep);
                                                         if (!res) error(current_eval_line, "memoria insuficiente en cutAfter");

                                                         Value v = val_string(res);
                                                         free(res);
                                                         return v;
                                                     }

                                                     static Value builtin_cutAfterLast(int argc, Value *args) {
                                                         if (argc != 2) error(current_eval_line, "cutAfterLast() espera exactamente 2 argumentos");
                                                         if (args[0].type != VAL_STRING) error(current_eval_line, "cutAfterLast() espera un string como primer argumento");
                                                         if (args[1].type != VAL_STRING) error(current_eval_line, "cutAfterLast() espera un string como segundo argumento");

                                                         const char *s = args[0].data.sval;
                                                         const char *sep = args[1].data.sval;

                                                         char *res = cut_after_last(s, sep);
                                                         if (!res) error(current_eval_line, "memoria insuficiente en cutAfterLast");

                                                         Value v = val_string(res);
                                                         free(res);
                                                         return v;
                                                     }

                                                     static Value builtin_cutBefore(int argc, Value *args) {
                                                         if (argc != 2) error(current_eval_line, "cutBefore() espera exactamente 2 argumentos");
                                                         if (args[0].type != VAL_STRING) error(current_eval_line, "cutBefore() espera un string como primer argumento");
                                                         if (args[1].type != VAL_STRING) error(current_eval_line, "cutBefore() espera un string como segundo argumento");

                                                         const char *s = args[0].data.sval;
                                                         const char *sep = args[1].data.sval;

                                                         char *res = cut_before(s, sep);
                                                         if (!res) error(current_eval_line, "memoria insuficiente en cutBefore");

                                                         Value v = val_string(res);
                                                         free(res);
                                                         return v;
                                                     }

                                                     static Value builtin_cutBeforeLast(int argc, Value *args) {
                                                         if (argc != 2) error(current_eval_line, "cutBeforeLast() espera exactamente 2 argumentos");
                                                         if (args[0].type != VAL_STRING) error(current_eval_line, "cutBeforeLast() espera un string como primer argumento");
                                                         if (args[1].type != VAL_STRING) error(current_eval_line, "cutBeforeLast() espera un string como segundo argumento");

                                                         const char *s = args[0].data.sval;
                                                         const char *sep = args[1].data.sval;

                                                         char *res = cut_before_last(s, sep);
                                                         if (!res) error(current_eval_line, "memoria insuficiente en cutBeforeLast");

                                                         Value v = val_string(res);
                                                         free(res);
                                                         return v;
                                                     }

                                                     static Value builtin_cutHead(int argc, Value *args) {
                                                         if (argc != 2) error(current_eval_line, "cutHead() espera exactamente 2 argumentos");
                                                         if (args[0].type != VAL_STRING) error(current_eval_line, "cutHead() espera un string como primer argumento");
                                                         if (args[1].type != VAL_INT) error(current_eval_line, "cutHead() espera un entero como segundo argumento");

                                                         const char *s = args[0].data.sval;
                                                         int n = args[1].data.ival;
                                                         if (n < 0) error(current_eval_line, "cutHead() no acepta índices negativos");

                                                         char *res = cut_head(s, n);
                                                         if (!res) error(current_eval_line, "memoria insuficiente en cutHead");

                                                         Value v = val_string(res);
                                                         free(res);
                                                         return v;
                                                     }

                                                     static Value builtin_cutTail(int argc, Value *args) {
                                                         if (argc != 2) error(current_eval_line, "cutTail() espera exactamente 2 argumentos");
                                                         if (args[0].type != VAL_STRING) error(current_eval_line, "cutTail() espera un string como primer argumento");
                                                         if (args[1].type != VAL_INT) error(current_eval_line, "cutTail() espera un entero como segundo argumento");

                                                         const char *s = args[0].data.sval;
                                                         int n = args[1].data.ival;
                                                         if (n < 0) error(current_eval_line, "cutTail() no acepta índices negativos");

                                                         char *res = cut_tail(s, n);
                                                         if (!res) error(current_eval_line, "memoria insuficiente en cutTail");

                                                         Value v = val_string(res);
                                                         free(res);
                                                         return v;
                                                     }


                                                     /* ================================================
                                                      *  Registro de funciones
                                                      * ================================================ */

                                                     void register_cut_builtins(void) {
                                                         func_register_builtin("cutAfter",      builtin_cutAfter);
                                                         func_register_builtin("cutAfterLast",  builtin_cutAfterLast);
                                                         func_register_builtin("cutBefore",     builtin_cutBefore);
                                                         func_register_builtin("cutBeforeLast", builtin_cutBeforeLast);
                                                         func_register_builtin("cutHead",       builtin_cutHead);
                                                         func_register_builtin("cutTail",       builtin_cutTail);

                                                         vm_register_builtin("cutAfter",      builtin_cutAfter);
                                                         vm_register_builtin("cutAfterLast",  builtin_cutAfterLast);
                                                         vm_register_builtin("cutBefore",     builtin_cutBefore);
                                                         vm_register_builtin("cutBeforeLast", builtin_cutBeforeLast);
                                                         vm_register_builtin("cutHead",       builtin_cutHead);
                                                         vm_register_builtin("cutTail",       builtin_cutTail);
                                                     }
