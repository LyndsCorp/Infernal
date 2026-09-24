/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: stdlib/io.c
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include "io.h"
#include "core/value.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "runtime/constants.h"
#include "runtime/evaluator/helpers.h"
#include "vm/vm.h"
#include "stdlib/output.h"

/* --- Impresión "de inspección" --------------------------------
 *
 * Igual que print_value(), pero entrecomilla los strings, tanto en el
 * nivel superior como dentro de listas y mapas. Se usa en printAllVars()
 * para que al inspeccionar variables quede claro qué valores son texto:
 *   variable: "Hola"   (string)
 *   n: 42              (int)
 *   lista: [1, "dos", ["k" = "v"]]
 *
 * print() sigue usando print_value(), que imprime strings sin comillas,
 * porque ahí el usuario está presentando datos, no depurándolos.
 */
static void print_value_for_inspection(Value v) {
    switch (v.type) {
        case VAL_STRING:
            putchar('"');
            if (v.data.sval) {
                for (const unsigned char *p = (const unsigned char *)v.data.sval; *p; ++p) {
                    switch (*p) {
                        case '\\': printf("\\\\"); break;
                        case '"':  printf("\\\""); break;
                        case '\n': printf("\\n");  break;
                        case '\r': printf("\\r");  break;
                        case '\t': printf("\\t");  break;
                        default:   putchar(*p);    break;
                    }
                }
            }
            putchar('"');
            break;

        case VAL_LIST:
            printf("[");
            for (int i = 0; i < v.data.list.count; i++) {
                if (i > 0) printf(", ");
                print_value_for_inspection(v.data.list.items[i]);
            }
            printf("]");
            break;

        case VAL_MAP: {
            printf("[");
            MapData *md = v.data.map;
            if (md) {
                for (int i = 0; i < md->count; i++) {
                    if (i > 0) printf(", ");
                    printf("%s = ", md->pairs[i].key ? md->pairs[i].key : "");
                    print_value_for_inspection(md->pairs[i].value);
                }
            }
            printf("]");
            break;
        }

        default:
            /* int, float, bool, null, reference, ptr */
            print_value(v);
            break;
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
        print_value_for_inspection(resolved);
        printf(" (%s)", value_type_name(resolved.type));
        value_free(&resolved);
        return;
    }
    printf(": ");
    print_value_for_inspection(entry->value);
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

/* --- Helpers para printAllFunctions() ------------------------- */

/*
 * Comprueba si una función de usuario tiene un "hermano" con prefijo de
 * módulo. Cuando el parser importa un .fire, registra la función DOS
 * veces: una con el nombre desnudo y otra con el prefijo del módulo
 * ("mod.func"). Aquí detectamos ese caso para no listar dos veces la
 * misma función.
 */
static bool has_dotted_sibling(FuncEntry *e) {
    if (!e->obj || e->obj->kind != FUNC_USER || !e->obj->def) return false;
    ASTNode *def = e->obj->def;
    for (FuncEntry *other = func_table; other; other = other->next) {
        if (other == e) continue;
        if (!other->obj || other->obj->kind != FUNC_USER) continue;
        if (other->obj->def != def) continue;
        if (strchr(other->name, '.') != NULL) return true;
    }
    return false;
}

/* Imprime "nombre(tipo param, tipo param, ...)" sin salto de línea. */
static void print_function_signature(FuncEntry *e) {
    printf("%s(", e->name);
    ASTNode *def = e->obj->def;
    if (def && def->kind == NODE_FUNC_DEF) {
        for (int i = 0; i < def->data.func.param_count; i++) {
            if (i > 0) printf(", ");
            int ptype = (def->data.func.ptypes) ? def->data.func.ptypes[i] : 0;
            if (ptype != 0) printf("%s ", type_name(ptype));
            printf("%s", def->data.func.params[i]);
        }
    }
    printf(")");
}

/* --- printAllFunctions() --- */
static Value builtin_printAllFunctions(int argc, Value *args) {
    (void)argc; (void)args;

    printf("Funciones disponibles:\n");

    /* --- 1. Funciones definidas en el script principal ---
     *
     * Filtramos las que tienen "." en el nombre (esas vienen de imports)
     * y también las que tienen un hermano "con punto" (duplicado que el
     * parser registra al importar un .fire). */
    printf("  Funciones definidas en el script:\n");
    int n = 0;
    for (FuncEntry *e = func_table; e; e = e->next) {
        if (!e->obj || e->obj->kind != FUNC_USER) continue;
        if (strchr(e->name, '.') != NULL) continue;
        if (has_dotted_sibling(e)) continue;
        printf("    ");
        print_function_signature(e);
        printf("\n");
        n++;
    }
    if (n == 0) printf("    (ninguna)\n");

    /* --- 2. Funciones importadas de módulos .fire --- */
    printf("\n  Funciones importadas de módulos .fire:\n");
    n = 0;
    for (FuncEntry *e = func_table; e; e = e->next) {
        if (!e->obj || e->obj->kind != FUNC_USER) continue;
        if (strchr(e->name, '.') == NULL) continue;
        printf("    ");
        print_function_signature(e);
        printf("\n");
        n++;
    }
    if (n == 0) printf("    (ninguna)\n");

    return val_make_null();
}

/* --- Helpers para printAllBuiltins() -------------------------- */

static void print_constant_visitor(const char *name,
                                   int vtype,
                                   const Value *value,
                                   bool internal,
                                   void *user_data) {
    (void)user_data;
    printf("    %s (%s%s) = ", name, type_name(vtype), internal ? "" : "");
    if (value) {
        print_value_for_inspection(*value);
    } else {
        printf("null");
    }
    printf("\n");
}

/* --- printAllBuiltins() ---
 *
 * Lista las funciones nativas (builtin) y todas las constantes
 * registradas (internas y definidas por el usuario), mostrando el tipo
 * y el valor actual de cada constante. Para las constantes internas
 * cuyo valor está respaldado por una variable de C (por ejemplo
 * _MAX_LOOP_LIMIT) se consulta siempre el valor vivo mediante el
 * getter, así que la salida refleja el estado real del intérprete. */
static Value builtin_printAllBuiltins(int argc, Value *args) {
    (void)argc; (void)args;

    printf("Elementos builtin disponibles:\n");

    /* --- 1. Funciones builtin --- */
    printf("  Funciones builtin (funciones nativas):\n");
    int n = 0;
    for (FuncEntry *e = func_table; e; e = e->next) {
        if (e->obj && e->obj->kind == FUNC_BUILTIN) {
            printf("    %s()\n", e->name);
            n++;
        }
    }
    if (n == 0) printf("    (ninguna)\n");

    /* --- 2. Constantes --- */
    printf("\n  Constantes:\n");
    constants_foreach(print_constant_visitor, NULL);

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
    func_register_builtin("printAllVars",      builtin_printAllVars);
    func_register_builtin("printAllFunctions", builtin_printAllFunctions);
    func_register_builtin("printAllBuiltins",  builtin_printAllBuiltins);
    func_register_builtin("vartype",           builtin_vartype);
    func_register_builtin("input",             builtin_input);

    vm_register_builtin("printAllVars",      builtin_printAllVars);
    vm_register_builtin("printAllFunctions", builtin_printAllFunctions);
    vm_register_builtin("printAllBuiltins",  builtin_printAllBuiltins);
    vm_register_builtin("vartype",           builtin_vartype);
    vm_register_builtin("input",             builtin_input);
}
