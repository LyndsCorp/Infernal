/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/scope.c
 */

#include "scope.h"
#include "core/memory.h"
#include "core/value.h"
#include "developer/debug.h"
#include "runtime/constants.h"
#include "runtime/error.h"
#include "runtime/evaluator/evaluator.h"
#include "runtime/evaluator/helpers.h"
#include <stdlib.h>
#include <string.h>

/* --- Declaraciones de ámbitos globales (definidos en globals.c) --- */
extern Scope *global_scope;
extern Scope *super_global_scope;
extern int current_eval_line;

typedef struct ScopeRegistryEntry {
    Scope *scope;
    struct ScopeRegistryEntry *next;
} ScopeRegistryEntry;

static ScopeRegistryEntry *scope_registry = NULL;
static uint64_t next_scope_identity = 1;
static uint64_t global_variable_generation = 1;

static void bump_global_variable_generation(void) {
    if (++global_variable_generation == 0)
        global_variable_generation = 1;
}

uint64_t scope_cache_generation(void) { return global_variable_generation; }

static unsigned long scope_name_hash(const char *name) {
    unsigned long hash = 1469598103934665603UL;
    while (*name) {
        hash ^= (unsigned char)*name++;
        hash *= 1099511628211UL;
    }
    return hash;
}

static void scope_index_insert(Scope *scope, VarEntry *entry) {
    if (scope->var_bucket_count == 0) {
        scope->var_bucket_count = 16;
        scope->var_buckets = infernal_calloc(scope->var_bucket_count, sizeof(*scope->var_buckets));
    } else if ((scope->var_count + 1) * 4 >= scope->var_bucket_count * 3) {
        size_t old_count = scope->var_bucket_count;
        VarEntry **old_buckets = scope->var_buckets;
        scope->var_bucket_count *= 2;
        scope->var_buckets = infernal_calloc(scope->var_bucket_count, sizeof(*scope->var_buckets));
        for (size_t i = 0; i < old_count; i++) {
            VarEntry *item = old_buckets[i];
            while (item) {
                VarEntry *next = item->hash_next;
                size_t bucket = scope_name_hash(item->name) % scope->var_bucket_count;
                item->hash_next = scope->var_buckets[bucket];
                scope->var_buckets[bucket] = item;
                item = next;
            }
        }
        free(old_buckets);
    }
    size_t bucket = scope_name_hash(entry->name) % scope->var_bucket_count;
    entry->hash_next = scope->var_buckets[bucket];
    scope->var_buckets[bucket] = entry;
    scope->var_count++;
    scope->variable_version++;
    bump_global_variable_generation();
}

static void scope_registry_add(Scope *scope) {
    ScopeRegistryEntry *entry = infernal_malloc(sizeof(*entry));
    entry->scope = scope;
    entry->next = scope_registry;
    scope_registry = entry;
}

static void scope_registry_remove(Scope *scope) {
    ScopeRegistryEntry **link = &scope_registry;
    while (*link) {
        if ((*link)->scope == scope) {
            ScopeRegistryEntry *entry = *link;
            *link = entry->next;
            free(entry);
            return;
        }
        link = &(*link)->next;
    }
}

/* --- Creación de un nuevo ámbito --- */
Scope *scope_new(Scope *parent, const char *function_name) {
    Scope *s = infernal_malloc(sizeof(Scope));
    s->vars = NULL;
    s->var_buckets = NULL;
    s->var_bucket_count = 0;
    s->var_count = 0;
    s->identity = next_scope_identity++;
    if (next_scope_identity == 0)
        next_scope_identity = 1;
    s->variable_version = 1;
    s->portals = NULL;
    s->parent = parent;
    /* Solo duplicamos si hay nombre: si es NULL, no reservamos
     * el byte de "" (que antes se colaba por cada scope anónimo). */
    s->function_name = (function_name && function_name[0]) ? infernal_strdup(function_name) : NULL;
    s->func_def = NULL;
    scope_registry_add(s);
    return s;
}

Scope *scope_new_func(Scope *parent, ASTNode *func_def) {
    Scope *s = scope_new(parent, NULL);
    s->func_def = func_def;
    return s;
}

/* ================================================================
 * BÚSQUEDA DE VARIABLE:
 * - Recorre la cadena de ámbitos (desde el actual hacia arriba).
 * - Si no encuentra REAL en la cadena, busca en global_scope (script) y luego en
 * super_global_scope.
 * - Devuelve la primera variable declarada más cercana o NULL si no existe.
 * ================================================================ */
VarEntry *scope_find(Scope *scope, const char *name) {
    if (!scope)
        return NULL;
    /* Calculamos el hash UNA VEZ y lo reutilizamos en cada nivel.
     * scope_find_current() lo recalcularía en cada scope. Para
     * cadenas de 3-5 scopes son 2-4 hashes menos por lookup. */
    unsigned long hash = scope_name_hash(name);
    for (Scope *s = scope; s; s = s->parent) {
        if (!s->var_bucket_count)
            continue;
        size_t bucket = hash % s->var_bucket_count;
        for (VarEntry *e = s->var_buckets[bucket]; e; e = e->hash_next) {
            if (strcmp(e->name, name) == 0)
                return e;
        }
    }
    if (global_scope && global_scope != super_global_scope && global_scope->var_bucket_count) {
        size_t bucket = hash % global_scope->var_bucket_count;
        for (VarEntry *e = global_scope->var_buckets[bucket]; e; e = e->hash_next) {
            if (strcmp(e->name, name) == 0)
                return e;
        }
    }
    if (super_global_scope && super_global_scope->var_bucket_count) {
        size_t bucket = hash % super_global_scope->var_bucket_count;
        for (VarEntry *e = super_global_scope->var_buckets[bucket]; e; e = e->hash_next) {
            if (strcmp(e->name, name) == 0)
                return e;
        }
    }
    return NULL;
}

/* --- Búsqueda solo en el ámbito actual (no sube por la cadena) --- */
VarEntry *scope_find_current(Scope *scope, const char *name) {
    if (!scope || !scope->var_bucket_count)
        return NULL;
    size_t bucket = scope_name_hash(name) % scope->var_bucket_count;
    for (VarEntry *e = scope->var_buckets[bucket]; e; e = e->hash_next) {
        if (strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

/* --- Búsqueda en la cadena de ámbitos excluyendo super_global_scope --- */
VarEntry *scope_find_script(Scope *scope, const char *name) {
    while (scope && scope != super_global_scope) {
        VarEntry *e = scope_find_current(scope, name);
        if (e)
            return e;
        scope = scope->parent;
    }
    return NULL;
}

/* --- Definir una nueva variable en un ámbito dado --- */
void scope_define(Scope *scope, const char *name, int vtype, Value val) {
    if (constants_is_reserved(name)) {
        value_free(&val);
        error(current_eval_line,
              "El nombre '%s' está reservado por una constante y no se puede usar como variable",
              name);
    }
    /* Si el valor es NULL pero el llamador declaró un tipo explícito,
     * sustituimos por el valor por defecto de ese tipo. Antes se
     * degradaba vtype a 0 (dinámico), rompiendo silenciosamente el
     * chequeo de tipos en asignaciones posteriores. */
    if (val.type == VAL_NULL && vtype != 0) {
        switch (vtype) {
        case TOK_INT:
            val = val_int(0);
            break;
        case TOK_UINT:
            val = val_uint(0);
            break;
        case TOK_FLOAT:
            val = val_float(0.0);
            break;
        case TOK_BOOL:
            val = val_bool(false);
            break;
        case TOK_STRING:
            val = val_string("");
            break;
        case TOK_LIST:
            val = val_list_empty();
            break;
        case TOK_MAP:
            val = val_map_empty();
            break;
        default:
            break;
        }
    }
    if (vtype == TOK_UINT && !try_convert_value(&val, TOK_UINT)) {
        value_free(&val);
        error(current_eval_line, "Una variable uint requiere un entero válido");
    }
    DEBUG_INFO("scope_define: definiendo '%s' en scope %p (vtype=%d)", name, (void *)scope, vtype);
    if (vtype == TOK_STRING && val.type == VAL_LIST) {
        if (!try_convert_value(&val, TOK_STRING)) {
            // Si falla la conversión, se mantiene el valor original
        }
    }
    /* Evitar entradas duplicadas en el mismo scope: si ya existe una
     * variable con el mismo nombre, actualizamos su valor y tipo en
     * lugar de prepender una entrada nueva (que dejaría la anterior
     * inalcanzable hasta scope_free). */
    VarEntry *existing = scope_find_current(scope, name);
    if (existing) {
        value_free(&existing->value);
        existing->value = val;
        existing->vtype = vtype ? vtype : valtype_to_tokentype(val.type);
        DEBUG_INFO("scope_define: '%s' actualizada en scope %p", name, (void *)scope);
        return;
    }

    VarEntry *e = infernal_malloc(sizeof(VarEntry));
    e->name = infernal_strdup(name);
    e->vtype = vtype ? vtype : valtype_to_tokentype(val.type);
    e->value = val;
    e->next = scope->vars;
    scope->vars = e;
    scope_index_insert(scope, e);
    DEBUG_INFO("scope_define: '%s' definida en scope %p", name, (void *)scope);
}

void scope_remove_var_entry(Scope *scope, VarEntry *entry) {
    if (!scope || !entry || !scope->var_bucket_count)
        return;
    size_t bucket = scope_name_hash(entry->name) % scope->var_bucket_count;
    VarEntry **hash_link = &scope->var_buckets[bucket];
    while (*hash_link && *hash_link != entry)
        hash_link = &(*hash_link)->hash_next;
    if (*hash_link) {
        *hash_link = entry->hash_next;
        scope->var_count--;
        scope->variable_version++;
        bump_global_variable_generation();
    }
}

/* --- Asignar un valor a una variable existente en el ámbito (o crear si no existe) --- */
void scope_assign(Scope *scope, const char *name, Value val, int line) {
    if (constants_is_reserved(name)) {
        value_free(&val);
        error(line, "La constante '%s' no se puede sobreescribir", name);
    }
    VarEntry *e = scope_find(scope, name);
    if (e) {
        if (e->value.type == VAL_REFERENCE) {
            assign_reference(&e->value, val, line);
            return;
        }
        // Si el tipo fijo es 0 o inválido (>= TOK_EOF), permitir cualquier valor
        if (e->vtype == 0) {
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
                value_free(&val);
                error(line, "No se pudo convertir lista a string en la asignación a '%s'", name);
            }
        }
        int expected = e->vtype;
        int new_type = valtype_to_tokentype(val.type);
        if (expected == TOK_UINT && new_type != TOK_UINT) {
            if (!try_convert_value(&val, TOK_UINT)) {
                value_free(&val);
                error(line, "La variable uint '%s' requiere un entero válido", name);
            }
            new_type = valtype_to_tokentype(val.type);
        }
        if (expected != 0 && new_type != expected) {
            value_free(&val);
            error(line,
                  "Tipado fijo: la variable '%s' es de tipo %s, no se puede asignar un valor de "
                  "tipo %s",
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

/* Variante de scope_assign() que recibe el VarEntry ya localizado.
 * Hace exactamente lo mismo que la rama `if (e)` de scope_assign(),
 * pero sin repetir el scope_find(). */
void scope_assign_direct(VarEntry *e, Value val, int line) {
    if (!e)
        return;
    if (constants_is_reserved(e->name)) {
        value_free(&val);
        error(line, "La constante '%s' no se puede sobreescribir", e->name);
    }
    if (e->value.type == VAL_REFERENCE) {
        assign_reference(&e->value, val, line);
        return;
    }
    if (e->vtype == 0) {
        Value copied = copy_value_secure(val);
        value_free(&e->value);
        e->value = copied;
        e->vtype = valtype_to_tokentype(val.type);
        return;
    }
    if (e->vtype == TOK_STRING && val.type == VAL_LIST) {
        if (!try_convert_value(&val, TOK_STRING)) {
            value_free(&val);
            error(line, "No se pudo convertir lista a string en la asignación a '%s'", e->name);
        }
    }
    int expected = e->vtype;
    int new_type = valtype_to_tokentype(val.type);
    if (expected == TOK_UINT && new_type != TOK_UINT) {
        if (!try_convert_value(&val, TOK_UINT)) {
            value_free(&val);
            error(line, "La variable uint '%s' requiere un entero válido", e->name);
        }
        new_type = valtype_to_tokentype(val.type);
    }
    if (expected != 0 && new_type != expected) {
        value_free(&val);
        error(
            line,
            "Tipado fijo: la variable '%s' es de tipo %s, no se puede asignar un valor de tipo %s",
            e->name, type_name(expected), type_name(new_type));
    }
    Value copied = copy_value_secure(val);
    value_free(&e->value);
    e->value = copied;
}

/* --- Búsqueda de portal en la cadena de ámbitos --- */
PortalEntry *portal_find(Scope *scope, const char *name) {
    while (scope) {
        for (PortalEntry *p = scope->portals; p; p = p->next) {
            if (strcmp(p->name, name) == 0)
                return p;
        }
        scope = scope->parent;
    }
    return NULL;
}

/* --- Búsqueda de portal solo en el ámbito actual --- */
PortalEntry *portal_find_in_scope(Scope *scope, const char *name) {
    for (PortalEntry *p = scope->portals; p; p = p->next) {
        if (strcmp(p->name, name) == 0)
            return p;
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

/* --- Vaciar un ámbito sin liberar la estructura ---
 *
 * Libera todas las variables y portales del scope, pero mantiene
 * la identidad, el array de buckets hash y function_name intactos.
 * Pensado para reutilizar el mismo Scope en bucles apretados, donde
 * hacer scope_new() + scope_free() por iteración añade dos malloc
 * por vuelta. */
void scope_reset(Scope *s) {
    if (!s)
        return;
    if (s->var_count)
        bump_global_variable_generation();
    VarEntry *e = s->vars;
    while (e) {
        VarEntry *next = e->next;
        free(e->name);
        value_free(&e->value);
        free(e);
        e = next;
    }
    s->vars = NULL;
    if (s->var_buckets) {
        memset(s->var_buckets, 0, s->var_bucket_count * sizeof(*s->var_buckets));
    }
    s->var_count = 0;
    s->variable_version++;
    PortalEntry *p = s->portals;
    while (p) {
        PortalEntry *next = p->next;
        free(p->name);
        free(p);
        p = next;
    }
    s->portals = NULL;
}

/* --- Liberar un ámbito (se desactiva la liberación de variables para evitar double-free) --- */
void scope_free(Scope *s) {
    if (!s)
        return;
    if (s->var_count)
        bump_global_variable_generation();
    scope_registry_remove(s);
    VarEntry *e = s->vars;
    while (e) {
        VarEntry *next = e->next;
        free(e->name);
        value_free(&e->value);
        free(e);
        e = next;
    }
    free(s->var_buckets);
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

void scope_free_all(void) {
    while (scope_registry) {
        Scope *scope = scope_registry->scope;
        scope_free(scope);
    }
}
