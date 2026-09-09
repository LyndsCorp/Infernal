/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: stdlib/io.c
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include "io.h"
#include "core/value.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "runtime/evaluator/helpers.h"
#include "vm/vm.h"
#include "stdlib/output.h"

static const char *value_type_name(int type) {
    switch (type) {
        case VAL_INT: return "int";
        case VAL_FLOAT: return "float";
        case VAL_BOOL: return "bool";
        case VAL_STRING: return "string";
        case VAL_LIST: return "list";
        case VAL_MAP: return "map";
        case VAL_NULL: return "null";
        case VAL_REFERENCE: return "reference";
        default: return "unknown";
    }
}

static void print_var_entry(const VarEntry *entry, const char *indent, int line) {
    printf("%s%s", indent, entry->name);
    if (entry->value.type == VAL_REFERENCE) {
        const char *container = entry->value.data.ref.container_name ? entry->value.data.ref.container_name : "?";
        if (entry->value.data.ref.is_map) {
            printf(" -> %s[\"%s\"]: ", container,
                   entry->value.data.ref.map_key ? entry->value.data.ref.map_key : "?");
        } else {
            printf(" -> %s[%d]: ", container, entry->value.data.ref.index);
        }
        Value ref = copy_value_secure(entry->value);
        Value resolved = resolve_reference(ref, line);
        print_value(resolved);
        printf(" (%s)", value_type_name(resolved.type));
        value_free(&resolved);
        return;
    }
    printf(": ");
    print_value(entry->value);
    printf(" (%s)", value_type_name(entry->value.type));
}

/* --- printAllVars() --- */
static Value builtin_printAllVars(int argc, Value *args) {
    (void)argc; (void)args;
    printf("Variables accesibles:\n");

    if (super_global_scope && super_global_scope->vars) {
        printf("  Ámbito superglobal (compartido entre scripts):\n");
        for (VarEntry *e = super_global_scope->vars; e; e = e->next) {
            print_var_entry(e, "    ", current_eval_line);
            printf("\n");
        }
    }

    Scope *s = current_scope;
    int total_vars = 0;
    while (s) {
        if (s == super_global_scope) {
            s = s->parent;
            continue;
        }
        if (s->vars) {
            if (s == global_scope) printf("  Ámbito del script:\n");
            else if (s->function_name) printf("  Ámbito local de función '%s':\n", s->function_name);
            else printf("  Scope %p:\n", (void*)s);

            for (VarEntry *e = s->vars; e; e = e->next) {
                total_vars++;
                print_var_entry(e, "    ", current_eval_line);
                printf("\n");
            }
        }
        s = s->parent;
    }

    if (total_vars == 0 && (!super_global_scope || !super_global_scope->vars))
        printf("  (no hay variables definidas)\n");

    return val_make_null();
}

/* --- vartype() --- */
static Value builtin_vartype(int argc, Value *args) {
    if (argc < 1) error(0, "vartype requiere un argumento");
    Value arg = args[0];
    const char *t = value_type_name(arg.type);
    return val_string(t);
}

static void redraw_input(const char *prompt, const char *buffer, size_t length, size_t cursor) {
    printf("\r%s%s\033[K", prompt ? prompt : "", buffer);
    if (length > cursor) printf("\033[%zuD", length - cursor);
    fflush(stdout);
}

static int read_byte(unsigned char *out) {
    ssize_t n;
    do {
        n = read(STDIN_FILENO, out, 1);
    } while (n < 0 && errno == EINTR);
    return n == 1 ? 1 : 0;
}

static Value input_line_editor(const char *prompt) {
    struct termios original, raw;
    if (tcgetattr(STDIN_FILENO, &original) != 0) return val_string("");
    raw = original;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return val_string("");

    size_t capacity = 128;
    size_t length = 0;
    size_t cursor = 0;
    char *buffer = malloc(capacity);
    if (!buffer) {
        tcsetattr(STDIN_FILENO, TCSANOW, &original);
        error(current_eval_line, "Memoria insuficiente en input()");
    }
    buffer[0] = '\0';

    for (;;) {
        unsigned char c;
        if (!read_byte(&c)) break;

        if (c == '\r' || c == '\n') {
            printf("\033[0m\n");
            fflush(stdout);
            break;
        }
        if (c == 4) { /* Ctrl-D */
            if (length == 0) {
                printf("\033[0m\n");
                fflush(stdout);
                free(buffer);
                tcsetattr(STDIN_FILENO, TCSANOW, &original);
                return val_string("");
            }
            continue;
        }
        if (c == 127 || c == 8) {
            if (cursor > 0) {
                memmove(buffer + cursor - 1, buffer + cursor, length - cursor + 1);
                length--;
                cursor--;
                redraw_input(prompt, buffer, length, cursor);
            }
            continue;
        }
        if (c == 27) {
            unsigned char a, b, d;
            if (!read_byte(&a)) continue;
            if (a == '[') {
                if (!read_byte(&b)) continue;
                if (b >= '0' && b <= '9') {
                    if (!read_byte(&d)) continue;
                    if (b == '3' && d == '~' && cursor < length) {
                        memmove(buffer + cursor, buffer + cursor + 1, length - cursor);
                        length--;
                        redraw_input(prompt, buffer, length, cursor);
                    }
                    continue;
                }
                switch (b) {
                    case 'D': if (cursor > 0) cursor--; redraw_input(prompt, buffer, length, cursor); break;
                    case 'C': if (cursor < length) cursor++; redraw_input(prompt, buffer, length, cursor); break;
                    case 'H': cursor = 0; redraw_input(prompt, buffer, length, cursor); break;
                    case 'F': cursor = length; redraw_input(prompt, buffer, length, cursor); break;
                    case 'A': /* up: reservado para historial */ break;
                    case 'B': /* down: reservado para historial */ break;
                    default: break;
                }
            } else if (a == 'O') {
                if (!read_byte(&b)) continue;
                if (b == 'H') cursor = 0;
                else if (b == 'F') cursor = length;
                redraw_input(prompt, buffer, length, cursor);
            }
            continue;
        }
        if (c < 32) continue;
        if (length + 1 >= capacity) {
            size_t new_capacity = (capacity < 4096) ? capacity * 2 : capacity + 4096;
            if (new_capacity > 1024 * 1024) {
                free(buffer);
                tcsetattr(STDIN_FILENO, TCSANOW, &original);
                error(current_eval_line, "La entrada es demasiado larga");
            }
            char *tmp = realloc(buffer, new_capacity);
            if (!tmp) {
                free(buffer);
                tcsetattr(STDIN_FILENO, TCSANOW, &original);
                error(current_eval_line, "Memoria insuficiente en input()");
            }
            buffer = tmp;
            capacity = new_capacity;
        }
        memmove(buffer + cursor + 1, buffer + cursor, length - cursor + 1);
        buffer[cursor] = (char)c;
        length++;
        cursor++;
        redraw_input(prompt, buffer, length, cursor);
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &original);
    Value result = val_string(buffer);
    free(buffer);
    return result;
}

/* --- input() --- */
static Value builtin_input(int argc, Value *args) {
    const char *prompt = "";
    if (argc >= 1 && args[0].type == VAL_STRING) prompt = args[0].data.sval;

    if (prompt[0]) {
        printf("%s", prompt);
        fflush(stdout);
    }

    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))
        return input_line_editor(prompt);

    char buffer[4096];
    if (!fgets(buffer, sizeof(buffer), stdin)) {
        printf("\033[0m");
        fflush(stdout);
        return val_string("");
    }
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') buffer[len - 1] = '\0';
    printf("\033[0m");
    fflush(stdout);
    return val_string(buffer);
}

/* ================================================
 *  Registro de funciones
 * ================================================ */

void register_io_builtins(void) {
    func_register_builtin("printAllVars", builtin_printAllVars);
    func_register_builtin("vartype", builtin_vartype);
    func_register_builtin("input", builtin_input);

    vm_register_builtin("printAllVars", builtin_printAllVars);
    vm_register_builtin("vartype", builtin_vartype);
    vm_register_builtin("input", builtin_input);
}
