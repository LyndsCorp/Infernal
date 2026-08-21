/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/scope.c
*/

#include <stdlib.h>
#include <string.h>
#include "scope.h"
#include "core/value.h"
#include "runtime/error.h"
#include "core/memory.h"
#include "runtime/evaluator/evaluator.h"
#include "runtime/evaluator/helpers.h"
#include "developer/debug.h"

/* --- Declaraciones de ámbitos globales (definidos en globals.c) --- */
extern Scope *global_scope;
extern Scope *super_global_scope;

/* --- Creación de un nuevo ámbito --- */
Scope *scope_new(Scope *parent, const char *function_name) {
    Scope *s = infernal_malloc(sizeof(Scope));
    s->vars = NULL;
    s->portals = NULL;
    s->parent = parent;
    s->function_name = function_name ? infernal_strdup(function_name) : NULL;
    return s;
}

/* ================================================================
 * BÚSQUEDA DE VARIABLE:
 * - Recorre la cadena de ámbitos (desde el actual hacia arriba).
 * - Prioriza valores REAL sobre NULL.
 * - Si no encuentra REAL en la cadena, busca en global_scope (script) y luego en super_global_scope.
 * - Devuelve la variable REAL más cercana (en profundidad) o NULL si no existe.
 * ================================================================ */
VarEntry *scope_find(Scope *scope, const char *name) {
    if (!scope) return NULL;

    /*
     * Resolución léxica normal: la primera declaración encontrada gana,
     * incluso aunque su valor sea NULL. Un NULL local debe ocultar una
     * variable homónima del ámbito padre.
     */
    for (Scope *s = scope; s; s = s->parent) {
        for (VarEntry *e = s->vars; e; e = e->next) {
            if (strcmp(e->name, name) == 0)
                return e;
        }
    }

    /* Compatibilidad con los ámbitos globales que pueden no formar parte
     * de la cadena de padres durante la inicialización/importación. */
    if (global_scope && global_scope != super_global_scope) {
        for (VarEntry *e = global_scope->vars; e; e = e->next) {
            if (strcmp(e->name, name) == 0)
                return e;
        }
    }
    if (super_global_scope) {
        for (VarEntry *e = super_global_scope->vars; e; e = e->next) {
            if (strcmp(e->name, name) == 0)
                return e;
        }
    }
    return NULL;
}

/* --- Búsqueda solo en el ámbito actual (no sube por la cadena) --- */
VarEntry *scope_find_current(Scope *scope, const char *name) {
    if (!scope) return NULL;
    for (VarEntry *e = scope->vars; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

/* --- Búsqueda en la cadena de ámbitos excluyendo super_global_scope --- */
VarEntry *scope_find_script(Scope *scope, const char *name) {
    while (scope && scope != super_global_scope) {
        for (VarEntry *e = scope->vars; e; e = e->next) {
            if (strcmp(e->name, name) == 0) return e;
        }
        scope = scope->parent;
    }
    return NULL;
}

/* --- Definir una nueva variable en un ámbito dado --- */
void scope_define(Scope *scope, const char *name, int vtype, Value val) {
    if (val.type == VAL_NULL) {
        vtype = 0;
    }
    DEBUG_INFO("scope_define: definiendo '%s' en scope %p (vtype=%d)", name, (void*)scope, vtype);
    if (vtype == TOK_STRING && val.type == VAL_LIST) {
        if (!try_convert_value(&val, TOK_STRING)) {
            // Si falla la conversión, se mantiene el valor original
        }
    }
    VarEntry *e = infernal_malloc(sizeof(VarEntry));
    e->name = infernal_strdup(name);
    e->vtype = vtype ? vtype : valtype_to_tokentype(val.type);
    e->value = val;
    e->next = scope->vars;
    scope->vars = e;
    DEBUG_INFO("scope_define: '%s' definida en scope %p", name, (void*)scope);
}

/* --- Asignar un valor a una variable existente en el ámbito (o crear si no existe) --- */
void scope_assign(Scope *scope, const char *name, Value val, int line) {
    VarEntry *e = scope_find(scope, name);
    if (e) {
        // Si el tipo fijo es 0 o inválido (>= TOK_EOF), permitir cualquier valor
        if (e->vtype == 0 || e->vtype >= TOK_EOF) {
            Value copied = copy_value_secure(val);
            value_free(&e->value);
            e->value = copied;
            /* Las variables dinámicas adquieren el tipo de su valor actual.
             * Esto permite que printAllVars() muestre el tipo real después de
             * una asignación, incluso cuando la variable fue registrada
             * previamente por el compilador con vtype=0. */
            e->vtype = valtype_to_tokentype(val.type);
            return;
        }
        // Conversión si el destino es string y el valor es lista
        if (e->vtype == TOK_STRING && val.type == VAL_LIST) {
            if (!try_convert_value(&val, TOK_STRING)) {
                error(line, "No se pudo convertir lista a string en la asignación a '%s'", name);
            }
        }
        int expected = e->vtype;
        int new_type = valtype_to_tokentype(val.type);
        if (expected != 0 && new_type != expected) {
            error(line, "Tipado fijo: la variable '%s' es de tipo %s, no se puede asignar un valor de tipo %s",
                  name, type_name(expected), type_name(new_type));
        }
        Value copied = copy_value_secure(val);
        value_free(&e->value);
        e->value = copied;
        return;
    }
    // Si no existe, definir en el ámbito actual
    scope_define(scope, name, 0, val);
}

/* --- Búsqueda de portal en la cadena de ámbitos --- */
PortalEntry *portal_find(Scope *scope, const char *name) {
    while (scope) {
        for (PortalEntry *p = scope->portals; p; p = p->next) {
            if (strcmp(p->name, name) == 0) return p;
        }
        scope = scope->parent;
    }
    return NULL;
}

/* --- Búsqueda de portal solo en el ámbito actual --- */
PortalEntry *portal_find_in_scope(Scope *scope, const char *name) {
    for (PortalEntry *p = scope->portals; p; p = p->next) {
        if (strcmp(p->name, name) == 0) return p;
    }
    return NULL;
}

/* --- Definir un portal en el ámbito dado --- */
void portal_define(Scope *scope, const char *name, int line) {
    PortalEntry *p = infernal_malloc(sizeof(PortalEntry));
    p->name = infernal_strdup(name);
    p->line = line;
    p->next = scope->portals;
    scope->portals = p;
}

/* --- Liberar un ámbito (se desactiva la liberación de variables para evitar double-free) --- */
void scope_free(Scope *s) {
    if (!s) return;
    VarEntry *e = s->vars;
    while (e) {
        VarEntry *next = e->next;
        free(e->name);
        value_free(&e->value);
        free(e);
        e = next;
    }
    PortalEntry *p = s->portals;
    while (p) {
        PortalEntry *next = p->next;
        free(p->name);
        free(p);
        p = next;
    }
    free(s->function_name);
    free(s);
}
