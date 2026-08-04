/*
 * Infernal: el lenguaje de programación.
 * Copyright (C) 2026, Lynds Corp., David Baña Szymaniak, GPL v3+ License.
 * Proyecto: Aros Legendarios
 * Código fuente de Infernal: vm/bytecode.h
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "output.h"
#include "core/value.h"
#include "runtime/globals.h"
#include "vm/vm.h"

/* ─── Mapa de nombres de color a códigos ANSI ──────────────── */
static const struct {
    const char *name;
    const char *code;
} color_map[] = {
    {"red",     "\033[31m"},
    {"blue",    "\033[34m"},
    {"green",   "\033[32m"},
    {"yellow",  "\033[33m"},
    {"orange",  "\033[38;5;208m"},
    {"magenta", "\033[35m"},
    {"black",   "\033[30m"},
    {"white",   "\033[37m"},
    {"reset",   "\033[0m"},
    {NULL, NULL}
};

static void print_value(Value v) {
    switch (v.type) {
        case VAL_INT:    printf("%d", v.data.ival); break;
        case VAL_FLOAT:  printf("%g", v.data.fval); break;
        case VAL_BOOL:   printf("%s", v.data.bval ? "true" : "false"); break;
        case VAL_STRING: printf("%s", v.data.sval ? v.data.sval : "(null)"); break;
        case VAL_LIST:
            printf("[");
            for (int j = 0; j < v.data.list.count; j++) {
                if (j > 0) printf(", ");
                print_value(v.data.list.items[j]);
            }
            printf("]");
            break;
        default: printf("?");
    }
}

/* ─── color() : devuelve el código ANSI para el color ──────── */
static Value builtin_color(int argc, Value *args) {
    if (argc < 1) {
        // Sin argumentos: devolver reset
        return val_string("\033[0m");
    }

    Value arg = args[0];
    if (arg.type != VAL_STRING) {
        // Si no es string, devolver reset
        return val_string("\033[0m");
    }

    const char *input = arg.data.sval;

    // 1) Buscar en el mapa de colores predefinidos (insensible a mayúsculas)
    for (int i = 0; color_map[i].name != NULL; i++) {
        if (strcasecmp(input, color_map[i].name) == 0) {
            return val_string(color_map[i].code);
        }
    }

    // 2) Si comienza con "\033", usarlo directamente (ANSI code)
    if (strncmp(input, "\033", 1) == 0) {
        return val_string(input);
    }

    // 3) Si no se reconoce, devolver reset
    return val_string("\033[0m");
}

/* ─── print(): imprime argumentos y siempre resetea al final ── */
static Value builtin_print(int argc, Value *args) {
    for (int i = 0; i < argc; i++) {
        if (i > 0) printf(" ");
        print_value(args[i]);
    }
    // Siempre resetear al final para no dejar color en la terminal
    printf("\033[0m\n");
    fflush(stdout);
    return val_make_null();
}

/* ─── Funciones warn, error, success usando print con color ── */
static Value builtin_warn(int argc, Value *args) {
    Value *new_args = malloc((argc + 1) * sizeof(Value));
    if (!new_args) return val_make_null();
    new_args[0] = val_string("\033[33m");
    for (int i = 0; i < argc; i++) {
        new_args[i+1] = args[i];
    }
    Value ret = builtin_print(argc + 1, new_args);
    free(new_args);
    return ret;
}

static Value builtin_error(int argc, Value *args) {
    Value *new_args = malloc((argc + 1) * sizeof(Value));
    if (!new_args) return val_make_null();
    new_args[0] = val_string("\033[31m");
    for (int i = 0; i < argc; i++) {
        new_args[i+1] = args[i];
    }
    Value ret = builtin_print(argc + 1, new_args);
    free(new_args);
    return ret;
}

static Value builtin_success(int argc, Value *args) {
    Value *new_args = malloc((argc + 1) * sizeof(Value));
    if (!new_args) return val_make_null();
    new_args[0] = val_string("\033[32m");
    for (int i = 0; i < argc; i++) {
        new_args[i+1] = args[i];
    }
    Value ret = builtin_print(argc + 1, new_args);
    free(new_args);
    return ret;
}

/* ─── Registro de funciones ──────────────────────────────────── */
void register_output_builtins(void) {
    func_register_builtin("print", builtin_print);
    func_register_builtin("warn", builtin_warn);
    func_register_builtin("error", builtin_error);
    func_register_builtin("success", builtin_success);
    func_register_builtin("color", builtin_color);

    vm_register_builtin("print", builtin_print);
    vm_register_builtin("warn", builtin_warn);
    vm_register_builtin("error", builtin_error);
    vm_register_builtin("success", builtin_success);
    vm_register_builtin("color", builtin_color);
}
