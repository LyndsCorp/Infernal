/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License, Lynds Corp., Aros Legendarios, David Baña Szymaniak.
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

static Value builtin_printAllVars(int argc, Value *args) {
    (void)argc; (void)args;

    printf("Variables accesibles:\n");

    Scope *s = current_scope;
    int total_vars = 0;

    while (s) {
        // Solo mostrar el ámbito si tiene variables
        if (s->vars) {
            printf("  Scope %p:\n", (void*)s);
            for (VarEntry *e = s->vars; e; e = e->next) {
                total_vars++;
                printf("    %s: ", e->name);
                // Imprimir valor (igual que antes)...
                switch (e->value.type) {
                    case VAL_INT:    printf("%d", e->value.data.ival); break;
                    case VAL_FLOAT:  printf("%g", e->value.data.fval); break;
                    case VAL_BOOL:   printf("%s", e->value.data.bval ? "true" : "false"); break;
                    case VAL_STRING: printf("\"%s\"", e->value.data.sval); break;
                    case VAL_LIST: {
                        printf("[");
                        for (int j = 0; j < e->value.data.list.count; j++) {
                            if (j > 0) printf(", ");
                            Value *item = &e->value.data.list.items[j];
                            switch (item->type) {
                                case VAL_INT:    printf("%d", item->data.ival); break;
                                case VAL_FLOAT:  printf("%g", item->data.fval); break;
                                case VAL_BOOL:   printf("%s", item->data.bval ? "true" : "false"); break;
                                case VAL_STRING: printf("\"%s\"", item->data.sval); break;
                                default: printf("?");
                            }
                        }
                        printf("]");
                        break;
                    }
                                default: printf("[?]");
                }
                if (e->vtype) {
                    const char *tname = (e->vtype == TOK_INT) ? "int" :
                    (e->vtype == TOK_FLOAT) ? "float" :
                    (e->vtype == TOK_BOOL) ? "bool" :
                    (e->vtype == TOK_STRING) ? "string" :
                    (e->vtype == TOK_LIST) ? "list" : "?";
                    printf(" (%s)", tname);
                }
                printf("\n");
            }
        }
        s = s->parent;
    }

    if (total_vars == 0) {
        printf("  (no hay variables definidas)\n");
    }

    return val_make_null();
}

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
        case VAL_NULL:   t = "null";   break;
        default:         t = "unknown";
    }
    return val_string(t);
}

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

void register_io_builtins(void) {
    func_register_builtin("printAllVars", builtin_printAllVars);
    func_register_builtin("vartype", builtin_vartype);
    func_register_builtin("input", builtin_input);

    vm_register_builtin("printAllVars", builtin_printAllVars);
    vm_register_builtin("vartype", builtin_vartype);
    vm_register_builtin("input", builtin_input);
}
