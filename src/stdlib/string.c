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
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include "string.h"
#include "core/value.h"
#include "runtime/error.h"
#include "runtime/globals.h"
#include "vm/vm.h"


/* ================================================
 *  Ayudantes UTF‑8
 * ================================================ */

/* --- calcular cuantos caracteres UTF-8 hay --- */
static size_t utf8_len(const char *s) {
    size_t n = 0;
    while (*s) {
        if ((*(unsigned char*)s & 0xC0) != 0x80) n++;
        s++;
    }
    return n;
}

/* --- devolver el inicio del siguiente caracter UTF-8 --- */
static const char* utf8_next(const char *p) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x80) return p + 1;
    if ((c & 0xE0) == 0xC0) return p + 2;
    if ((c & 0xF0) == 0xE0) return p + 3;
    if ((c & 0xF8) == 0xF0) return p + 4;
    return p + 1;
}

/* --- devolver el inicio del caracter UTF-8 anterior --- */
static const char* utf8_prev(const char *str, const char *p) {
    p--;
    while (p > str && (*(unsigned char*)p & 0xC0) == 0x80) p--;
    return p;
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
 *  Funciones de la biblioteca
 * ================================================ */

/* --- lower() --- */
static Value builtin_lower(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "lower() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "lower() espera un string.");
    char *s = strdup(args[0].data.sval);
    if (!s) error(current_eval_line, "memoria insuficiente");
    for (char *p = s; *p; p++) *p = tolower((unsigned char)*p);
    Value res = val_string(s);
    free(s);
    return res;
}

/* --- upper() --- */
static Value builtin_upper(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "upper() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "upper() espera un string.");
    char *s = strdup(args[0].data.sval);
    if (!s) error(current_eval_line, "memoria insuficiente");
    for (char *p = s; *p; p++) *p = toupper((unsigned char)*p);
    Value res = val_string(s);
    free(s);
    return res;
}

/* --- capitalize() --- */
static Value builtin_capitalize(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "capitalize() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "capitalize() espera un string.");

    const char *s = args[0].data.sval;
    if (!s || *s == '\0') return val_string("");

    char *result = strdup(s);
    if (!result) error(current_eval_line, "memoria insuficiente en capitalize");

    result[0] = toupper((unsigned char)result[0]);
    for (char *p = result + 1; *p; p++) {
        *p = tolower((unsigned char)*p);
    }

    Value res = val_string(result);
    free(result);
    return res;
}

/* --- count() --- */
static Value builtin_count(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "count() espera exactamente 2 argumentos");
    if (args[0].type != VAL_STRING) error(current_eval_line, "count() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(current_eval_line, "count() espera un string como segundo argumento");

    const char *haystack = args[0].data.sval;
    const char *needle   = args[1].data.sval;
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return val_int(0);

    int count = 0;
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    return val_int(count);
}

/* --- indexof() --- */
static Value builtin_indexof(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "indexof() espera exactamente 2 argumentos");
    if (args[0].type != VAL_STRING) error(current_eval_line, "indexof() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(current_eval_line, "indexof() espera un string como segundo argumento");

    const char *haystack = args[0].data.sval;
    const char *needle   = args[1].data.sval;

    // Si needle está vacío, devuelve 0 (por convención)
    if (*needle == '\0') return val_int(0);

    int hay_count, needle_count;
    CharSegment *hay_segs = utf8_to_segments(haystack, &hay_count);
    if (!hay_segs) error(current_eval_line, "memoria insuficiente en indexof");
    CharSegment *needle_segs = utf8_to_segments(needle, &needle_count);
    if (!needle_segs) {
        free(hay_segs);
        error(current_eval_line, "memoria insuficiente en indexof");
    }

    int result = -1;
    if (needle_count > hay_count) {
        // no puede haber coincidencia
    } else {
        for (int i = 0; i <= hay_count - needle_count; i++) {
            bool match = true;
            for (int j = 0; j < needle_count; j++) {
                if (!seg_equal(&hay_segs[i + j], &needle_segs[j])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                result = i;  // índice en caracteres
                break;
            }
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

    const char *src      = args[0].data.sval;
    const char *from_str = args[1].data.sval;
    const char *to_str   = args[2].data.sval;
    if (from_str[0] == '\0') return val_string(src);

    int src_seg_count, from_seg_count, to_seg_count;
    CharSegment *src_segs  = utf8_to_segments(src, &src_seg_count);
    if (!src_segs) error(current_eval_line, "memoria insuficiente en replace");
    CharSegment *from_segs = utf8_to_segments(from_str, &from_seg_count);
    if (!from_segs) { free(src_segs); error(current_eval_line, "memoria insuficiente en replace"); }
    CharSegment *to_segs   = utf8_to_segments(to_str, &to_seg_count);
    if (!to_segs) { free(src_segs); free(from_segs); error(current_eval_line, "memoria insuficiente en replace"); }

    int *replace_pos = malloc(src_seg_count * sizeof(int));
    if (!replace_pos) { free(src_segs); free(from_segs); free(to_segs); error(current_eval_line, "memoria insuficiente en replace"); }

    int match_count = 0;
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

    size_t total_bytes = 0;
    int current = 0;
    for (int m = 0; m < match_count; m++) {
        int pos = replace_pos[m];
        for (int i = current; i < pos; i++) total_bytes += src_segs[i].len;
        for (int i = 0; i < to_seg_count; i++) total_bytes += to_segs[i].len;
        current = pos + from_seg_count;
    }
    for (int i = current; i < src_seg_count; i++) total_bytes += src_segs[i].len;

    char *result = malloc(total_bytes + 1);
    if (!result) { free(src_segs); free(from_segs); free(to_segs); free(replace_pos); error(current_eval_line, "memoria insuficiente en replace"); }

    char *dst = result;
    current = 0;
    for (int m = 0; m < match_count; m++) {
        int pos = replace_pos[m];
        for (int i = current; i < pos; i++) { memcpy(dst, src_segs[i].start, src_segs[i].len); dst += src_segs[i].len; }
        for (int i = 0; i < to_seg_count; i++) { memcpy(dst, to_segs[i].start, to_segs[i].len); dst += to_segs[i].len; }
        current = pos + from_seg_count;
    }
    for (int i = current; i < src_seg_count; i++) { memcpy(dst, src_segs[i].start, src_segs[i].len); dst += src_segs[i].len; }
    *dst = '\0';

    Value res = val_string(result);
    free(result);
    free(src_segs); free(from_segs); free(to_segs); free(replace_pos);
    return res;
}

/* --- reverse() -- */
static Value builtin_reverse(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "reverse() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "reverse() espera un string.");

    const char *src = args[0].data.sval;
    int seg_count;
    CharSegment *segs = utf8_to_segments(src, &seg_count);
    if (!segs) error(current_eval_line, "memoria insuficiente en reverse");

    size_t total_len = 0;
    for (int i = 0; i < seg_count; i++) total_len += segs[i].len;

    char *rev = malloc(total_len + 1);
    if (!rev) { free(segs); error(current_eval_line, "memoria insuficiente en reverse"); }

    char *dst = rev;
    for (int i = seg_count - 1; i >= 0; i--) {
        memcpy(dst, segs[i].start, segs[i].len);
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
        size_t elem_len = strlen(args[0].data.list.items[i].data.sval);
        if (total_len > SIZE_MAX - elem_len) error(current_eval_line, "join() resultado demasiado grande");
        total_len += elem_len;
    }
    if (n > 1) {
        if (sep_len > 0 && (size_t)(n - 1) > SIZE_MAX / sep_len)
            error(current_eval_line, "join() resultado demasiado grande");
        size_t sep_total = (n - 1) * sep_len;
        if (total_len > SIZE_MAX - sep_total) error(current_eval_line, "join() resultado demasiado grande");
        total_len += sep_total;
    }
    if (total_len == SIZE_MAX) error(current_eval_line, "join() resultado demasiado grande");
    total_len += 1;

    char *buf = (char*)malloc(total_len);
    if (!buf) error(current_eval_line, "memoria insuficiente en join");

    char *ptr = buf;
    for (int i = 0; i < n; i++) {
        const char *elem = args[0].data.list.items[i].data.sval;
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

/* --- trim helpers --- */
static char* do_trim(const char *str, int left, int right) {
    const char *start = str;
    const char *end = str + strlen(str);

    if (left) {
        while (start < end && isspace((unsigned char)*start)) start++;
    }
    if (right) {
        while (end > start && isspace((unsigned char)*(end - 1))) end--;
    }

    size_t len = end - start;
    char *trimmed = (char*)malloc(len + 1);
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

/* --- trimcenter() --- */
static Value builtin_trimcenter(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "trimcenter() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "trimcenter() espera un string.");

    const char *src = args[0].data.sval;
    size_t len = strlen(src);

    const char *first = NULL;
    const char *last  = NULL;
    for (const char *p = src; *p; ++p) {
        if (!isspace((unsigned char)*p)) {
            if (!first) first = p;
            last = p;
        }
    }

    if (!first) return val_string(src);

    size_t leading_len  = first - src;
    size_t trailing_len = (src + len) - (last + 1);

    size_t interior_len = 0;
    bool in_space = false;
    for (const char *p = first; p <= last; ++p) {
        if (isspace((unsigned char)*p)) {
            if (!in_space) {
                interior_len++;
                in_space = true;
            }
        } else {
            interior_len++;
            in_space = false;
        }
    }

    size_t total_len = leading_len + interior_len + trailing_len;
    char *result = malloc(total_len + 1);
    if (!result) error(current_eval_line, "memoria insuficiente en trimcenter");

    char *dst = result;

    memcpy(dst, src, leading_len);
    dst += leading_len;

    in_space = false;
    for (const char *p = first; p <= last; ++p) {
        if (isspace((unsigned char)*p)) {
            if (!in_space) {
                *dst++ = ' ';
                in_space = true;
            }
        } else {
            *dst++ = *p;
            in_space = false;
        }
    }

    memcpy(dst, last + 1, trailing_len);
    dst += trailing_len;
    *dst = '\0';

    Value res = val_string(result);
    free(result);
    return res;
}

/* --- head() --- */
static Value builtin_head(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "head requiere dos argumentos");
    if (args[0].type != VAL_STRING) error(current_eval_line, "head espera un string como primer argumento");

    const char *s = args[0].data.sval;
    size_t byte_len = strlen(s);

    if (args[1].type == VAL_INT) {
        int n = args[1].data.ival;
        if (n < 0) n = 0;

        const char *p = s;
        size_t chars = 0;
        while (*p && chars < (size_t)n) {
            p = utf8_next(p);
            chars++;
        }
        size_t copy_bytes = p - s;
        char *buf = (char*)malloc(copy_bytes + 1);
        if (!buf) error(current_eval_line, "memoria insuficiente en head");
        memcpy(buf, s, copy_bytes);
        buf[copy_bytes] = '\0';
        Value res = val_string(buf);
        free(buf);
        return res;
    }
    else if (args[1].type == VAL_STRING) {
        const char *prefix = args[1].data.sval;
        size_t plen = strlen(prefix);
        bool starts = (byte_len >= plen && strncmp(s, prefix, plen) == 0);
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

    const char *s = args[0].data.sval;
    size_t byte_len = strlen(s);

    if (args[1].type == VAL_INT) {
        int n = args[1].data.ival;
        if (n < 0) n = 0;

        const char *p = s + byte_len;
        size_t chars = 0;
        while (p > s && chars < (size_t)n) {
            p = utf8_prev(s, p);
            chars++;
        }
        size_t copy_bytes = (s + byte_len) - p;
        char *buf = (char*)malloc(copy_bytes + 1);
        if (!buf) error(current_eval_line, "memoria insuficiente en tail");
        memcpy(buf, p, copy_bytes);
        buf[copy_bytes] = '\0';
        Value res = val_string(buf);
        free(buf);
        return res;
    }
    else if (args[1].type == VAL_STRING) {
        const char *suffix = args[1].data.sval;
        size_t slen = strlen(suffix);
        bool ends = (byte_len >= slen && strcmp(s + byte_len - slen, suffix) == 0);
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

    const char *s = args[0].data.sval;
    const char *prefix = args[1].data.sval;
    size_t slen = strlen(s);
    size_t plen = strlen(prefix);
    return val_bool(slen >= plen && strncmp(s, prefix, plen) == 0);
}

/* --- ends() --- */
static Value builtin_ends(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "ends() espera exactamente 2 argumentos");
    if (args[0].type != VAL_STRING) error(current_eval_line, "ends() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(current_eval_line, "ends() espera un string como segundo argumento");

    const char *s = args[0].data.sval;
    const char *suffix = args[1].data.sval;
    size_t slen = strlen(s);
    size_t suflen = strlen(suffix);
    if (slen < suflen) return val_bool(false);
    return val_bool(strcmp(s + slen - suflen, suffix) == 0);
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
    func_register_builtin("starts", builtin_starts);
    func_register_builtin("ends", builtin_ends);
    func_register_builtin("capitalize", builtin_capitalize);
    func_register_builtin("length", builtin_length);

    vm_register_builtin("head", builtin_head);
    vm_register_builtin("tail", builtin_tail);
    vm_register_builtin("lower", builtin_lower);
    vm_register_builtin("upper", builtin_upper);
    vm_register_builtin("count", builtin_count);
    vm_register_builtin("indexof", builtin_indexof);
    vm_register_builtin("replace", builtin_replace);
    vm_register_builtin("reverse", builtin_reverse);
    vm_register_builtin("join", builtin_join);
    vm_register_builtin("trim", builtin_trim);
    vm_register_builtin("rtrim", builtin_rtrim);
    vm_register_builtin("ltrim", builtin_ltrim);
    vm_register_builtin("trimcenter", builtin_trimcenter);
    vm_register_builtin("starts", builtin_starts);
    vm_register_builtin("ends", builtin_ends);
    vm_register_builtin("capitalize", builtin_capitalize);
    vm_register_builtin("length", builtin_length);
}
