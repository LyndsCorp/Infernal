/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, Lynds Corp., David Baña Szymaniak
 * Licencia GPL v3 o posterior
 * Código fuente de Infernal: stdlib/string.c
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

/* ======================================================================
 *  Ayudantes UTF‑8
 * ====================================================================== */

/* cuenta los puntos de código (caracteres) de una cadena UTF‑8 */
static size_t utf8_len(const char *s) {
    size_t n = 0;
    while (*s) {
        if ((*(unsigned char*)s & 0xC0) != 0x80) n++;
        s++;
    }
    return n;
}

/* avanza p hasta el siguiente carácter UTF‑8 (p nunca es NULL) */
static const char* utf8_next(const char *p) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x80) return p + 1;
    if ((c & 0xE0) == 0xC0) return p + 2;
    if ((c & 0xF0) == 0xE0) return p + 3;
    if ((c & 0xF8) == 0xF0) return p + 4;
    return p + 1;  /* fallback, no debería ocurrir */
}

/* retrocede p hasta el inicio del carácter UTF‑8 anterior (p > str) */
static const char* utf8_prev(const char *str, const char *p) {
    p--;
    while (p > str && (*(unsigned char*)p & 0xC0) == 0x80) p--;
    return p;
}

/* Estructura auxiliar para manejar segmentos de caracteres */
typedef struct {
    const char *start;   /* inicio del carácter en la cadena original */
    int         len;     /* longitud en bytes del carácter */
} CharSegment;

/* Convierte una cadena UTF‑8 en un array de segmentos (caracteres) */
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
            if (!tmp) {
                free(segs);
                return NULL;
            }
            segs = tmp;
        }
        segs[n].start = start;
        segs[n].len   = (int)(p - start);
        n++;
    }
    *count = n;
    return segs;
}

/* Compara dos segmentos de caracteres (byte a byte) */
static bool seg_equal(const CharSegment *a, const CharSegment *b) {
    if (a->len != b->len) return false;
    return memcmp(a->start, b->start, a->len) == 0;
}

/* ======================================================================
 *  Funciones de la biblioteca
 * ====================================================================== */

static Value builtin_lower(int argc, Value *args) {
    if (argc != 1) error(0, "lower() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(0, "lower() espera un string.");
    /* Nota: lower() solo maneja caracteres ASCII (A-Z → a-z).
     *      Para soporte Unicode completo se necesitaría una biblioteca como ICU. */
    char *s = strdup(args[0].data.sval);
    if (!s) error(0, "memoria insuficiente");
    for (char *p = s; *p; p++) *p = tolower((unsigned char)*p);
    Value res = val_string(s);
    free(s);
    return res;
}

static Value builtin_upper(int argc, Value *args) {
    if (argc != 1) error(0, "upper() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(0, "upper() espera un string.");
    /* Nota: upper() solo maneja caracteres ASCII (a-z → A-Z).
     *      Para soporte Unicode completo se necesitaría una biblioteca como ICU. */
    char *s = strdup(args[0].data.sval);
    if (!s) error(0, "memoria insuficiente");
    for (char *p = s; *p; p++) *p = toupper((unsigned char)*p);
    Value res = val_string(s);
    free(s);
    return res;
}

static Value builtin_capitalize(int argc, Value *args) {
    if (argc != 1) error(0, "capitalize() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(0, "capitalize() espera un string.");

    const char *s = args[0].data.sval;
    if (!s || *s == '\0') return val_string("");

    char *result = strdup(s);
    if (!result) error(0, "memoria insuficiente en capitalize");

    // Convertir la primera letra a mayúscula (ASCII)
    result[0] = toupper((unsigned char)result[0]);

    // Convertir el resto a minúsculas
    for (char *p = result + 1; *p; p++) {
        *p = tolower((unsigned char)*p);
    }

    Value res = val_string(result);
    free(result);
    return res;
}

static Value builtin_countbytes(int argc, Value *args) {
    if (argc != 1) error(0, "countbytes() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(0, "countbytes() espera un string.");
    size_t len = strlen(args[0].data.sval);
    return val_int((int)len);
}

static Value builtin_length(int argc, Value *args) {
    if (argc != 1) error(0, "length() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(0, "length() espera un string.");
    size_t len = utf8_len(args[0].data.sval);
    return val_int((int)len);
}

static Value builtin_count(int argc, Value *args) {
    if (argc != 2) error(0, "count() espera exactamente 2 argumentos");
    if (args[0].type != VAL_STRING) error(0, "count() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(0, "count() espera un string como segundo argumento");

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

/* ─── replace (versión carácter a carácter, UTF‑8 seguro) ─── */
static Value builtin_replace(int argc, Value *args) {
    if (argc != 3) error(0, "replace() espera exactamente 3 argumentos");
    if (args[0].type != VAL_STRING) error(0, "replace() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(0, "replace() espera un string como segundo argumento (a reemplazar)");
    if (args[2].type != VAL_STRING) error(0, "replace() espera un string como tercer argumento (reemplazo)");

    const char *src      = args[0].data.sval;
    const char *from_str = args[1].data.sval;
    const char *to_str   = args[2].data.sval;

    if (from_str[0] == '\0') return val_string(src);

    int src_seg_count, from_seg_count, to_seg_count;
    CharSegment *src_segs  = utf8_to_segments(src, &src_seg_count);
    if (!src_segs) error(0, "memoria insuficiente en replace");
    CharSegment *from_segs = utf8_to_segments(from_str, &from_seg_count);
    if (!from_segs) { free(src_segs); error(0, "memoria insuficiente en replace"); }
    CharSegment *to_segs   = utf8_to_segments(to_str, &to_seg_count);
    if (!to_segs) { free(src_segs); free(from_segs); error(0, "memoria insuficiente en replace"); }

    int *replace_pos = malloc(src_seg_count * sizeof(int));
    if (!replace_pos) { free(src_segs); free(from_segs); free(to_segs); error(0, "memoria insuficiente en replace"); }

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
        for (int i = current; i < pos; i++) {
            total_bytes += src_segs[i].len;
        }
        for (int i = 0; i < to_seg_count; i++) {
            total_bytes += to_segs[i].len;
        }
        current = pos + from_seg_count;
    }
    for (int i = current; i < src_seg_count; i++) {
        total_bytes += src_segs[i].len;
    }

    char *result = malloc(total_bytes + 1);
    if (!result) { free(src_segs); free(from_segs); free(to_segs); free(replace_pos); error(0, "memoria insuficiente en replace"); }

    char *dst = result;
    current = 0;
    for (int m = 0; m < match_count; m++) {
        int pos = replace_pos[m];
        for (int i = current; i < pos; i++) {
            memcpy(dst, src_segs[i].start, src_segs[i].len);
            dst += src_segs[i].len;
        }
        for (int i = 0; i < to_seg_count; i++) {
            memcpy(dst, to_segs[i].start, to_segs[i].len);
            dst += to_segs[i].len;
        }
        current = pos + from_seg_count;
    }
    for (int i = current; i < src_seg_count; i++) {
        memcpy(dst, src_segs[i].start, src_segs[i].len);
        dst += src_segs[i].len;
    }
    *dst = '\0';

    Value res = val_string(result);
    free(result);
    free(src_segs);
    free(from_segs);
    free(to_segs);
    free(replace_pos);
    return res;
}

/* ─── replacebytes (versión byte a byte, como antes) ─── */
static Value builtin_replacebytes(int argc, Value *args) {
    if (argc != 3) error(0, "replacebytes() espera exactamente 3 argumentos");
    if (args[0].type != VAL_STRING) error(0, "replacebytes() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(0, "replacebytes() espera un string como segundo argumento");
    if (args[2].type != VAL_STRING) error(0, "replacebytes() espera un string como tercer argumento");

    const char *str = args[0].data.sval;
    const char *from = args[1].data.sval;
    const char *to = args[2].data.sval;

    size_t from_len = strlen(from);
    if (from_len == 0) return val_string(str);

    size_t count = 0;
    const char *tmp = str;
    while ((tmp = strstr(tmp, from)) != NULL) {
        count++;
        tmp += from_len;
    }

    size_t to_len = strlen(to);
    size_t original_len = strlen(str);
    size_t result_len;

    if (to_len >= from_len) {
        size_t diff = to_len - from_len;
        if (diff > 0 && count > SIZE_MAX / diff)
            error(0, "replacebytes() resultado demasiado grande");
        size_t added = count * diff;
        if (added > SIZE_MAX - original_len - 1)
            error(0, "replacebytes() resultado demasiado grande");
        result_len = original_len + added + 1;
    } else {
        size_t diff = from_len - to_len;
        if (diff > 0 && count > SIZE_MAX / diff)
            error(0, "replacebytes() resultado demasiado grande");
        size_t reduction = count * diff;
        if (reduction > original_len)
            error(0, "replacebytes() inconsistencia interna");
        result_len = original_len - reduction + 1;
    }

    char *result = (char*)malloc(result_len);
    if (!result) error(0, "memoria insuficiente en replacebytes");

    const char *read_ptr = str;
    char *write_ptr = result;
    while (*read_ptr) {
        const char *found = strstr(read_ptr, from);
        if (found == read_ptr) {
            memcpy(write_ptr, to, to_len);
            write_ptr += to_len;
            read_ptr += from_len;
        } else {
            const char *next = found ? found : read_ptr + strlen(read_ptr);
            size_t chunk = next - read_ptr;
            memcpy(write_ptr, read_ptr, chunk);
            write_ptr += chunk;
            read_ptr = next;
        }
    }
    *write_ptr = '\0';

    Value res = val_string(result);
    free(result);
    return res;
}

/* ─── reverse (caracteres) usando utf8_to_segments ─── */
static Value builtin_reverse(int argc, Value *args) {
    if (argc != 1) error(0, "reverse() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(0, "reverse() espera un string.");

    const char *src = args[0].data.sval;
    int seg_count;
    CharSegment *segs = utf8_to_segments(src, &seg_count);
    if (!segs) error(0, "memoria insuficiente en reverse");

    size_t total_len = 0;
    for (int i = 0; i < seg_count; i++) {
        total_len += segs[i].len;
    }

    char *rev = malloc(total_len + 1);
    if (!rev) { free(segs); error(0, "memoria insuficiente en reverse"); }

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

/* ─── reversebytes (bytes) ─── */
static Value builtin_reversebytes(int argc, Value *args) {
    if (argc != 1) error(0, "reversebytes() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(0, "reversebytes() espera un string.");

    const char *src = args[0].data.sval;
    size_t len = strlen(src);
    char *rev = (char*)malloc(len + 1);
    if (!rev) error(0, "memoria insuficiente en reversebytes");
    for (size_t i = 0; i < len; i++) {
        rev[i] = src[len - 1 - i];
    }
    rev[len] = '\0';
    Value res = val_string(rev);
    free(rev);
    return res;
}

/* ─── join ─── */
static Value builtin_join(int argc, Value *args) {
    if (argc != 2) error(0, "join() espera exactamente 2 argumentos");
    if (args[0].type != VAL_LIST) error(0, "join() espera una lista como primer argumento");
    if (args[1].type != VAL_STRING) error(0, "join() espera un string como segundo argumento");

    int n = args[0].data.list.count;
    const char *sep = args[1].data.sval ? args[1].data.sval : "";
    size_t sep_len = strlen(sep);

    size_t total_len = 0;
    for (int i = 0; i < n; i++) {
        if (args[0].data.list.items[i].type != VAL_STRING)
            error(0, "join() solo admite listas de strings");
        size_t elem_len = strlen(args[0].data.list.items[i].data.sval);
        if (total_len > SIZE_MAX - elem_len)
            error(0, "join() resultado demasiado grande");
        total_len += elem_len;
    }
    if (n > 1) {
        if (sep_len > 0 && (size_t)(n - 1) > SIZE_MAX / sep_len)
            error(0, "join() resultado demasiado grande");
        size_t sep_total = (n - 1) * sep_len;
        if (total_len > SIZE_MAX - sep_total)
            error(0, "join() resultado demasiado grande");
        total_len += sep_total;
    }
    if (total_len == SIZE_MAX) error(0, "join() resultado demasiado grande");
    total_len += 1;

    char *buf = (char*)malloc(total_len);
    if (!buf) error(0, "memoria insuficiente en join");

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

/* ─── trim helpers ─── */
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
    if (!trimmed) error(0, "memoria insuficiente en trim");
    memcpy(trimmed, start, len);
    trimmed[len] = '\0';
    return trimmed;
}

static Value builtin_trim(int argc, Value *args) {
    if (argc != 1) error(0, "trim() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(0, "trim() espera un string.");
    char *s = do_trim(args[0].data.sval, 1, 1);
    Value res = val_string(s);
    free(s);
    return res;
}

static Value builtin_rtrim(int argc, Value *args) {
    if (argc != 1) error(0, "rtrim() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(0, "rtrim() espera un string.");
    char *s = do_trim(args[0].data.sval, 0, 1);
    Value res = val_string(s);
    free(s);
    return res;
}

static Value builtin_ltrim(int argc, Value *args) {
    if (argc != 1) error(0, "ltrim() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(0, "ltrim() espera un string.");
    char *s = do_trim(args[0].data.sval, 1, 0);
    Value res = val_string(s);
    free(s);
    return res;
}

/* ─── head ─── */
static Value builtin_head(int argc, Value *args) {
    if (argc != 2) error(0, "head requiere dos argumentos");
    if (args[0].type != VAL_STRING) error(0, "head espera un string como primer argumento");

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
        if (!buf) error(0, "memoria insuficiente en head");
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
        error(0, "head: el segundo argumento debe ser int o string");
    }
    return val_make_null();
}

static Value builtin_headbytes(int argc, Value *args) {
    if (argc != 2) error(0, "headbytes requiere dos argumentos");
    if (args[0].type != VAL_STRING) error(0, "headbytes espera un string como primer argumento");
    if (args[1].type != VAL_INT) error(0, "headbytes espera un entero como segundo argumento");

    const char *s = args[0].data.sval;
    int n = args[1].data.ival;
    if (n < 0) n = 0;
    size_t len = strlen(s);
    if ((size_t)n > len) n = (int)len;
    char *buf = (char*)malloc(n + 1);
    if (!buf) error(0, "memoria insuficiente en headbytes");
    memcpy(buf, s, n);
    buf[n] = '\0';
    Value res = val_string(buf);
    free(buf);
    return res;
}

/* ─── tail ─── */
static Value builtin_tail(int argc, Value *args) {
    if (argc != 2) error(0, "tail requiere dos argumentos");
    if (args[0].type != VAL_STRING) error(0, "tail espera un string como primer argumento");

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
        if (!buf) error(0, "memoria insuficiente en tail");
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
        error(0, "tail: el segundo argumento debe ser int o string");
    }
    return val_make_null();
}

static Value builtin_tailbytes(int argc, Value *args) {
    if (argc != 2) error(0, "tailbytes requiere dos argumentos");
    if (args[0].type != VAL_STRING) error(0, "tailbytes espera un string como primer argumento");
    if (args[1].type != VAL_INT) error(0, "tailbytes espera un entero como segundo argumento");

    const char *s = args[0].data.sval;
    int n = args[1].data.ival;
    if (n < 0) n = 0;
    size_t len = strlen(s);
    if ((size_t)n > len) n = (int)len;
    size_t start = len - n;
    char *buf = (char*)malloc(n + 1);
    if (!buf) error(0, "memoria insuficiente en tailbytes");
    memcpy(buf, s + start, n);
    buf[n] = '\0';
    Value res = val_string(buf);
    free(buf);
    return res;
}

/* ─── starts, ends, has ─── */
static Value builtin_starts(int argc, Value *args) {
    if (argc != 2) error(0, "starts() espera exactamente 2 argumentos");
    if (args[0].type != VAL_STRING) error(0, "starts() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(0, "starts() espera un string como segundo argumento");

    const char *s = args[0].data.sval;
    const char *prefix = args[1].data.sval;
    size_t slen = strlen(s);
    size_t plen = strlen(prefix);
    return val_bool(slen >= plen && strncmp(s, prefix, plen) == 0);
}

static Value builtin_ends(int argc, Value *args) {
    if (argc != 2) error(0, "ends() espera exactamente 2 argumentos");
    if (args[0].type != VAL_STRING) error(0, "ends() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(0, "ends() espera un string como segundo argumento");

    const char *s = args[0].data.sval;
    const char *suffix = args[1].data.sval;
    size_t slen = strlen(s);
    size_t suflen = strlen(suffix);
    if (slen < suflen) return val_bool(false);
    return val_bool(strcmp(s + slen - suflen, suffix) == 0);
}

static Value builtin_has(int argc, Value *args) {
    if (argc != 2) error(0, "has() espera exactamente 2 argumentos");
    if (args[0].type != VAL_STRING) error(0, "has() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(0, "has() espera un string como segundo argumento");

    const char *s = args[0].data.sval;
    const char *sub = args[1].data.sval;
    return val_bool(strstr(s, sub) != NULL);
}

/* ======================================================================
 *  Registro de funciones
 * ====================================================================== */
void register_string_builtins(void) {
    func_register_builtin("head", builtin_head);
    func_register_builtin("headbytes", builtin_headbytes);
    func_register_builtin("tail", builtin_tail);
    func_register_builtin("tailbytes", builtin_tailbytes);
    func_register_builtin("lower", builtin_lower);
    func_register_builtin("upper", builtin_upper);
    func_register_builtin("length", builtin_length);
    func_register_builtin("countbytes", builtin_countbytes);
    func_register_builtin("count", builtin_count);
    func_register_builtin("replace", builtin_replace);
    func_register_builtin("replacebytes", builtin_replacebytes);
    func_register_builtin("reverse", builtin_reverse);
    func_register_builtin("reversebytes", builtin_reversebytes);
    func_register_builtin("join", builtin_join);
    func_register_builtin("trim", builtin_trim);
    func_register_builtin("rtrim", builtin_rtrim);
    func_register_builtin("ltrim", builtin_ltrim);
    func_register_builtin("starts", builtin_starts);
    func_register_builtin("ends", builtin_ends);
    func_register_builtin("has", builtin_has);
    func_register_builtin("capitalize", builtin_capitalize);

    vm_register_builtin("head", builtin_head);
    vm_register_builtin("headbytes", builtin_headbytes);
    vm_register_builtin("tail", builtin_tail);
    vm_register_builtin("tailbytes", builtin_tailbytes);
    vm_register_builtin("lower", builtin_lower);
    vm_register_builtin("upper", builtin_upper);
    vm_register_builtin("length", builtin_length);
    vm_register_builtin("countbytes", builtin_countbytes);
    vm_register_builtin("count", builtin_count);
    vm_register_builtin("replace", builtin_replace);
    vm_register_builtin("replacebytes", builtin_replacebytes);
    vm_register_builtin("reverse", builtin_reverse);
    vm_register_builtin("reversebytes", builtin_reversebytes);
    vm_register_builtin("join", builtin_join);
    vm_register_builtin("trim", builtin_trim);
    vm_register_builtin("rtrim", builtin_rtrim);
    vm_register_builtin("ltrim", builtin_ltrim);
    vm_register_builtin("starts", builtin_starts);
    vm_register_builtin("ends", builtin_ends);
    vm_register_builtin("has", builtin_has);
    vm_register_builtin("capitalize", builtin_capitalize);
}
