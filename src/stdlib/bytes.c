/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, Lynds Corp., David Baña Szymaniak
 * Licencia GPL v3 o posterior
 * Código fuente de Infernal: stdlib/string.c
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


/* ================================================
 *  Registro de funciones
 * ================================================ */

void register_bytes_builtins(void) {
    func_register_builtin("countbytes", builtin_countbytes);
    func_register_builtin("headbytes", builtin_headbytes);
    func_register_builtin("tailbytes", builtin_tailbytes);
    func_register_builtin("replacebytes", builtin_replacebytes);
    func_register_builtin("reversebytes", builtin_reversebytes);
    func_register_builtin("lengthbytes", builtin_lengthbytes);

    vm_register_builtin("countbytes", builtin_countbytes);
    vm_register_builtin("headbytes", builtin_headbytes);
    vm_register_builtin("tailbytes", builtin_tailbytes);
    vm_register_builtin("replacebytes", builtin_replacebytes);
    vm_register_builtin("reversebytes", builtin_reversebytes);
    vm_register_builtin("lengthbytes", builtin_lengthbytes);
}
