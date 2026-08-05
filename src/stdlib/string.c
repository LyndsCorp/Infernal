/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License.
 * Código fuente de Infernal: stdlib/string.c
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "string.h"
#include "core/value.h"
#include "runtime/error.h"
#include "runtime/globals.h"
#include "vm/vm.h"

/* --------------------------------------------------------------------------
 *  lower(str) : convierte la cadena a minúsculas
 *  -------------------------------------------------------------------------- */
static Value builtin_lower(int argc, Value *args) {
    if (argc != 1) error(0, "lower() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(0, "lower() espera un string.");
    char *s = strdup(args[0].data.sval);
    for (char *p = s; *p; p++) *p = tolower((unsigned char)*p);
    Value res = val_string(s);
    free(s);
    return res;
}

/* --------------------------------------------------------------------------
 *  upper(str) : convierte la cadena a mayúsculas
 *  -------------------------------------------------------------------------- */
static Value builtin_upper(int argc, Value *args) {
    if (argc != 1) error(0, "upper() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(0, "upper() espera un string.");
    char *s = strdup(args[0].data.sval);
    for (char *p = s; *p; p++) *p = toupper((unsigned char)*p);
    Value res = val_string(s);
    free(s);
    return res;
}

/* --------------------------------------------------------------------------
 *  head(str, n)  : devuelve los primeros n caracteres de str
 *  head(str, sub): devuelve true si str comienza con sub
 *  -------------------------------------------------------------------------- */
static Value builtin_head(int argc, Value *args) {
    if (argc < 2) error(0, "head requiere dos argumentos");
    if (args[0].type != VAL_STRING) error(0, "head espera un string como primer argumento");

    const char *s = args[0].data.sval;
    int len = (int)strlen(s);

    if (args[1].type == VAL_INT) {
        int n = args[1].data.ival;
        if (n < 0) n = 0;
        if (n > len) n = len;
        char *buf = (char*)malloc(n + 1);
        if (!buf) error(0, "memoria insuficiente en head");
        strncpy(buf, s, n);
        buf[n] = '\0';
        Value res = val_string(buf);
        free(buf);
        return res;
    }
    else if (args[1].type == VAL_STRING) {
        const char *prefix = args[1].data.sval;
        size_t plen = strlen(prefix);
        bool starts = (len >= (int)plen && strncmp(s, prefix, plen) == 0);
        return val_bool(starts);
    }
    else {
        error(0, "head: el segundo argumento debe ser int o string");
    }
    return val_make_null(); // no se alcanza
}

/* --------------------------------------------------------------------------
 *  tail(str, n)  : devuelve los últimos n caracteres de str
 *  tail(str, sub): devuelve true si str termina con sub
 *  -------------------------------------------------------------------------- */
static Value builtin_tail(int argc, Value *args) {
    if (argc < 2) error(0, "tail requiere dos argumentos");
    if (args[0].type != VAL_STRING) error(0, "tail espera un string como primer argumento");

    const char *s = args[0].data.sval;
    int len = (int)strlen(s);

    if (args[1].type == VAL_INT) {
        int n = args[1].data.ival;
        if (n < 0) n = 0;
        if (n > len) n = len;
        int start = len - n;
        char *buf = (char*)malloc(n + 1);
        if (!buf) error(0, "memoria insuficiente en tail");
        strncpy(buf, s + start, n);
        buf[n] = '\0';
        Value res = val_string(buf);
        free(buf);
        return res;
    }
    else if (args[1].type == VAL_STRING) {
        const char *suffix = args[1].data.sval;
        size_t slen = strlen(suffix);
        bool ends = false;
        if (len >= (int)slen) {
            ends = (strcmp(s + len - slen, suffix) == 0);
        }
        return val_bool(ends);
    }
    else {
        error(0, "tail: el segundo argumento debe ser int o string");
    }
    return val_make_null();
}

/* --------------------------------------------------------------------------
 *  starts(string, str) : devuelve true si `string` comienza por `str`
 *  -------------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------------
 *  ends(string, str) : devuelve true si `string` termina por `str`
 *  -------------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------------
 *  has(string, str) : devuelve true si `str` aparece en cualquier parte de `string`
 *  -------------------------------------------------------------------------- */
static Value builtin_has(int argc, Value *args) {
    if (argc != 2) error(0, "has() espera exactamente 2 argumentos");
    if (args[0].type != VAL_STRING) error(0, "has() espera un string como primer argumento");
    if (args[1].type != VAL_STRING) error(0, "has() espera un string como segundo argumento");

    const char *s = args[0].data.sval;
    const char *sub = args[1].data.sval;

    return val_bool(strstr(s, sub) != NULL);
}

/* --------------------------------------------------------------------------
 *  Registro de las funciones integradas
 *  -------------------------------------------------------------------------- */
void register_string_builtins(void) {
    func_register_builtin("head", builtin_head);
    func_register_builtin("tail", builtin_tail);
    func_register_builtin("lower", builtin_lower);
    func_register_builtin("upper", builtin_upper);
    func_register_builtin("starts", builtin_starts);
    func_register_builtin("ends", builtin_ends);
    func_register_builtin("has", builtin_has);

    vm_register_builtin("head", builtin_head);
    vm_register_builtin("tail", builtin_tail);
    vm_register_builtin("lower", builtin_lower);
    vm_register_builtin("upper", builtin_upper);
    vm_register_builtin("starts", builtin_starts);
    vm_register_builtin("ends", builtin_ends);
    vm_register_builtin("has", builtin_has);
}
