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
 *   print(bytes.length("hola"))             # 4
 *   print(bytes.bin("AB"))                  # "01000001 01000010"
 *   print(bytes.hex("AB"))                  # "41 42"
 *   print(bytes.utf8("ñ"))                  # "195 177"
 *   print(bytes.unicodeCodepoints("ñ"))     # "U+00F1"
 *   print(bytes.byteAt("hola", 1))          # 104
 *   print(bytes.slice("hola mundo", 1, 4))  # "hola"
 *   print(bytes.base64("hola"))             # "aG9sYQ=="
 *   print(bytes.fromBase64("aG9sYQ=="))     # "hola"
 *   print(bytes.crc32("hola"))              # "E3C4B5B6" (ejemplo)
 *   print(bytes.checksum("hola"))           # 164
 *   print(bytes.compare("abc", "abd"))      # -1
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
 * Helpers para Base64
 * ================================================================== */

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* ==================================================================
 * API pública
 * ================================================================== */

/* length(s) — número de bytes (== strlen). */
LAVA_EXPORT void lava_length(const char *s) {
    infernal_return_type("int");
    infernal_return_value("%d", (int)strlen(s));
}

/* bin(s) — representación binaria de cada byte, separados por espacios. */
LAVA_EXPORT void lava_bin(const char *s) {
    size_t len = strlen(s);
    size_t out_len = (len == 0) ? 1 : len * 9;
    char *buf = malloc(out_len);
    if (!buf) raise("bytes.bin(): memoria insuficiente");

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

/* hex(s) — representación hex mayúsculas separada por espacios. */
LAVA_EXPORT void lava_hex(const char *s) {
    size_t len = strlen(s);
    size_t out_len = (len == 0) ? 1 : len * 3;
    char *buf = malloc(out_len);
    if (!buf) raise("bytes.hex(): memoria insuficiente");

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

/* utf8(s) — bytes decimales (0-255), separados por espacios. */
LAVA_EXPORT void lava_utf8(const char *s) {
    size_t len = strlen(s);
    size_t out_len = (len == 0) ? 1 : len * 4;
    char *buf = malloc(out_len);
    if (!buf) raise("bytes.utf8(): memoria insuficiente");

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

/* byteAt(s, index) — devuelve el byte en la posición index (1-based). */
LAVA_EXPORT void lava_byteAt(const char *s, int index) {
    size_t len = strlen(s);
    if (index < 1 || (size_t)index > len)
        raise("bytes.byteAt(): índice fuera de rango");
    int byte = (int)(unsigned char)s[index - 1];
    infernal_return_type("int");
    infernal_return_value("%d", byte);
}

/* slice(s, start, length) — extrae length bytes desde start (1-based). */
LAVA_EXPORT void lava_slice(const char *s, int start, int length) {
    if (start < 1) raise("bytes.slice(): 'start' debe ser >= 1");
    if (length < 0) raise("bytes.slice(): 'length' debe ser >= 0");
    size_t len = strlen(s);
    if ((size_t)start > len) {
        infernal_return_type("string");
        infernal_return_value("%s", "");
        return;
    }
    size_t start_idx = (size_t)start - 1;
    size_t available = len - start_idx;
    size_t n = (length > available) ? available : (size_t)length;
    char *buf = malloc(n + 1);
    if (!buf) raise("bytes.slice(): memoria insuficiente");
    memcpy(buf, s + start_idx, n);
    buf[n] = '\0';

    infernal_return_type("string");
    infernal_return_value("%s", buf);
    free(buf);
}

/* base64(s) — codifica la cadena en Base64. */
LAVA_EXPORT void lava_base64(const char *s) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(s);
    size_t out_len = 4 * ((len + 2) / 3);
    char *buf = malloc(out_len + 1);
    if (!buf) raise("bytes.base64(): memoria insuficiente");

    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t octet_a = i < len ? (unsigned char)s[i] : 0;
        uint32_t octet_b = i + 1 < len ? (unsigned char)s[i+1] : 0;
        uint32_t octet_c = i + 2 < len ? (unsigned char)s[i+2] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        buf[j++] = b64[(triple >> 18) & 0x3F];
        buf[j++] = b64[(triple >> 12) & 0x3F];
        buf[j++] = b64[(triple >> 6) & 0x3F];
        buf[j++] = b64[triple & 0x3F];
    }
    if (len % 3 == 1) {
        buf[out_len - 2] = '=';
        buf[out_len - 1] = '=';
    } else if (len % 3 == 2) {
        buf[out_len - 1] = '=';
    }
    buf[out_len] = '\0';

    infernal_return_type("string");
    infernal_return_value("%s", buf);
    free(buf);
}

/* fromBase64(s) — decodifica una cadena Base64. */
LAVA_EXPORT void lava_fromBase64(const char *s) {
    size_t slen = strlen(s);
    char *clean = malloc(slen + 1);
    if (!clean) raise("bytes.fromBase64(): memoria insuficiente");
    size_t clen = 0;
    for (size_t i = 0; i < slen; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        clean[clen++] = c;
    }
    clean[clen] = '\0';

    if (clen % 4 != 0) {
        free(clean);
        raise("bytes.fromBase64(): longitud inválida (debe ser múltiplo de 4)");
    }

    size_t out_max = (clen / 4) * 3;
    char *out = malloc(out_max + 1);
    if (!out) { free(clean); raise("bytes.fromBase64(): memoria insuficiente"); }

    size_t out_len = 0;
    bool padding_done = false;
    for (size_t i = 0; i < clen; i += 4) {
        if (padding_done) {
            free(clean); free(out);
            raise("bytes.fromBase64(): datos después del relleno");
        }
        int v0 = b64_val(clean[i]);
        int v1 = b64_val(clean[i+1]);
        if (v0 < 0 || v1 < 0) {
            free(clean); free(out);
            raise("bytes.fromBase64(): carácter inválido");
        }
        if (clean[i+2] == '=') {
            if (clean[i+3] != '=') {
                free(clean); free(out);
                raise("bytes.fromBase64(): relleno inválido");
            }
            uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12);
            unsigned char b = (triple >> 16) & 0xFF;
            if (b == 0) {
                free(clean); free(out);
                raise("bytes.fromBase64(): el resultado contiene byte NUL, no representable como string");
            }
            out[out_len++] = (char)b;
            padding_done = true;
        } else if (clean[i+3] == '=') {
            int v2 = b64_val(clean[i+2]);
            if (v2 < 0) {
                free(clean); free(out);
                raise("bytes.fromBase64(): carácter inválido");
            }
            uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6);
            unsigned char b1 = (triple >> 16) & 0xFF;
            unsigned char b2 = (triple >> 8) & 0xFF;
            if (b1 == 0 || b2 == 0) {
                free(clean); free(out);
                raise("bytes.fromBase64(): el resultado contiene byte NUL, no representable como string");
            }
            out[out_len++] = (char)b1;
            out[out_len++] = (char)b2;
            padding_done = true;
        } else {
            int v2 = b64_val(clean[i+2]);
            int v3 = b64_val(clean[i+3]);
            if (v2 < 0 || v3 < 0) {
                free(clean); free(out);
                raise("bytes.fromBase64(): carácter inválido");
            }
            uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6) | (uint32_t)v3;
            unsigned char b1 = (triple >> 16) & 0xFF;
            unsigned char b2 = (triple >> 8) & 0xFF;
            unsigned char b3 = triple & 0xFF;
            if (b1 == 0 || b2 == 0 || b3 == 0) {
                free(clean); free(out);
                raise("bytes.fromBase64(): el resultado contiene byte NUL, no representable como string");
            }
            out[out_len++] = (char)b1;
            out[out_len++] = (char)b2;
            out[out_len++] = (char)b3;
        }
    }
    out[out_len] = '\0';

    infernal_return_type("string");
    infernal_return_value("%s", out);
    free(clean);
    free(out);
}

/* crc32(s) — CRC-32 (polinomio 0xEDB88320) en hex mayúsculas de 8 dígitos. */
LAVA_EXPORT void lava_crc32(const char *s) {
    uint32_t crc = 0xFFFFFFFF;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        crc ^= *p;
        for (int i = 0; i < 8; i++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
            else crc >>= 1;
        }
    }
    crc = ~crc;
    char buf[9];
    snprintf(buf, sizeof(buf), "%08X", crc);
    infernal_return_type("string");
    infernal_return_value("%s", buf);
}

/* checksum(s) — suma de todos los bytes módulo 256. */
LAVA_EXPORT void lava_checksum(const char *s) {
    unsigned int sum = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        sum += *p;
    }
    sum %= 256;
    infernal_return_type("int");
    infernal_return_value("%d", (int)sum);
}

/* compare(s1, s2) — comparación lexicográfica byte a byte: -1, 0, 1. */
LAVA_EXPORT void lava_compare(const char *s1, const char *s2) {
    size_t len1 = strlen(s1);
    size_t len2 = strlen(s2);
    size_t min_len = len1 < len2 ? len1 : len2;
    int cmp = memcmp(s1, s2, min_len);
    if (cmp == 0) {
        if (len1 < len2) cmp = -1;
        else if (len1 > len2) cmp = 1;
    } else {
        cmp = cmp < 0 ? -1 : 1;
    }
    infernal_return_type("int");
    infernal_return_value("%d", cmp);
}

/* ==================================================================
 * Registro
 * ================================================================== */

LAVA_MODULE {
    LAVA_REGISTER("length",              lava_length,              "s");
    LAVA_REGISTER("bin",                 lava_bin,                 "s");
    LAVA_REGISTER("hex",                 lava_hex,                 "s");
    LAVA_REGISTER("utf8",                lava_utf8,                "s");
    LAVA_REGISTER("unicodeCodepoints",   lava_unicodeCodepoints,   "s");
    LAVA_REGISTER("byteAt",              lava_byteAt,              "si");
    LAVA_REGISTER("slice",               lava_slice,               "sii");
    LAVA_REGISTER("base64",              lava_base64,              "s");
    LAVA_REGISTER("fromBase64",          lava_fromBase64,          "s");
    LAVA_REGISTER("crc32",               lava_crc32,               "s");
    LAVA_REGISTER("checksum",            lava_checksum,            "s");
    LAVA_REGISTER("compare",             lava_compare,             "ss");
}
