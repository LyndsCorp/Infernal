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
 * BÚSQUEDA DE VARIABLE: prioriza valores reales sobre NULL
 * ================================================================ */
VarEntry *scope_find(Scope *scope, const char *name) {
    DEBUG_INFO("scope_find: buscando '%s'", name);

    VarEntry *found_null = NULL;   // guardar la primera variable NULL encontrada en la cadena
    Scope *s = scope;

    // 1) Recorrer la cadena de ámbitos (desde el actual hacia arriba)
    while (s) {
        DEBUG_INFO("scope_find: revisando scope %p", (void*)s);
        for (VarEntry *e = s->vars; e; e = e->next) {
            if (strcmp(e->name, name) == 0) {
                if (e->value.type == VAL_NULL) {
                    // Si es NULL, la recordamos pero no devolvemos aún
                    if (!found_null) found_null = e;
                    DEBUG_INFO("scope_find: encontrado NULL en scope %p", (void*)s);
                } else {
                    // Si tiene valor real, ¡es la ganadora! (prioridad máxima)
                    DEBUG_INFO("scope_find: encontrado valor REAL en scope %p, devolviendo", (void*)s);
                    return e;
                }
            }
        }
        s = s->parent;
    }

    // 2) Si no encontramos valor real en la cadena, buscar en super_global_scope
    if (super_global_scope) {
        DEBUG_INFO("scope_find: buscando en super_global_scope");
        for (VarEntry *e = super_global_scope->vars; e; e = e->next) {
            if (strcmp(e->name, name) == 0) {
                if (e->value.type != VAL_NULL) {
                    DEBUG_INFO("scope_find: encontrado valor REAL en super_global_scope");
                    return e;
                } else {
                    DEBUG_INFO("scope_find: encontrado NULL en super_global_scope (ignorado)");
                }
            }
        }
    }

    // 3) Si no, buscar en global_scope (si es diferente de super_global_scope)
    if (global_scope && global_scope != super_global_scope) {
        DEBUG_INFO("scope_find: buscando en global_scope");
        for (VarEntry *e = global_scope->vars; e; e = e->next) {
            if (strcmp(e->name, name) == 0) {
                if (e->value.type != VAL_NULL) {
                    DEBUG_INFO("scope_find: encontrado valor REAL en global_scope");
                    return e;
                } else {
                    DEBUG_INFO("scope_find: encontrado NULL en global_scope (ignorado)");
                }
            }
        }
    }

    // 4) Si todo falló, pero teníamos una variable NULL en la cadena, la devolvemos
    if (found_null) {
        DEBUG_INFO("scope_find: devolviendo variable NULL de la cadena (sin alternativa)");
        return found_null;
    }

    DEBUG_INFO("scope_find: NO encontrado '%s'", name);
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
    // Si el valor es NULL, no asignar un tipo fijo para evitar conflictos
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
        // Si el nuevo valor es NULL, permitir la asignación sin verificación de tipo
        if (val.type == VAL_NULL) {
            e->value = val;
            return;
        }
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
