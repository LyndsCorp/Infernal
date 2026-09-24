/*
 * Infernal: el lenguaje de programación.
 * Copyright (C) 2026, David Baña Szymaniak, GPL v3+ License.
 * Proyecto: Aros Legendarios
 * Código fuente de Infernal: stdlib/output.c
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "output.h"
#include "core/value.h"
#include "runtime/globals.h"
#include "vm/vm.h"

/* --- Mapa de nombres de color a códigos ANSI ---------------- */
static const struct {
    const char *name;
    const char *code;
} color_map[] = {
    {"red",       "\033[31m"},
    {"orange",    "\033[38;5;208m"},
    {"yellow",    "\033[93m"},
    {"green",     "\033[32m"},
    {"blue",      "\033[34m"},
    {"purple",    "\033[35m"},
    {"cyan",      "\033[36m"},
    {"magenta",   "\033[35m"},
    {"black",     "\033[30m"},
    {"white",     "\033[37m"},
    {"gray",      "\033[90m"},
    {"reset",     "\033[0m"},
    {NULL, NULL}
};

/* --- Helpers internos que escriben en un FILE* arbitrario ---- */

static void fprint_value_literal(FILE *out, Value v);

/* Imprime un valor en su forma normal de salida. */
static void fprint_value(FILE *out, Value v) {
    switch (v.type) {
        case VAL_INT:    fprintf(out, "%d", v.data.ival); break;
        case VAL_FLOAT:  fprintf(out, "%g", v.data.fval); break;
        case VAL_BOOL:   fprintf(out, "%s", v.data.bval ? "true" : "false"); break;
        case VAL_STRING: fprintf(out, "%s", v.data.sval ? v.data.sval : ""); break;
        case VAL_LIST:
            fprintf(out, "[");
            for (int j = 0; j < v.data.list.count; j++) {
                if (j > 0) fprintf(out, ", ");
                fprint_value_literal(out, v.data.list.items[j]);
            }
            fprintf(out, "]");
            break;
        case VAL_MAP: {
            fprintf(out, "[");
            MapData *md = v.data.map;
            if (md) {
                for (int i = 0; i < md->count; i++) {
                    if (i > 0) fprintf(out, ", ");
                    fprintf(out, "%s = ", md->pairs[i].key ? md->pairs[i].key : "");
                    fprint_value_literal(out, md->pairs[i].value);
                }
            }
            fprintf(out, "]");
            break;
        }
        default:
            fprintf(out, "?");
            break;
    }
}

/* Representa strings dentro de contenedores con comillas, sin cambiar
 * el comportamiento de print("texto") fuera de una lista/mapa. */
static void fprint_value_literal(FILE *out, Value v) {
    switch (v.type) {
        case VAL_STRING:
            fputc('"', out);
            if (v.data.sval) {
                for (const unsigned char *p = (const unsigned char *)v.data.sval; *p; ++p) {
                    switch (*p) {
                        case '\\': fprintf(out, "\\\\"); break;
                        case '"':  fprintf(out, "\\\""); break;
                        case '\n': fprintf(out, "\\n"); break;
                        case '\r': fprintf(out, "\\r"); break;
                        case '\t': fprintf(out, "\\t"); break;
                        default:   fputc(*p, out); break;
                    }
                }
            }
            fputc('"', out);
            break;
                        case VAL_LIST:
                            fprintf(out, "[");
                            for (int i = 0; i < v.data.list.count; i++) {
                                if (i > 0) fprintf(out, ", ");
                                fprint_value_literal(out, v.data.list.items[i]);
                            }
                            fprintf(out, "]");
                            break;
                        case VAL_MAP: {
                            fprintf(out, "[");
                            MapData *md = v.data.map;
                            if (md) {
                                for (int i = 0; i < md->count; i++) {
                                    if (i > 0) fprintf(out, ", ");
                                    fprintf(out, "%s = ", md->pairs[i].key ? md->pairs[i].key : "");
                                    fprint_value_literal(out, md->pairs[i].value);
                                }
                            }
                            fprintf(out, "]");
                            break;
                        }
                        default:
                            fprint_value(out, v);
                            break;
    }
}

/* --- API pública usada por io.c y otros módulos ------------- */
void print_value(Value v) {
    fprint_value(stdout, v);
}

/* --- Strip del sufijo 0x1A (\\N) para suprimir el salto de línea --- */
static int strip_newline_marker(Value *args, int argc) {
    if (argc > 0 && args[argc-1].type == VAL_STRING) {
        char *s = args[argc-1].data.sval;
        size_t len = strlen(s);
        if (len > 0 && s[len-1] == 0x1A) {
            char *new_s = malloc(len);
            if (new_s) {
                memcpy(new_s, s, len - 1);
                new_s[len - 1] = '\0';
                free(args[argc-1].data.sval);
                args[argc-1].data.sval = new_s;
                return 1;
            }
        }
    }
    return 0;
}

/* --- color() --- */
static Value builtin_color(int argc, Value *args) {
    if (argc < 1) return val_string("\033[0m");
    Value arg = args[0];
    if (arg.type != VAL_STRING) return val_string("\033[0m");
    const char *input = arg.data.sval;
    for (int i = 0; color_map[i].name != NULL; i++) {
        if (strcasecmp(input, color_map[i].name) == 0)
            return val_string(color_map[i].code);
    }
    if (strncmp(input, "\033", 1) == 0) return val_string(input);
    return val_string("\033[0m");
}

/* --- print() --- */
static Value builtin_print(int argc, Value *args) {
    int suppress_newline = strip_newline_marker(args, argc);

    for (int i = 0; i < argc; i++) {
        if (i > 0) printf(" ");
        print_value(args[i]);
    }
    printf("\033[0m");
    if (!suppress_newline) printf("\n");
    fflush(stdout);
    return val_make_null();
}

/* --- printf() ---
 * Como print normal pero sin salto de línea ni color reset. */
static Value builtin_printf(int argc, Value *args) {
    for (int i = 0; i < argc; i++) {
        if (i > 0) printf(" ");
        print_value(args[i]);
    }
    fflush(stdout);
    return val_make_null();
}

/* --- error() ---
 * Se comporta como print normal (salto de línea y color reset), pero
 * toda la salida va a stderr. */
static Value builtin_error(int argc, Value *args) {
    int suppress_newline = strip_newline_marker(args, argc);

    for (int i = 0; i < argc; i++) {
        if (i > 0) fprintf(stderr, " ");
        fprint_value(stderr, args[i]);
    }
    fprintf(stderr, "\033[0m");
    if (!suppress_newline) fprintf(stderr, "\n");
    fflush(stderr);
    return val_make_null();
}

/* --- Registro --- */
void register_output_builtins(void) {
    func_register_builtin("print",  builtin_print);
    func_register_builtin("printf", builtin_printf);
    func_register_builtin("error",  builtin_error);
    func_register_builtin("color",  builtin_color);

    vm_register_builtin("print",  builtin_print);
    vm_register_builtin("printf", builtin_printf);
    vm_register_builtin("error",  builtin_error);
    vm_register_builtin("color",  builtin_color);
}
