/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License.
 * Código fuente de Infernal: runtime/scope.c
*/

#include <stdlib.h>
#include <string.h>
#include "scope.h"
#include "core/value.h"
#include "runtime/error.h"
#include "core/memory.h"
#include "runtime/evaluator.h"   // <-- añadir para try_convert_value

Scope *scope_new(Scope *parent, const char *function_name) {
    Scope *s = infernal_malloc(sizeof(Scope));
    s->vars = NULL;
    s->portals = NULL;
    s->parent = parent;
    s->function_name = function_name ? infernal_strdup(function_name) : NULL;
    return s;
}

VarEntry *scope_find(Scope *scope, const char *name) {
    while (scope) {
        for (VarEntry *e = scope->vars; e; e = e->next) {
            if (strcmp(e->name, name) == 0) return e;
        }
        scope = scope->parent;
    }
    return NULL;
}

VarEntry *scope_find_current(Scope *scope, const char *name) {
    if (!scope) return NULL;
    for (VarEntry *e = scope->vars; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

VarEntry *scope_find_script(Scope *scope, const char *name) {
    extern Scope *super_global_scope;
    while (scope && scope != super_global_scope) {
        for (VarEntry *e = scope->vars; e; e = e->next) {
            if (strcmp(e->name, name) == 0) return e;
        }
        scope = scope->parent;
    }
    return NULL;
}

void scope_define(Scope *scope, const char *name, int vtype, Value val) {
    // Si se define como string y el valor es lista, convertir
    if (vtype == TOK_STRING && val.type == VAL_LIST) {
        if (!try_convert_value(&val, TOK_STRING)) {
            // En caso de fallo, mantener el valor original (se mostrará error más adelante)
        }
    }
    VarEntry *e = infernal_malloc(sizeof(VarEntry));
    e->name = infernal_strdup(name);
    e->vtype = vtype ? vtype : valtype_to_tokentype(val.type);
    e->value = val;
    e->next = scope->vars;
    scope->vars = e;
}

void scope_assign(Scope *scope, const char *name, Value val, int line) {
    VarEntry *e = scope_find(scope, name);
    if (e) {
        // Si la variable es de tipo string y el valor es lista, convertir
        if (e->vtype == TOK_STRING && val.type == VAL_LIST) {
            if (!try_convert_value(&val, TOK_STRING)) {
                error(line, "No se pudo convertir lista a string en la asignación a '%s'", name);
            }
        }
        int expected = e->vtype;
        int new_type = valtype_to_tokentype(val.type);
        if (expected != 0 && new_type != expected) {
            error(line, "Tipado fijo: la variable '%s' es de tipo %s, no se puede asignar un valor de tipo %s",
                  name,
                  expected == TOK_INT ? "int" : expected == TOK_FLOAT ? "float" :
                  expected == TOK_BOOL ? "bool" : expected == TOK_STRING ? "string" :
                  expected == TOK_LIST ? "list" : "?",
                  new_type == TOK_INT ? "int" : new_type == TOK_FLOAT ? "float" :
                  new_type == TOK_BOOL ? "bool" : new_type == TOK_STRING ? "string" :
                  new_type == TOK_LIST ? "list" : "?");
        }
        e->value = val;
    } else {
        scope_define(scope, name, 0, val);
    }
}

PortalEntry *portal_find(Scope *scope, const char *name) {
    while (scope) {
        for (PortalEntry *p = scope->portals; p; p = p->next) {
            if (strcmp(p->name, name) == 0) return p;
        }
        scope = scope->parent;
    }
    return NULL;
}

PortalEntry *portal_find_in_scope(Scope *scope, const char *name) {
    for (PortalEntry *p = scope->portals; p; p = p->next) {
        if (strcmp(p->name, name) == 0) return p;
    }
    return NULL;
}

void portal_define(Scope *scope, const char *name, int line) {
    PortalEntry *p = infernal_malloc(sizeof(PortalEntry));
    p->name = infernal_strdup(name);
    p->line = line;
    p->next = scope->portals;
    scope->portals = p;
}

void scope_free(Scope *s) {
    if (!s) return;
    // Desactivamos la liberación de variables para evitar double-free
    free(s);
}
