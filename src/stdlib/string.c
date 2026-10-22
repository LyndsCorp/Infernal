/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: stdlib/string.c
 *
 * Funciones para trabajar con strings (cadenas de texto).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "string.h"
#include "core/value.h"
#include "runtime/error.h"
#include "runtime/globals.h"


/* ================================================
 *  Ayudantes UTF-8
 * ================================================ */

/* Decodifica un codepoint UTF-8 desde `p`. Devuelve los bytes consumidos.
 *
 * Decodificador estricto (RFC 3629): rechaza secuencias overlong,
 * surrogates (U+D800–U+DFFF) y codepoints > U+10FFFF. En cualquiera
 * de esos casos, o si la secuencia está truncada, se cae al fallback
 * Latin-1: se trata el byte como un codepoint U+0000–U+00FF y se
 * avanza 1 byte. Esto evita que, p. ej., "\xC0\x80" se decodifique
 * como NUL (truncando el string) o que "\xC0\xAF" se decodifique
 * como '/' cuando el emisor quería enviar bytes crudos. */
static size_t utf8_decode_cp(const char *p, uint32_t *out_cp) {
    const unsigned char *u = (const unsigned char *)p;
    if (u[0] < 0x80) { *out_cp = u[0]; return 1; }

    if ((u[0] & 0xE0) == 0xC0 && u[1] && (u[1] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
        if (cp >= 0x80) { *out_cp = cp; return 2; }        /* overlong */
    }
    else if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2] &&
             (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(u[0] & 0x0F) << 12) |
                      ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) {
            *out_cp = cp; return 3;                        /* overlong + surrogates */
        }
    }
    else if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3] &&
             (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 &&
             (u[3] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(u[0] & 0x07) << 18) |
                      ((uint32_t)(u[1] & 0x3F) << 12) |
                      ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            *out_cp = cp; return 4;                        /* overlong + fuera de rango */
        }
    }
    /* Fallback Latin-1: byte inválido o secuencia truncada. */
    *out_cp = u[0];
    return 1;
}

/* Codifica un codepoint UTF-8 en `out`. Devuelve los bytes escritos. */
static size_t utf8_encode_cp(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ¿Es `cp` un codepoint con la propiedad Unicode White_Space=true?
 * Cubre ASCII, Latin-1 (NBSP, NEL) y los espacios "tipográficos"
 * (en/em/thin space, line/paragraph separator, etc.).
 * Referencia: https://www.unicode.org/Public/UCD/latest/ucd/PropList.txt */
static bool utf8_is_space(uint32_t cp) {
    return cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D ||
           cp == 0x20 || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
           cp == 0x205F || cp == 0x3000;
}

/* Avanza al inicio del siguiente caracter UTF-8.
 * Delega en utf8_decode_cp para que la validación sea consistente:
 * las mismas secuencias inválidas que utf8_decode_cp rechaza son
 * también consumidas byte a byte aquí. */
static const char* utf8_next(const char *p) {
    if ((unsigned char)*p == 0) return p;
    uint32_t cp;
    return p + utf8_decode_cp(p, &cp);
}

/* Cuenta codepoints UTF-8 (consistente con utf8_to_segments).
 * Nótese que "codepoint" != "grapheme cluster": "👨‍👩‍👧" cuenta 5
 * (ZWJ + 2 emoji + ZWJ + emoji + modificador), no 1. Contar
 * graphemes requeriría tablas de segmentación que aquí no usamos. */
static size_t utf8_len(const char *s) {
    if (!s) return 0;
    size_t n = 0;
    while (*s) { s = utf8_next(s); n++; }
    return n;
}

/* Representa un caracter UTF-8 dentro de la cadena. */
typedef struct {
    const char *start;
    int         len;
} CharSegment;

/* Convierte una cadena en un array de segmentos UTF-8. */
static CharSegment* utf8_to_segments(const char *s, int *count) {
    if (!s) s = "";
    int cap = 8;
    CharSegment *segs = malloc(cap * sizeof(CharSegment));
    if (!segs) return NULL;

    int n = 0;
    const char *p = s;
    while (*p) {
        const char *start = p;
        p = utf8_next(p);
        if (n >= cap) {
            /* Chequeo de overflow: nos aseguramos de que
             * cap*2*sizeof(CharSegment) no desborde size_t. */
            if (cap > SIZE_MAX / 2 / sizeof(CharSegment)) {
                free(segs); return NULL;
            }
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

/* Comprueba si 2 caracteres UTF-8 son iguales. */
static bool seg_equal(const CharSegment *a, const CharSegment *b) {
    if (a->len != b->len) return false;
    return memcmp(a->start, b->start, a->len) == 0;
}


/* ================================================
 *  Conversión de caja (UTF-8 aware)
 * ================================================
 *
 * Se usan tablas manuales para los rangos Unicode más comunes,
 * evitando depender del locale del sistema (que puede no ser UTF-8).
 * Los caracteres fuera de los rangos cubiertos se dejan sin cambios.
 *
 * Rangos cubiertos:
 *   - ASCII (A-Z / a-z)
 *   - Latin-1 Supplement (U+00C0–U+00FF)
 *   - Latin Extended-A  (U+0100–U+017E)
 *   - Griego            (U+0391–U+03AB / U+03B1–U+03CB)
 *   - Cirílico          (U+0400–U+045F)
 *
 * Casos especiales:
 *   - U+0131 (ı, i turca sin punto) -> 'I' en mayúscula
 *   - U+0178 (Ÿ) / U+00FF (ÿ) se mapean entre sí (no es ±0x20)
 *
 * No se cubren (quedan sin cambios, como es habitual):
 *   - ß (U+00DF) -> "SS" (multi-carácter)
 *   - İ (U+0130) -> "i̇" (multi-carácter)
 *   - Σ/ς (sigma final): se usa Σ -> σ (sin contexto)
 */

static uint32_t codepoint_to_lower(uint32_t cp) {
    /* ASCII A-Z */
    if (cp >= 'A' && cp <= 'Z') return cp + 0x20;

    /* Latin-1 Supplement: A–Ö, Ø–Þ -> +0x20 */
    if (cp >= 0x00C0 && cp <= 0x00D6) return cp + 0x20;
    if (cp >= 0x00D8 && cp <= 0x00DE) return cp + 0x20;

    /* Latin Extended-A. Muchos pares son consecutivos (par=upper,
     * impar=lower), pero hay huecos que respetamos. */
    if (cp >= 0x0100 && cp <= 0x012F) {       /* Ā–į */
        return (cp & 1) == 0 ? cp + 1 : cp;
    }
    if (cp >= 0x0132 && cp <= 0x0137) {       /* Ĳ–ķ */
        return (cp & 1) == 0 ? cp + 1 : cp;
    }
    if (cp >= 0x0139 && cp <= 0x0148) {       /* Ĺ–ň (impar=upper) */
        return (cp & 1) == 1 ? cp + 1 : cp;
    }
    if (cp >= 0x014A && cp <= 0x0177) {       /* Ŋ–ŷ */
        return (cp & 1) == 0 ? cp + 1 : cp;
    }
    if (cp == 0x0178) return 0x00FF;          /* Ÿ -> ÿ */
    if (cp >= 0x0179 && cp <= 0x017E) {       /* Ź–ž (impar=upper) */
        return (cp & 1) == 1 ? cp + 1 : cp;
    }

    /* Griego: Α–Ρ, Σ–Ϋ -> +0x20 */
    if ((cp >= 0x0391 && cp <= 0x03A1) ||
        (cp >= 0x03A3 && cp <= 0x03AB)) return cp + 0x20;

    /* Cirílico: Ѐ–Џ -> +0x50 ; А–Я -> +0x20 */
    if (cp >= 0x0400 && cp <= 0x040F) return cp + 0x50;
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;

    return cp;
}

static uint32_t codepoint_to_upper(uint32_t cp) {
    /* ASCII a-z */
    if (cp >= 'a' && cp <= 'z') return cp - 0x20;

    /* Latin-1 Supplement: à–ö, ø–þ -> -0x20 ; ÿ -> Ÿ */
    if (cp >= 0x00E0 && cp <= 0x00F6) return cp - 0x20;
    if (cp >= 0x00F8 && cp <= 0x00FE) return cp - 0x20;
    if (cp == 0x00FF) return 0x0178;

    /* Latin Extended-A */
    if (cp >= 0x0100 && cp <= 0x012F) {
        return (cp & 1) == 1 ? cp - 1 : cp;
    }
    if (cp == 0x0131) return 'I';             /* ı -> I (turca) */
    if (cp >= 0x0132 && cp <= 0x0137) {
        return (cp & 1) == 1 ? cp - 1 : cp;
    }
    if (cp >= 0x0139 && cp <= 0x0148) {
        return (cp & 1) == 0 ? cp - 1 : cp;
    }
    if (cp >= 0x014A && cp <= 0x0177) {
        return (cp & 1) == 1 ? cp - 1 : cp;
    }
    if (cp >= 0x0179 && cp <= 0x017E) {
        return (cp & 1) == 0 ? cp - 1 : cp;
    }

    /* Griego: α–ρ, σ–ϋ -> -0x20 */
    if ((cp >= 0x03B1 && cp <= 0x03C1) ||
        (cp >= 0x03C3 && cp <= 0x03CB)) return cp - 0x20;

    /* Cirílico: ѐ–џ -> -0x50 ; а–я -> -0x20 */
    if (cp >= 0x0450 && cp <= 0x045F) return cp - 0x50;
    if (cp >= 0x0430 && cp <= 0x044F) return cp - 0x20;

    return cp;
}

/* Transforma la caja de `s`, devolviendo un buffer malloc'd.
 *
 * Modos:
 *   - to_upper=true,  capitalize_mode=false  → TODO a mayúscula
 *   - to_upper=false, capitalize_mode=false  → TODO a minúscula
 *   - capitalize_mode=true                   → primer codepoint a
 *                                              mayúscula, resto a
 *                                              minúscula (capitalize)
 *
 * Los codepoints sin mapeo se dejan intactos.
 *
 * Cota de tamaño:
 *   - Para UTF-8 VÁLIDO en los rangos cubiertos: cada codepoint
 *     mapeado ocupa <= los mismos bytes que el original (los mapeos
 *     van dentro del mismo bloque: Latin-1→Latin-1, Latin-A→Latin-A,
 *     U+0131→'I' encoge 2→1, U+00FF↔U+0178 ambos 2→2). Por eso
 *     `cap = byte_len + 1` basta.
 *   - Para UTF-8 INVÁLIDO: el fallback Latin-1 convierte un byte
 *     0x80–0xFF en un codepoint de 2 bytes UTF-8. La expansión
 *     máxima es 2×, y el `realloc` defensivo la absorbe. */
static char *utf8_case_transform(const char *s, bool to_upper,
                                 bool capitalize_mode) {
    if (!s) return NULL;
    size_t byte_len = strlen(s);
    size_t cap = byte_len + 1;
    char *out = malloc(cap);
    if (!out) return NULL;

    size_t out_len = 0;
    const char *p = s;
    bool first = true;

    while (*p) {
        uint32_t cp;
        size_t consumed = utf8_decode_cp(p, &cp);

        uint32_t mapped;
        if (capitalize_mode) {
            mapped = first ? codepoint_to_upper(cp) : codepoint_to_lower(cp);
        } else {
            mapped = to_upper ? codepoint_to_upper(cp) : codepoint_to_lower(cp);
        }

        char encoded[4];
        size_t enc_len = utf8_encode_cp(mapped, encoded);

        if (out_len + enc_len + 1 > cap) {
            size_t needed = out_len + enc_len + 1;
            size_t new_cap = cap;
            while (new_cap < needed) {
                if (new_cap > SIZE_MAX / 2) { free(out); return NULL; }
                new_cap *= 2;
            }
            char *tmp = realloc(out, new_cap);
            if (!tmp) { free(out); return NULL; }
            out = tmp;
            cap = new_cap;
        }
        memcpy(out + out_len, encoded, enc_len);
        out_len += enc_len;
        p += consumed;
        first = false;
    }
    out[out_len] = '\0';
    return out;
}


/* ================================================
 *  Funciones de la biblioteca
 * ================================================ */

/* --- lower() --- */
static Value builtin_lower(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "lower() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "lower() espera un string.");
    char *s = utf8_case_transform(args[0].data.sval, false, false);
    if (!s) error(current_eval_line, "memoria insuficiente en lower");
    Value res = val_string(s);
    free(s);
    return res;
}

/* --- upper() --- */
static Value builtin_upper(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "upper() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "upper() espera un string.");
    char *s = utf8_case_transform(args[0].data.sval, true, false);
    if (!s) error(current_eval_line, "memoria insuficiente en upper");
    Value res = val_string(s);
    free(s);
    return res;
}

/* --- capitalize() --- */
static Value builtin_capitalize(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "capitalize() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "capitalize() espera un string.");

    /* Preserva comportamiento original: NULL/vacío -> "". */
    if (!args[0].data.sval || *args[0].data.sval == '\0')
        return val_string("");

    char *s = utf8_case_transform(args[0].data.sval, false, true);
    if (!s) error(current_eval_line, "memoria insuficiente en capitalize");
    Value res = val_string(s);
    free(s);
    return res;
}

/* --- count() --- */
static Value builtin_count(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "count() espera exactamente 2 argumentos");
    if (args[0].type != VAL_STRING) error(current_eval_line, "count() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(current_eval_line, "count() espera un string como segundo argumento");

    const char *haystack = args[0].data.sval ? args[0].data.sval : "";
    const char *needle   = args[1].data.sval ? args[1].data.sval : "";
    if (*needle == '\0') return val_int(0);

    int hay_count, needle_count;
    CharSegment *hay_segs = utf8_to_segments(haystack, &hay_count);
    if (!hay_segs) error(current_eval_line, "memoria insuficiente en count");
    CharSegment *needle_segs = utf8_to_segments(needle, &needle_count);
    if (!needle_segs) {
        free(hay_segs);
        error(current_eval_line, "memoria insuficiente en count");
    }

    int count = 0;
    if (needle_count <= hay_count) {
        for (int i = 0; i <= hay_count - needle_count; ) {
            bool match = true;
            for (int j = 0; j < needle_count; j++) {
                if (!seg_equal(&hay_segs[i + j], &needle_segs[j])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                count++;
                i += needle_count;  /* no solapado */
            } else {
                i++;
            }
        }
    }

    free(hay_segs);
    free(needle_segs);
    return val_int(count);
}

/* --- indexof() --- */
static Value builtin_indexof(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "indexof() espera exactamente 2 argumentos");
    if (args[0].type != VAL_STRING) error(current_eval_line, "indexof() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(current_eval_line, "indexof() espera un string como segundo argumento");

    const char *haystack = args[0].data.sval ? args[0].data.sval : "";
    const char *needle   = args[1].data.sval ? args[1].data.sval : "";

    if (*needle == '\0') return val_int(1);

    int hay_count, needle_count;
    CharSegment *hay_segs = utf8_to_segments(haystack, &hay_count);
    if (!hay_segs) error(current_eval_line, "memoria insuficiente en indexof");
    CharSegment *needle_segs = utf8_to_segments(needle, &needle_count);
    if (!needle_segs) {
        free(hay_segs);
        error(current_eval_line, "memoria insuficiente en indexof");
    }

    int result = 0;
    if (needle_count <= hay_count) {
        for (int i = 0; i <= hay_count - needle_count; i++) {
            bool match = true;
            for (int j = 0; j < needle_count; j++) {
                if (!seg_equal(&hay_segs[i + j], &needle_segs[j])) {
                    match = false;
                    break;
                }
            }
            if (match) { result = i + 1; break; }
        }
    }

    free(hay_segs);
    free(needle_segs);
    return val_int(result);
}

/* --- replace() --- */
static Value builtin_replace(int argc, Value *args) {
    if (argc != 3) error(current_eval_line, "replace() espera exactamente 3 argumentos");
    if (args[0].type != VAL_STRING) error(current_eval_line, "replace() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(current_eval_line, "replace() espera un string como segundo argumento (a reemplazar)");
    if (args[2].type != VAL_STRING) error(current_eval_line, "replace() espera un string como tercer argumento (reemplazo)");

    const char *src      = args[0].data.sval ? args[0].data.sval : "";
    const char *from_str = args[1].data.sval ? args[1].data.sval : "";
    const char *to_str   = args[2].data.sval ? args[2].data.sval : "";
    if (from_str[0] == '\0') return val_string(src);

    int src_seg_count, from_seg_count, to_seg_count;
    CharSegment *src_segs  = utf8_to_segments(src, &src_seg_count);
    if (!src_segs) error(current_eval_line, "memoria insuficiente en replace");
    CharSegment *from_segs = utf8_to_segments(from_str, &from_seg_count);
    if (!from_segs) { free(src_segs); error(current_eval_line, "memoria insuficiente en replace"); }
    CharSegment *to_segs   = utf8_to_segments(to_str, &to_seg_count);
    if (!to_segs) { free(src_segs); free(from_segs); error(current_eval_line, "memoria insuficiente en replace"); }

    /* malloc(0) es implementación-definido: puede devolver
     * NULL y disparar un falso "memoria insuficiente" cuando
     * el string de origen está vacío. Devolvemos directamente
     * la cadena vacía en ese caso. */
    if (src_seg_count == 0) {
        free(src_segs);
        free(from_segs);
        free(to_segs);
        return val_string("");
    }
    int *replace_pos = malloc((size_t)src_seg_count * sizeof(int));
    if (!replace_pos) { free(src_segs); free(from_segs); free(to_segs); error(current_eval_line, "memoria insuficiente en replace"); }

    int match_count = 0;
    if (from_seg_count > 0) {
        for (int i = 0; i <= src_seg_count - from_seg_count; i++) {
            bool match = true;
            for (int j = 0; j < from_seg_count; j++) {
                if (!seg_equal(&src_segs[i + j], &from_segs[j])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                replace_pos[match_count++] = i;
                i += from_seg_count - 1;
            }
        }
    }

    size_t total_bytes = 0;
    int current = 0;
    for (int m = 0; m < match_count; m++) {
        int pos = replace_pos[m];
        for (int i = current; i < pos; i++) total_bytes += (size_t)src_segs[i].len;
        for (int i = 0; i < to_seg_count; i++) total_bytes += (size_t)to_segs[i].len;
        current = pos + from_seg_count;
    }
    for (int i = current; i < src_seg_count; i++) total_bytes += (size_t)src_segs[i].len;

    char *result = malloc(total_bytes + 1);
    if (!result) { free(src_segs); free(from_segs); free(to_segs); free(replace_pos); error(current_eval_line, "memoria insuficiente en replace"); }

    char *dst = result;
    current = 0;
    for (int m = 0; m < match_count; m++) {
        int pos = replace_pos[m];
        for (int i = current; i < pos; i++) { memcpy(dst, src_segs[i].start, (size_t)src_segs[i].len); dst += src_segs[i].len; }
        for (int i = 0; i < to_seg_count; i++) { memcpy(dst, to_segs[i].start, (size_t)to_segs[i].len); dst += to_segs[i].len; }
        current = pos + from_seg_count;
    }
    for (int i = current; i < src_seg_count; i++) { memcpy(dst, src_segs[i].start, (size_t)src_segs[i].len); dst += src_segs[i].len; }
    *dst = '\0';

    Value res = val_string(result);
    free(result);
    free(src_segs); free(from_segs); free(to_segs); free(replace_pos);
    return res;
}

/* --- reverse() --- */
static Value builtin_reverse(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "reverse() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "reverse() espera un string.");

    const char *src = args[0].data.sval ? args[0].data.sval : "";
    int seg_count;
    CharSegment *segs = utf8_to_segments(src, &seg_count);
    if (!segs) error(current_eval_line, "memoria insuficiente en reverse");

    size_t total_len = 0;
    for (int i = 0; i < seg_count; i++) total_len += (size_t)segs[i].len;

    char *rev = malloc(total_len + 1);
    if (!rev) { free(segs); error(current_eval_line, "memoria insuficiente en reverse"); }

    char *dst = rev;
    for (int i = seg_count - 1; i >= 0; i--) {
        memcpy(dst, segs[i].start, (size_t)segs[i].len);
        dst += segs[i].len;
    }
    *dst = '\0';

    Value res = val_string(rev);
    free(rev);
    free(segs);
    return res;
}

/* --- join() --- */
static Value builtin_join(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "join() espera exactamente 2 argumentos");
    if (args[0].type != VAL_LIST) error(current_eval_line, "join() espera una lista como primer argumento");
    if (args[1].type != VAL_STRING) error(current_eval_line, "join() espera un string como segundo argumento");

    int n = args[0].data.list.count;
    const char *sep = args[1].data.sval ? args[1].data.sval : "";
    size_t sep_len = strlen(sep);

    size_t total_len = 0;
    for (int i = 0; i < n; i++) {
        if (args[0].data.list.items[i].type != VAL_STRING)
            error(current_eval_line, "join() solo admite listas de strings");
        const char *elem = args[0].data.list.items[i].data.sval ? args[0].data.list.items[i].data.sval : "";
        size_t elem_len = strlen(elem);
        if (total_len > SIZE_MAX - elem_len) error(current_eval_line, "join() resultado demasiado grande");
        total_len += elem_len;
    }
    if (n > 1) {
        if (sep_len > 0 && (size_t)(n - 1) > SIZE_MAX / sep_len)
            error(current_eval_line, "join() resultado demasiado grande");
        size_t sep_total = (size_t)(n - 1) * sep_len;
        if (total_len > SIZE_MAX - sep_total) error(current_eval_line, "join() resultado demasiado grande");
        total_len += sep_total;
    }
    if (total_len == SIZE_MAX) error(current_eval_line, "join() resultado demasiado grande");
    total_len += 1;

    char *buf = malloc(total_len);
    if (!buf) error(current_eval_line, "memoria insuficiente en join");

    char *ptr = buf;
    for (int i = 0; i < n; i++) {
        const char *elem = args[0].data.list.items[i].data.sval ? args[0].data.list.items[i].data.sval : "";
        size_t elem_len = strlen(elem);
        memcpy(ptr, elem, elem_len);
        ptr += elem_len;
        if (i < n - 1) {
            memcpy(ptr, sep, sep_len);
            ptr += sep_len;
        }
    }
    *ptr = '\0';

    Value res = val_string(buf);
    free(buf);
    return res;
}

/* --- length() --- */
static Value builtin_length(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "length() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "length() espera un string.");
    size_t n = utf8_len(args[0].data.sval);
    return val_int((int)n);
}

/* --- trim helpers (UTF-8 aware) ---
 *
 * Recorta `str` por la izquierda (left=1) y/o derecha (right=1)
 * eliminando codepoints con propiedad White_Space. La parte no
 * recortada se copia byte a byte, así que los caracteres multibyte
 * internos se preservan intactos. */
static char* do_trim(const char *str, int left, int right) {
    if (!str) str = "";
    const char *start = str;
    const char *end   = str + strlen(str);

    if (left) {
        while (*start) {
            uint32_t cp;
            size_t n = utf8_decode_cp(start, &cp);
            if (!utf8_is_space(cp)) break;
            start += n;
        }
    }
    if (right) {
        /* Avanzamos forward guardando el último fin de codepoint
         * no-espacio. Evita necesitar utf8_prev (que no conoce
         * el decoder estricto) y mantiene una sola pasada. */
        const char *last_nonspace_end = start;
        const char *p = start;
        while (*p) {
            uint32_t cp;
            size_t n = utf8_decode_cp(p, &cp);
            if (!utf8_is_space(cp)) last_nonspace_end = p + n;
            p += n;
        }
        end = last_nonspace_end;
    }

    size_t len = (size_t)(end - start);
    char *trimmed = malloc(len + 1);
    if (!trimmed) error(current_eval_line, "memoria insuficiente en trim");
    memcpy(trimmed, start, len);
    trimmed[len] = '\0';
    return trimmed;
}

/* --- trim() --- */
static Value builtin_trim(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "trim() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "trim() espera un string.");
    char *s = do_trim(args[0].data.sval, 1, 1);
    Value res = val_string(s);
    free(s);
    return res;
}

/* --- rtrim() --- */
static Value builtin_rtrim(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "rtrim() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "rtrim() espera un string.");
    char *s = do_trim(args[0].data.sval, 0, 1);
    Value res = val_string(s);
    free(s);
    return res;
}

/* --- ltrim() --- */
static Value builtin_ltrim(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "ltrim() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "ltrim() espera un string.");
    char *s = do_trim(args[0].data.sval, 1, 0);
    Value res = val_string(s);
    free(s);
    return res;
}

/* --- trimcenter() ---
 *
 * Conserva el whitespace inicial y final tal cual, y colapsa las
 * secuencias de whitespace *interiores* a un único ' '. Ejemplo:
 *   "  h   o  " -> "  h o  "
 *   "  ñ  "     -> "  ñ  "  (un solo espacio a cada lado)
 */
static Value builtin_trimcenter(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "trimcenter() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "trimcenter() espera un string.");

    const char *src = args[0].data.sval ? args[0].data.sval : "";
    size_t src_len = strlen(src);

    /* Encontrar primer y último codepoint no-espacio. */
    const char *first_ns  = NULL;
    const char *last_ns_end = NULL;
    const char *p = src;
    while (*p) {
        uint32_t cp;
        size_t n = utf8_decode_cp(p, &cp);
        if (!utf8_is_space(cp)) {
            if (!first_ns) first_ns = p;
            last_ns_end = p + n;
        }
        p += n;
    }

    if (!first_ns) {
        /* Todo whitespace: se devuelve intacto. */
        return val_string(src);
    }

    size_t leading_len  = (size_t)(first_ns - src);
    size_t trailing_len = src_len - (size_t)(last_ns_end - src);

    /* Calcular tamaño del interior colapsado. */
    size_t interior_len = 0;
    bool in_space = false;
    p = first_ns;
    while (p < last_ns_end) {
        uint32_t cp;
        size_t n = utf8_decode_cp(p, &cp);
        if (utf8_is_space(cp)) {
            if (!in_space) { interior_len++; in_space = true; }
        } else {
            interior_len += n;
            in_space = false;
        }
        p += n;
    }

    size_t total_len = leading_len + interior_len + trailing_len;
    char *result = malloc(total_len + 1);
    if (!result) error(current_eval_line, "memoria insuficiente en trimcenter");

    char *dst = result;
    memcpy(dst, src, leading_len);
    dst += leading_len;

    in_space = false;
    p = first_ns;
    while (p < last_ns_end) {
        uint32_t cp;
        size_t n = utf8_decode_cp(p, &cp);
        if (utf8_is_space(cp)) {
            if (!in_space) { *dst++ = ' '; in_space = true; }
        } else {
            memcpy(dst, p, n);
            dst += n;
            in_space = false;
        }
        p += n;
    }

    memcpy(dst, last_ns_end, trailing_len);
    dst += trailing_len;
    *dst = '\0';

    Value res = val_string(result);
    free(result);
    return res;
}

/* --- delspaces() ---
 *
 * Elimina TODO el whitespace (UTF-8 aware). */
static Value builtin_delspaces(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "delspaces() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "delspaces() espera un string.");

    const char *src = args[0].data.sval ? args[0].data.sval : "";
    size_t len = strlen(src);
    /* La salida nunca crece: como máximo, elimina bytes. */
    char *result = malloc(len + 1);
    if (!result) error(current_eval_line, "memoria insuficiente en delspaces");

    char *dst = result;
    const char *p = src;
    while (*p) {
        uint32_t cp;
        size_t n = utf8_decode_cp(p, &cp);
        if (!utf8_is_space(cp)) {
            memcpy(dst, p, n);
            dst += n;
        }
        p += n;
    }
    *dst = '\0';

    Value res = val_string(result);
    free(result);
    return res;
}

/* --- head() --- */
static Value builtin_head(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "head requiere dos argumentos");
    if (args[0].type != VAL_STRING) error(current_eval_line, "head espera un string como primer argumento");

    const char *s = args[0].data.sval ? args[0].data.sval : "";
    size_t byte_len = strlen(s);

    if (args[1].type == VAL_INT) {
        int n = args[1].data.ival;
        if (n < 0) n = 0;

        /* Avanzo `n` codepoints (o hasta NUL). */
        const char *p = s;
        for (int i = 0; i < n && *p; i++) p = utf8_next(p);
        size_t copy_bytes = (size_t)(p - s);
        char *buf = malloc(copy_bytes + 1);
        if (!buf) error(current_eval_line, "memoria insuficiente en head");
        memcpy(buf, s, copy_bytes);
        buf[copy_bytes] = '\0';
        Value res = val_string(buf);
        free(buf);
        return res;
    }
    else if (args[1].type == VAL_STRING) {
        const char *prefix = args[1].data.sval ? args[1].data.sval : "";
        size_t plen = strlen(prefix);
        bool starts = (byte_len >= plen && memcmp(s, prefix, plen) == 0);
        return val_bool(starts);
    }
    else {
        error(current_eval_line, "head: el segundo argumento debe ser int o string");
    }
    return val_make_null();
}

/* --- tail() --- */
static Value builtin_tail(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "tail requiere dos argumentos");
    if (args[0].type != VAL_STRING) error(current_eval_line, "tail espera un string como primer argumento");

    const char *s = args[0].data.sval ? args[0].data.sval : "";
    size_t byte_len = strlen(s);

    if (args[1].type == VAL_INT) {
        int n = args[1].data.ival;
        if (n < 0) n = 0;

        /* Pasada única forward con ventana deslizante: `tail_start`
         * queda exactamente `n` codepoints por detrás de `p` (o en
         * `s` si aún no se han visto `n`). Evita utf8_prev, que no
         * conoce el decoder estricto. */
        const char *p = s;
        const char *tail_start = s;
        size_t seen = 0;
        while (*p) {
            p = utf8_next(p);
            if (seen >= (size_t)n) tail_start = utf8_next(tail_start);
            seen++;
        }
        size_t copy_bytes = (size_t)((s + byte_len) - tail_start);
        char *buf = malloc(copy_bytes + 1);
        if (!buf) error(current_eval_line, "memoria insuficiente en tail");
        memcpy(buf, tail_start, copy_bytes);
        buf[copy_bytes] = '\0';
        Value res = val_string(buf);
        free(buf);
        return res;
    }
    else if (args[1].type == VAL_STRING) {
        const char *suffix = args[1].data.sval ? args[1].data.sval : "";
        size_t slen = strlen(suffix);
        bool ends = (byte_len >= slen && memcmp(s + byte_len - slen, suffix, slen) == 0);
        return val_bool(ends);
    }
    else {
        error(current_eval_line, "tail: el segundo argumento debe ser int o string");
    }
    return val_make_null();
}

/* --- starts() --- */
static Value builtin_starts(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "starts() espera exactamente 2 argumentos");
    if (args[0].type != VAL_STRING) error(current_eval_line, "starts() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(current_eval_line, "starts() espera un string como segundo argumento");

    const char *s = args[0].data.sval ? args[0].data.sval : "";
    const char *prefix = args[1].data.sval ? args[1].data.sval : "";
    size_t slen = strlen(s);
    size_t plen = strlen(prefix);
    return val_bool(slen >= plen && memcmp(s, prefix, plen) == 0);
}

/* --- ends() --- */
static Value builtin_ends(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "ends() espera exactamente 2 argumentos");
    if (args[0].type != VAL_STRING) error(current_eval_line, "ends() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(current_eval_line, "ends() espera un string como segundo argumento");

    const char *s = args[0].data.sval ? args[0].data.sval : "";
    const char *suffix = args[1].data.sval ? args[1].data.sval : "";
    size_t slen = strlen(s);
    size_t suflen = strlen(suffix);
    if (slen < suflen) return val_bool(false);
    return val_bool(memcmp(s + slen - suflen, suffix, suflen) == 0);
}


/* ================================================
 *  Registro de funciones
 * ================================================ */

void register_string_builtins(void) {
    func_register_builtin("head", builtin_head);
    func_register_builtin("tail", builtin_tail);
    func_register_builtin("lower", builtin_lower);
    func_register_builtin("upper", builtin_upper);
    func_register_builtin("count", builtin_count);
    func_register_builtin("indexof", builtin_indexof);
    func_register_builtin("replace", builtin_replace);
    func_register_builtin("reverse", builtin_reverse);
    func_register_builtin("join", builtin_join);
    func_register_builtin("trim", builtin_trim);
    func_register_builtin("rtrim", builtin_rtrim);
    func_register_builtin("ltrim", builtin_ltrim);
    func_register_builtin("trimcenter", builtin_trimcenter);
    func_register_builtin("delspaces", builtin_delspaces);
    func_register_builtin("starts", builtin_starts);
    func_register_builtin("ends", builtin_ends);
    func_register_builtin("capitalize", builtin_capitalize);
    func_register_builtin("length", builtin_length);
}
