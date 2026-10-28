/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/scope.h
*/

#ifndef RUNTIME_SCOPE_H
#define RUNTIME_SCOPE_H

#include <stddef.h>
#include "core/types.h"

typedef struct VarEntry {
    char *name;
    int vtype;
    Value value;
    struct VarEntry *next;
    struct VarEntry *hash_next;
} VarEntry;

typedef struct PortalEntry {
    char *name;
    int line;
    struct PortalEntry *next;
} PortalEntry;

typedef struct Scope {
    VarEntry *vars;
    VarEntry **var_buckets;
    size_t var_bucket_count;
    size_t var_count;
    uint64_t identity;
    uint64_t variable_version;
    PortalEntry *portals;
    struct Scope *parent;
    char *function_name;  // nombre de la función si es un ámbito de función, o NULL
    /* Si el scope es de una función de usuario, apunta a su NODE_FUNC_DEF.
     * Vive en el AST, así que no hay que liberarlo desde scope_free. Se
     * usa para mostrar el nombre en printAllVars() sin duplicarlo. */
    struct ASTNode *func_def;
} Scope;

Scope *scope_new(Scope *parent, const char *function_name);

/* Variante para scopes de función: guarda el puntero al NODE_FUNC_DEF
 * en lugar de duplicar el nombre. Evita un malloc por llamada. */
Scope *scope_new_func(Scope *parent, ASTNode *func_def);
VarEntry *scope_find(Scope *scope, const char *name);
VarEntry *scope_find_current(Scope *scope, const char *name); // solo en el ámbito actual
uint64_t scope_cache_generation(void);
void scope_remove_var_entry(Scope *scope, VarEntry *entry);
void scope_define(Scope *scope, const char *name, int vtype, Value val);

/* Igual que scope_assign() pero recibe directamente el VarEntry ya
 * localizado. Evita repetir el scope_find() cuando el llamador ya
 * tiene el puntero. */
void scope_assign_direct(VarEntry *e, Value val, int line);

PortalEntry *portal_find(Scope *scope, const char *name);
PortalEntry *portal_find_in_scope(Scope *scope, const char *name);
void portal_define(Scope *scope, const char *name, int line);

void scope_free(Scope *s);

/* Vacía un scope (libera variables y portales) pero conserva la
 * estructura: identity, buckets hash y function_name se mantienen.
 * Se usa para reutilizar el mismo Scope en cada iteración de un
 * bucle, en lugar de scope_new() + scope_free() por iteración. */
void scope_reset(Scope *s);
void scope_free_all(void);

#endif
