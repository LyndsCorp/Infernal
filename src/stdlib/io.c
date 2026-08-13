/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: stdlib/io.c
*/

#include <stdio.h>
#include <string.h>
#include "io.h"
#include "core/value.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "vm/vm.h"
#include "stdlib/output.h"


/* ================================================
 *  Funciones de la biblioteca
 * ================================================ */

/* --- printAllVars() --- */
static Value builtin_printAllVars(int argc, Value *args) {
    (void)argc; (void)args;

    printf("Variables accesibles:\n");

    if (super_global_scope && super_global_scope->vars) {
        printf("  Ámbito superglobal (compartido entre scripts):\n");
        for (VarEntry *e = super_global_scope->vars; e; e = e->next) {
            printf("    %s: ", e->name);
            print_value(e->value);
            if (e->vtype) {
                const char *tname = (e->vtype == TOK_INT) ? "int" :
                (e->vtype == TOK_FLOAT) ? "float" :
                (e->vtype == TOK_BOOL) ? "bool" :
                (e->vtype == TOK_STRING) ? "string" :
                (e->vtype == TOK_LIST) ? "list" :
                (e->vtype == TOK_MAP) ? "map" : "?";
                printf(" (%s)", tname);
            }
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
            if (s == global_scope) {
                printf("  Ámbito del script:\n");
            } else if (s->function_name) {
                printf("  Ámbito local de función '%s':\n", s->function_name);
            } else {
                printf("  Scope %p:\n", (void*)s);
            }
            for (VarEntry *e = s->vars; e; e = e->next) {
                total_vars++;
                printf("    %s: ", e->name);
                print_value(e->value);
                if (e->vtype) {
                    const char *tname = (e->vtype == TOK_INT) ? "int" :
                    (e->vtype == TOK_FLOAT) ? "float" :
                    (e->vtype == TOK_BOOL) ? "bool" :
                    (e->vtype == TOK_STRING) ? "string" :
                    (e->vtype == TOK_LIST) ? "list" :
                    (e->vtype == TOK_MAP) ? "map" : "?";
                    printf(" (%s)", tname);
                }
                printf("\n");
            }
        }
        s = s->parent;
    }

    if (total_vars == 0 && (!super_global_scope || !super_global_scope->vars)) {
        printf("  (no hay variables definidas)\n");
    }

    return val_make_null();
}

/* --- vartype() --- */
static Value builtin_vartype(int argc, Value *args) {
    if (argc < 1) error(0, "vartype requiere un argumento");
    Value arg = args[0];
    const char *t = "unknown";
    switch (arg.type) {
        case VAL_INT:    t = "int";    break;
        case VAL_FLOAT:  t = "float";  break;
        case VAL_BOOL:   t = "bool";   break;
        case VAL_STRING: t = "string"; break;
        case VAL_LIST:   t = "list";   break;
        case VAL_MAP:    t = "map";    break;
        case VAL_NULL:   t = "null";   break;
        default:         t = "unknown";
    }
    return val_string(t);
}

/* --- input() --- */
static Value builtin_input(int argc, Value *args) {
    if (argc >= 1 && args[0].type == VAL_STRING) {
        printf("%s", args[0].data.sval);
        fflush(stdout);
    }
    char buffer[4096];
    if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
        return val_string("");
    }
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
        buffer[len - 1] = '\0';
    }
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
