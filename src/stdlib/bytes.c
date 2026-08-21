/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: stdlib/bytes.c
 *
 * Operaciones sobre bytes crudos (sin interpretación UTF‑8).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "bytes.h"
#include "core/value.h"
#include "runtime/error.h"
#include "runtime/globals.h"
#include "vm/vm.h"


/* ================================================
 *  Funciones de la biblioteca
 * ================================================ */

/* --- countbytes() --- */
static Value builtin_countbytes(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "countbytes() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "countbytes() espera un string.");
    size_t len = strlen(args[0].data.sval);
    return val_int((int)len);
}

/* --- indexofbytes() --- */
static Value builtin_indexofbytes(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "indexofbytes() espera exactamente 2 argumentos");
    if (args[0].type != VAL_STRING) error(current_eval_line, "indexofbytes() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(current_eval_line, "indexofbytes() espera un string como segundo argumento");

    const char *haystack = args[0].data.sval;
    const char *needle   = args[1].data.sval;

    if (*needle == '\0') return val_int(1);  // subcadena vacía: posición 1 (base 1)

    const char *found = strstr(haystack, needle);
    if (!found) return val_int(0);  // no encontrado

    return val_int((int)(found - haystack) + 1);
}

/* --- headbytes() --- */
static Value builtin_headbytes(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "headbytes requiere dos argumentos");
    if (args[0].type != VAL_STRING) error(current_eval_line, "headbytes espera un string como primer argumento");
    if (args[1].type != VAL_INT) error(current_eval_line, "headbytes espera un entero como segundo argumento");

    const char *s = args[0].data.sval;
    int n = args[1].data.ival;
    if (n < 0) n = 0;
    size_t len = strlen(s);
    if ((size_t)n > len) n = (int)len;
    char *buf = (char*)malloc(n + 1);
    if (!buf) error(current_eval_line, "memoria insuficiente en headbytes");
    memcpy(buf, s, n);
    buf[n] = '\0';
    Value res = val_string(buf);
    free(buf);
    return res;
}

/* --- tailbytes() --- */
static Value builtin_tailbytes(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "tailbytes requiere dos argumentos");
    if (args[0].type != VAL_STRING) error(current_eval_line, "tailbytes espera un string como primer argumento");
    if (args[1].type != VAL_INT) error(current_eval_line, "tailbytes espera un entero como segundo argumento");

    const char *s = args[0].data.sval;
    int n = args[1].data.ival;
    if (n < 0) n = 0;
    size_t len = strlen(s);
    if ((size_t)n > len) n = (int)len;
    size_t start = len - n;
    char *buf = (char*)malloc(n + 1);
    if (!buf) error(current_eval_line, "memoria insuficiente en tailbytes");
    memcpy(buf, s + start, n);
    buf[n] = '\0';
    Value res = val_string(buf);
    free(buf);
    return res;
}

/* --- replacebytes() --- */
static Value builtin_replacebytes(int argc, Value *args) {
    if (argc != 3) error(current_eval_line, "replacebytes() espera exactamente 3 argumentos");
    if (args[0].type != VAL_STRING) error(current_eval_line, "replacebytes() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(current_eval_line, "replacebytes() espera un string como segundo argumento");
    if (args[2].type != VAL_STRING) error(current_eval_line, "replacebytes() espera un string como tercer argumento");

    const char *str = args[0].data.sval;
    const char *from = args[1].data.sval;
    const char *to = args[2].data.sval;
    size_t from_len = strlen(from);
    if (from_len == 0) return val_string(str);

    size_t count = 0;
    const char *tmp = str;
    while ((tmp = strstr(tmp, from)) != NULL) { count++; tmp += from_len; }

    size_t to_len = strlen(to);
    size_t original_len = strlen(str);
    size_t result_len;

    if (to_len >= from_len) {
        size_t diff = to_len - from_len;
        if (diff > 0 && count > SIZE_MAX / diff)
            error(current_eval_line, "replacebytes() resultado demasiado grande");
        size_t added = count * diff;
        if (added > SIZE_MAX - original_len - 1)
            error(current_eval_line, "replacebytes() resultado demasiado grande");
        result_len = original_len + added + 1;
    } else {
        size_t diff = from_len - to_len;
        if (diff > 0 && count > SIZE_MAX / diff)
            error(current_eval_line, "replacebytes() resultado demasiado grande");
        size_t reduction = count * diff;
        if (reduction > original_len)
            error(current_eval_line, "replacebytes() inconsistencia interna");
        result_len = original_len - reduction + 1;
    }

    char *result = (char*)malloc(result_len);
    if (!result) error(current_eval_line, "memoria insuficiente en replacebytes");

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

/* --- reversebytes() --- */
static Value builtin_reversebytes(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "reversebytes() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "reversebytes() espera un string.");

    const char *src = args[0].data.sval;
    size_t len = strlen(src);
    char *rev = (char*)malloc(len + 1);
    if (!rev) error(current_eval_line, "memoria insuficiente en reversebytes");
    for (size_t i = 0; i < len; i++) rev[i] = src[len - 1 - i];
    rev[len] = '\0';
    Value res = val_string(rev);
    free(rev);
    return res;
}

/* --- lengthbytes() --- */
static Value builtin_lengthbytes(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "lengthbytes() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "lengthbytes() espera un string.");
    size_t len = strlen(args[0].data.sval);
    return val_int((int)len);
}

/* --- binbytes() --- */
static Value builtin_binbytes(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "binbytes() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "binbytes() espera un string.");

    const unsigned char *s = (const unsigned char *)args[0].data.sval;
    size_t len = strlen((const char *)s);

    size_t out_len = (len == 0) ? 1 : len * 9;
    char *buf = (char *)malloc(out_len);
    if (!buf) error(current_eval_line, "memoria insuficiente en binbytes");

    char *p = buf;
    for (size_t i = 0; i < len; i++) {
        unsigned char byte = s[i];
        for (int bit = 7; bit >= 0; bit--) {
            *p++ = (byte & (1u << bit)) ? '1' : '0';
        }
        if (i < len - 1) *p++ = ' ';
    }
    *p = '\0';

    Value res = val_string(buf);
    free(buf);
    return res;
}

/* --- hexbytes() --- */
static Value builtin_hexbytes(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "hexbytes() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "hexbytes() espera un string.");

    const unsigned char *s = (const unsigned char *)args[0].data.sval;
    size_t len = strlen((const char *)s);

    size_t out_len = (len == 0) ? 1 : len * 3;
    char *buf = (char *)malloc(out_len);
    if (!buf) error(current_eval_line, "memoria insuficiente en hexbytes");

    char *p = buf;
    for (size_t i = 0; i < len; i++) {
        unsigned char byte = s[i];
        *p++ = "0123456789ABCDEF"[byte >> 4];
        *p++ = "0123456789ABCDEF"[byte & 0x0F];
        if (i < len - 1) *p++ = ' ';
    }
    *p = '\0';

    Value res = val_string(buf);
    free(buf);
    return res;
}

/* --- utf8bytes() ---
 * Muestra los bytes UTF‑8 reales en formato decimal (0‑255), separados por espacios.
 */
static Value builtin_utf8bytes(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "utf8bytes() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "utf8bytes() espera un string.");

    const unsigned char *s = (const unsigned char *)args[0].data.sval;
    size_t len = strlen((const char *)s);

    // Cada byte decimal ocupa hasta 3 dígitos + 1 espacio (sin espacio final)
    size_t out_len = (len == 0) ? 1 : len * 4;
    char *buf = (char *)malloc(out_len);
    if (!buf) error(current_eval_line, "memoria insuficiente en utf8bytes");

    char *p = buf;
    for (size_t i = 0; i < len; i++) {
        int byte = s[i];
        // Escribimos el número decimal manualmente para evitar sprintf
        char num[4];
        int digits = 0;
        if (byte >= 100) {
            num[digits++] = '0' + byte / 100;
            byte %= 100;
            num[digits++] = '0' + byte / 10;
            num[digits++] = '0' + byte % 10;
        } else if (byte >= 10) {
            num[digits++] = '0' + byte / 10;
            num[digits++] = '0' + byte % 10;
        } else {
            num[digits++] = '0' + byte;
        }
        memcpy(p, num, digits);
        p += digits;
        if (i < len - 1) *p++ = ' ';
    }
    *p = '\0';

    Value res = val_string(buf);
    free(buf);
    return res;
}

/* --- unicodeCodepoints() ---
 * Devuelve los puntos de código Unicode de cada carácter UTF‑8.
 * Formato: U+XXXX (o U+XXXXXX para fuera del plano básico), separados por espacios.
 */
static Value builtin_unicodeCodepoints(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "unicodeCodepoints() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "unicodeCodepoints() espera un string.");

    const unsigned char *s = (const unsigned char *)args[0].data.sval;
    size_t len = strlen((const char *)s);
    const unsigned char *p = s;
    const unsigned char *end = s + len;

    // Contamos cuántos caracteres UTF‑8 hay
    size_t char_count = 0;
    while (p < end) {
        unsigned char c = *p;
        int extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else extra = 0;
        p += 1 + extra;
        char_count++;
    }

    // Máximo: U+10FFFF (8 caracteres) + espacio
    size_t out_len = (char_count == 0) ? 1 : char_count * 10;
    char *buf = (char *)malloc(out_len);
    if (!buf) error(current_eval_line, "memoria insuficiente en unicodeCodepoints");

    char *out = buf;
    p = s;
    bool first = true;
    while (p < end) {
        unsigned char c = *p;
        uint32_t codepoint;
        int extra = 0;

        if (c < 0x80) {
            codepoint = c;
        } else if ((c & 0xE0) == 0xC0) {
            codepoint = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            codepoint = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            codepoint = c & 0x07;
            extra = 3;
        } else {
            codepoint = c;
            extra = 0;
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
            *out++ = "0123456789ABCDEF"[(codepoint >> 12) & 0xF];
            *out++ = "0123456789ABCDEF"[(codepoint >> 8) & 0xF];
            *out++ = "0123456789ABCDEF"[(codepoint >> 4) & 0xF];
            *out++ = "0123456789ABCDEF"[codepoint & 0xF];
        } else {
            *out++ = "0123456789ABCDEF"[(codepoint >> 20) & 0xF];
            *out++ = "0123456789ABCDEF"[(codepoint >> 16) & 0xF];
            *out++ = "0123456789ABCDEF"[(codepoint >> 12) & 0xF];
            *out++ = "0123456789ABCDEF"[(codepoint >> 8) & 0xF];
            *out++ = "0123456789ABCDEF"[(codepoint >> 4) & 0xF];
            *out++ = "0123456789ABCDEF"[codepoint & 0xF];
        }
    }
    *out = '\0';

    Value res = val_string(buf);
    free(buf);
    return res;
}


/* ================================================
 *  Registro de funciones
 * ================================================ */

void register_bytes_builtins(void) {
    func_register_builtin("countbytes", builtin_countbytes);
    func_register_builtin("indexofbytes", builtin_indexofbytes);
    func_register_builtin("headbytes", builtin_headbytes);
    func_register_builtin("tailbytes", builtin_tailbytes);
    func_register_builtin("replacebytes", builtin_replacebytes);
    func_register_builtin("reversebytes", builtin_reversebytes);
    func_register_builtin("lengthbytes", builtin_lengthbytes);
    func_register_builtin("binbytes", builtin_binbytes);
    func_register_builtin("hexbytes", builtin_hexbytes);
    func_register_builtin("utf8bytes", builtin_utf8bytes);
    func_register_builtin("unicodeCodepoints", builtin_unicodeCodepoints);

    vm_register_builtin("countbytes", builtin_countbytes);
    vm_register_builtin("indexofbytes", builtin_indexofbytes);
    vm_register_builtin("headbytes", builtin_headbytes);
    vm_register_builtin("tailbytes", builtin_tailbytes);
    vm_register_builtin("replacebytes", builtin_replacebytes);
    vm_register_builtin("reversebytes", builtin_reversebytes);
    vm_register_builtin("lengthbytes", builtin_lengthbytes);
    vm_register_builtin("binbytes", builtin_binbytes);
    vm_register_builtin("hexbytes", builtin_hexbytes);
    vm_register_builtin("utf8bytes", builtin_utf8bytes);
    vm_register_builtin("unicodeCodepoints", builtin_unicodeCodepoints);
}
