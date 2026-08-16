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
    DEBUG_INFO("scope_find: buscando '%s'", name);

    VarEntry *null_candidate = NULL;

    Scope *s = scope;

    while (s) {
        DEBUG_INFO("scope_find: revisando scope %p", (void *)s);

        for (VarEntry *e = s->vars; e; e = e->next) {
            if (strcmp(e->name, name) != 0)
                continue;

            /*
             * Guardamos una coincidencia NULL como candidato,
             * pero seguimos buscando por si existe una variable
             * REAL en un ámbito superior.
             */
            if (e->value.type == VAL_NULL) {
                if (!null_candidate) {
                    null_candidate = e;
                }

                DEBUG_INFO(
                    "scope_find: encontrado '%s' en scope %p "
                    "pero su valor es NULL; continuando búsqueda",
                    name,
                    (void *)s
                );

                continue;
            }

            DEBUG_INFO(
                "scope_find: encontrado '%s' en scope %p "
                "(type=%d)",
                       name,
                       (void *)s,
                       e->value.type
            );

            return e;
        }

        s = s->parent;
    }

    /*
     * Buscar también explícitamente en global_scope.
     */
    if (global_scope && global_scope != super_global_scope) {
        DEBUG_INFO("scope_find: buscando en global_scope");

        for (VarEntry *e = global_scope->vars; e; e = e->next) {
            if (strcmp(e->name, name) != 0)
                continue;

            if (e->value.type == VAL_NULL) {
                if (!null_candidate) {
                    null_candidate = e;
                }

                DEBUG_INFO(
                    "scope_find: '%s' existe en global_scope "
                    "pero es NULL; continuando búsqueda",
                    name
                );

                continue;
            }

            DEBUG_INFO(
                "scope_find: encontrado '%s' en global_scope",
                name
            );

            return e;
        }
    }

    /*
     * Buscar en super_global_scope.
     */
    if (super_global_scope) {
        DEBUG_INFO("scope_find: buscando en super_global_scope");

        for (VarEntry *e = super_global_scope->vars; e; e = e->next) {
            if (strcmp(e->name, name) != 0)
                continue;

            if (e->value.type == VAL_NULL) {
                if (!null_candidate) {
                    null_candidate = e;
                }

                DEBUG_INFO(
                    "scope_find: '%s' existe en super_global_scope "
                    "pero es NULL",
                    name
                );

                continue;
            }

            DEBUG_INFO(
                "scope_find: encontrado '%s' en super_global_scope",
                name
            );

            return e;
        }
    }

    /*
     * Si solo encontramos una variable NULL, devolverla.
     * Así mantenemos la semántica existente cuando no hay
     * ninguna versión con valor real.
     */
    if (null_candidate) {
        DEBUG_INFO(
            "scope_find: usando candidato NULL para '%s'",
            name
        );

        return null_candidate;
    }

    DEBUG_INFO(
        "scope_find: NO encontrado '%s'",
        name
    );

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
            e->value = copy_value_secure(val);
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
        e->value = copy_value_secure(val);
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
    free(s);
}
