/*
 * Librería Lava para Infernal base: bytes
 *
 * Este software se distribuye bajo la licencia Apache 2.0
 * Copyright (C) 2026, David Baña Szymaniak
 *
 * Uso desde Infernal:
 *
 *   import bytes
 *
 *   print(bytes.countbytes("hola"))              # 4
 *   print(bytes.indexofbytes("hola", "la"))      # 3
 *   print(bytes.headbytes("hola mundo", 4))      # "hola"
 *   print(bytes.tailbytes("hola mundo", 5))      # "mundo"
 *   print(bytes.replacebytes("a-b-c", "-", "+")) # "a+b+c"
 *   print(bytes.reversebytes("abc"))             # "cba"
 *   print(bytes.lengthbytes("hola"))             # 4
 *   print(bytes.binbytes("AB"))                  # "01000001 01000010"
 *   print(bytes.hexbytes("AB"))                  # "41 42"
 *   print(bytes.utf8bytes("ñ"))                  # "195 177"
 *   print(bytes.unicodeCodepoints("ñ"))          # "U+00F1"
 *   print(bytes.cutAfterbytes("a/b/c", "/"))     # "a/"
 *   print(bytes.cutBeforebytes("a/b/c", "/"))    # "/b/c"
 *   print(bytes.cutHeadbytes("abcdef", 2))       # "cdef"
 *   print(bytes.cutTailbytes("abcdef", 2))       # "abcd"
 */

#include "lava.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>

/* ==================================================================
 * Dependencias del intérprete (visibles gracias a -rdynamic)
 * ================================================================== */

extern int current_eval_line;
extern void error(int line, const char *fmt, ...) __attribute__((noreturn));

/* ==================================================================
 * Helper de error
 * ================================================================== */

static void raise(const char *fmt, ...) __attribute__((noreturn));

static void raise(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    error(current_eval_line, "%s", buf);
}

/* ==================================================================
 * API pública
 * ================================================================== */

/* countbytes(s) — número de bytes (== strlen). */
LAVA_EXPORT void lava_countbytes(const char *s) {
    infernal_return_type("int");
    infernal_return_value("%d", (int)strlen(s));
}

/* indexofbytes(haystack, needle) — posición 1-based, 0 si no se encuentra.
 * Subcadena vacía → 1 (como en el original). */
LAVA_EXPORT void lava_indexofbytes(const char *haystack, const char *needle) {
    if (*needle == '\0') {
        infernal_return_type("int");
        infernal_return_value("%d", 1);
        return;
    }
    const char *found = strstr(haystack, needle);
    infernal_return_type("int");
    infernal_return_value("%d", found ? (int)(found - haystack) + 1 : 0);
}

/* headbytes(s, n) — primeros n bytes. n se recorta a [0, len]. */
LAVA_EXPORT void lava_headbytes(const char *s, int n) {
    if (n < 0) n = 0;
    size_t len = strlen(s);
    if ((size_t)n > len) n = (int)len;

    char *buf = malloc((size_t)n + 1);
    if (!buf) raise("bytes.headbytes(): memoria insuficiente");
    memcpy(buf, s, (size_t)n);
    buf[n] = '\0';

    infernal_return_type("string");
    infernal_return_value("%s", buf);
    free(buf);
}

/* tailbytes(s, n) — últimos n bytes. */
LAVA_EXPORT void lava_tailbytes(const char *s, int n) {
    if (n < 0) n = 0;
    size_t len = strlen(s);
    if ((size_t)n > len) n = (int)len;
    size_t start = len - (size_t)n;

    char *buf = malloc((size_t)n + 1);
    if (!buf) raise("bytes.tailbytes(): memoria insuficiente");
    memcpy(buf, s + start, (size_t)n);
    buf[n] = '\0';

    infernal_return_type("string");
    infernal_return_value("%s", buf);
    free(buf);
}

/* replacebytes(str, from, to) — reemplaza todas las apariciones. */
LAVA_EXPORT void lava_replacebytes(const char *str, const char *from, const char *to) {
    size_t from_len = strlen(from);
    if (from_len == 0) {
        infernal_return_type("string");
        infernal_return_value("%s", str);
        return;
    }

    /* Contamos ocurrencias */
    size_t count = 0;
    const char *tmp = str;
    while ((tmp = strstr(tmp, from)) != NULL) { count++; tmp += from_len; }

    size_t to_len       = strlen(to);
    size_t original_len = strlen(str);
    size_t result_len;

    if (to_len >= from_len) {
        size_t diff = to_len - from_len;
        if (diff > 0 && count > SIZE_MAX / diff)
            raise("bytes.replacebytes(): resultado demasiado grande");
        size_t added = count * diff;
        if (added > SIZE_MAX - original_len - 1)
            raise("bytes.replacebytes(): resultado demasiado grande");
        result_len = original_len + added + 1;
    } else {
        size_t diff = from_len - to_len;
        if (diff > 0 && count > SIZE_MAX / diff)
            raise("bytes.replacebytes(): resultado demasiado grande");
        size_t reduction = count * diff;
        if (reduction > original_len)
            raise("bytes.replacebytes(): inconsistencia interna");
        result_len = original_len - reduction + 1;
    }

    char *result = malloc(result_len);
    if (!result) raise("bytes.replacebytes(): memoria insuficiente");

    const char *read_ptr = str;
    char       *write_ptr = result;
    while (*read_ptr) {
        const char *found = strstr(read_ptr, from);
        if (found == read_ptr) {
            memcpy(write_ptr, to, to_len);
            write_ptr += to_len;
            read_ptr  += from_len;
        } else {
            const char *next = found ? found : read_ptr + strlen(read_ptr);
            size_t chunk = (size_t)(next - read_ptr);
            memcpy(write_ptr, read_ptr, chunk);
            write_ptr += chunk;
            read_ptr   = next;
        }
    }
    *write_ptr = '\0';

    infernal_return_type("string");
    infernal_return_value("%s", result);
    free(result);
}

/* reversebytes(s) — invierte el orden de los bytes. */
LAVA_EXPORT void lava_reversebytes(const char *src) {
    size_t len = strlen(src);
    char *rev = malloc(len + 1);
    if (!rev) raise("bytes.reversebytes(): memoria insuficiente");

    for (size_t i = 0; i < len; i++) rev[i] = src[len - 1 - i];
    rev[len] = '\0';

    infernal_return_type("string");
    infernal_return_value("%s", rev);
    free(rev);
}

/* lengthbytes(s) — igual que countbytes, mantenido por compatibilidad. */
LAVA_EXPORT void lava_lengthbytes(const char *s) {
    infernal_return_type("int");
    infernal_return_value("%d", (int)strlen(s));
}

/* binbytes(s) — representación binaria de cada byte, separados por espacios. */
LAVA_EXPORT void lava_binbytes(const char *s) {
    size_t len = strlen(s);
    size_t out_len = (len == 0) ? 1 : len * 9;
    char *buf = malloc(out_len);
    if (!buf) raise("bytes.binbytes(): memoria insuficiente");

    char *p = buf;
    for (size_t i = 0; i < len; i++) {
        unsigned char byte = (unsigned char)s[i];
        for (int bit = 7; bit >= 0; bit--) {
            *p++ = (byte & (1u << bit)) ? '1' : '0';
        }
        if (i < len - 1) *p++ = ' ';
    }
    *p = '\0';

    infernal_return_type("string");
    infernal_return_value("%s", buf);
    free(buf);
}

/* hexbytes(s) — representación hex mayúsculas separada por espacios. */
LAVA_EXPORT void lava_hexbytes(const char *s) {
    size_t len = strlen(s);
    size_t out_len = (len == 0) ? 1 : len * 3;
    char *buf = malloc(out_len);
    if (!buf) raise("bytes.hexbytes(): memoria insuficiente");

    static const char hexdigits[] = "0123456789ABCDEF";
    char *p = buf;
    for (size_t i = 0; i < len; i++) {
        unsigned char byte = (unsigned char)s[i];
        *p++ = hexdigits[byte >> 4];
        *p++ = hexdigits[byte & 0x0F];
        if (i < len - 1) *p++ = ' ';
    }
    *p = '\0';

    infernal_return_type("string");
    infernal_return_value("%s", buf);
    free(buf);
}

/* utf8bytes(s) — bytes decimales (0-255), separados por espacios. */
LAVA_EXPORT void lava_utf8bytes(const char *s) {
    size_t len = strlen(s);
    size_t out_len = (len == 0) ? 1 : len * 4;
    char *buf = malloc(out_len);
    if (!buf) raise("bytes.utf8bytes(): memoria insuficiente");

    char *p = buf;
    for (size_t i = 0; i < len; i++) {
        int byte = (unsigned char)s[i];
        char num[4];
        int digits = 0;
        if (byte >= 100) {
            num[digits++] = (char)('0' + byte / 100); byte %= 100;
            num[digits++] = (char)('0' + byte / 10);
            num[digits++] = (char)('0' + byte % 10);
        } else if (byte >= 10) {
            num[digits++] = (char)('0' + byte / 10);
            num[digits++] = (char)('0' + byte % 10);
        } else {
            num[digits++] = (char)('0' + byte);
        }
        memcpy(p, num, (size_t)digits);
        p += digits;
        if (i < len - 1) *p++ = ' ';
    }
    *p = '\0';

    infernal_return_type("string");
    infernal_return_value("%s", buf);
    free(buf);
}

/* unicodeCodepoints(s) — puntos de código Unicode en formato U+XXXX. */
LAVA_EXPORT void lava_unicodeCodepoints(const char *str) {
    const unsigned char *s   = (const unsigned char *)str;
    size_t len = strlen(str);
    const unsigned char *p   = s;
    const unsigned char *end = s + len;

    /* Contamos cuántos caracteres UTF-8 hay (para reservar) */
    size_t char_count = 0;
    while (p < end) {
        unsigned char c = *p;
        int extra = 0;
        if      (c < 0x80)             extra = 0;
        else if ((c & 0xE0) == 0xC0)   extra = 1;
        else if ((c & 0xF0) == 0xE0)   extra = 2;
        else if ((c & 0xF8) == 0xF0)   extra = 3;
        p += 1 + extra;
        char_count++;
    }

    size_t out_len = (char_count == 0) ? 1 : char_count * 10;
    char *buf = malloc(out_len);
    if (!buf) raise("bytes.unicodeCodepoints(): memoria insuficiente");

    static const char hexdigits[] = "0123456789ABCDEF";

    char *out = buf;
    p = s;
    bool first = true;
    while (p < end) {
        unsigned char c = *p;
        uint32_t codepoint = 0;
        int extra = 0;

        if (c < 0x80) {
            codepoint = c;
        } else if ((c & 0xE0) == 0xC0) {
            codepoint = c & 0x1F; extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            codepoint = c & 0x0F; extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            codepoint = c & 0x07; extra = 3;
        } else {
            codepoint = c;
        }

        for (int i = 1; i <= extra; i++) {
            codepoint = (codepoint << 6) | (p[i] & 0x3F);
        }
        p += 1 + extra;

        if (!first) *out++ = ' ';
        first = false;

        *out++ = 'U';
        *out++ = '+';
        if (codepoint <= 0xFFFF) {
            *out++ = hexdigits[(codepoint >> 12) & 0xF];
            *out++ = hexdigits[(codepoint >>  8) & 0xF];
            *out++ = hexdigits[(codepoint >>  4) & 0xF];
            *out++ = hexdigits[ codepoint        & 0xF];
        } else {
            *out++ = hexdigits[(codepoint >> 20) & 0xF];
            *out++ = hexdigits[(codepoint >> 16) & 0xF];
            *out++ = hexdigits[(codepoint >> 12) & 0xF];
            *out++ = hexdigits[(codepoint >>  8) & 0xF];
            *out++ = hexdigits[(codepoint >>  4) & 0xF];
            *out++ = hexdigits[ codepoint        & 0xF];
        }
    }
    *out = '\0';

    infernal_return_type("string");
    infernal_return_value("%s", buf);
    free(buf);
}

/* ------------------------------------------------------------------
 * cortes por separador
 * ------------------------------------------------------------------ */

/* cutAfterbytes(s, sep) — todo hasta el final de la primera aparición de sep.
 * Si sep no aparece (o es vacío), devuelve s tal cual. */
LAVA_EXPORT void lava_cutAfterbytes(const char *s, const char *sep) {
    if (*sep == '\0') {
        infernal_return_type("string");
        infernal_return_value("%s", s);
        return;
    }
    const char *p = strstr(s, sep);
    if (!p) {
        infernal_return_type("string");
        infernal_return_value("%s", s);
        return;
    }
    size_t n = (size_t)(p - s) + strlen(sep);
    char *buf = malloc(n + 1);
    if (!buf) raise("bytes.cutAfterbytes(): memoria insuficiente");
    memcpy(buf, s, n);
    buf[n] = '\0';

    infernal_return_type("string");
    infernal_return_value("%s", buf);
    free(buf);
}

/* cutAfterLastbytes(s, sep) — igual pero con la última aparición. */
LAVA_EXPORT void lava_cutAfterLastbytes(const char *s, const char *sep) {
    if (*sep == '\0') {
        infernal_return_type("string");
        infernal_return_value("%s", s);
        return;
    }
    const char *last = NULL, *p = s;
    while ((p = strstr(p, sep)) != NULL) { last = p; p++; }
    if (!last) {
        infernal_return_type("string");
        infernal_return_value("%s", s);
        return;
    }
    size_t n = (size_t)(last - s) + strlen(sep);
    char *buf = malloc(n + 1);
    if (!buf) raise("bytes.cutAfterLastbytes(): memoria insuficiente");
    memcpy(buf, s, n);
    buf[n] = '\0';

    infernal_return_type("string");
    infernal_return_value("%s", buf);
    free(buf);
}

/* cutBeforebytes(s, sep) — desde el inicio de la primera aparición de sep. */
LAVA_EXPORT void lava_cutBeforebytes(const char *s, const char *sep) {
    if (*sep == '\0') {
        infernal_return_type("string");
        infernal_return_value("%s", s);
        return;
    }
    const char *p = strstr(s, sep);
    infernal_return_type("string");
    infernal_return_value("%s", p ? p : "");
}

/* cutBeforeLastbytes(s, sep) — desde el inicio de la última aparición. */
LAVA_EXPORT void lava_cutBeforeLastbytes(const char *s, const char *sep) {
    if (*sep == '\0') {
        infernal_return_type("string");
        infernal_return_value("%s", s);
        return;
    }
    const char *last = NULL, *p = s;
    while ((p = strstr(p, sep)) != NULL) { last = p; p++; }
    infernal_return_type("string");
    infernal_return_value("%s", last ? last : "");
}

/* cutHeadbytes(s, n) — quita los primeros n bytes. */
LAVA_EXPORT void lava_cutHeadbytes(const char *s, int n) {
    if (n < 0) raise("bytes.cutHeadbytes(): no acepta índices negativos");
    size_t len = strlen(s);
    if ((size_t)n >= len) {
        infernal_return_type("string");
        infernal_return_value("%s", "");
        return;
    }
    infernal_return_type("string");
    infernal_return_value("%s", s + n);
}

/* cutTailbytes(s, n) — quita los últimos n bytes. */
LAVA_EXPORT void lava_cutTailbytes(const char *s, int n) {
    if (n < 0) raise("bytes.cutTailbytes(): no acepta índices negativos");
    size_t len = strlen(s);
    if ((size_t)n >= len) {
        infernal_return_type("string");
        infernal_return_value("%s", "");
        return;
    }
    size_t keep = len - (size_t)n;
    char *buf = malloc(keep + 1);
    if (!buf) raise("bytes.cutTailbytes(): memoria insuficiente");
    memcpy(buf, s, keep);
    buf[keep] = '\0';

    infernal_return_type("string");
    infernal_return_value("%s", buf);
    free(buf);
}

/* ==================================================================
 * Registro
 * ================================================================== */

LAVA_MODULE {
    LAVA_REGISTER("countbytes",         lava_countbytes,         "s");
    LAVA_REGISTER("indexofbytes",       lava_indexofbytes,       "ss");
    LAVA_REGISTER("headbytes",          lava_headbytes,          "si");
    LAVA_REGISTER("tailbytes",          lava_tailbytes,          "si");
    LAVA_REGISTER("replacebytes",       lava_replacebytes,       "sss");
    LAVA_REGISTER("reversebytes",       lava_reversebytes,       "s");
    LAVA_REGISTER("lengthbytes",        lava_lengthbytes,        "s");
    LAVA_REGISTER("binbytes",           lava_binbytes,           "s");
    LAVA_REGISTER("hexbytes",           lava_hexbytes,           "s");
    LAVA_REGISTER("utf8bytes",          lava_utf8bytes,          "s");
    LAVA_REGISTER("unicodeCodepoints",  lava_unicodeCodepoints,  "s");
    LAVA_REGISTER("cutAfterbytes",      lava_cutAfterbytes,      "ss");
    LAVA_REGISTER("cutAfterLastbytes",  lava_cutAfterLastbytes,  "ss");
    LAVA_REGISTER("cutBeforebytes",     lava_cutBeforebytes,     "ss");
    LAVA_REGISTER("cutBeforeLastbytes", lava_cutBeforeLastbytes, "ss");
    LAVA_REGISTER("cutHeadbytes",       lava_cutHeadbytes,       "si");
    LAVA_REGISTER("cutTailbytes",       lava_cutTailbytes,       "si");
}
