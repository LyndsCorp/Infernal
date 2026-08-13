/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/scope.h
*/

#ifndef RUNTIME_SCOPE_H
#define RUNTIME_SCOPE_H

#include "core/types.h"

typedef struct VarEntry {
    char *name;
    int vtype;
    Value value;
    struct VarEntry *next;
} VarEntry;

typedef struct PortalEntry {
    char *name;
    int line;
    struct PortalEntry *next;
} PortalEntry;

typedef struct Scope {
    VarEntry *vars;
    PortalEntry *portals;
    struct Scope *parent;
    char *function_name;  // nombre de la función si es un ámbito de función, o NULL
} Scope;

Scope *scope_new(Scope *parent, const char *function_name);
VarEntry *scope_find(Scope *scope, const char *name);
VarEntry *scope_find_current(Scope *scope, const char *name); // solo en el ámbito actual
VarEntry *scope_find_script(Scope *scope, const char *name);
void scope_define(Scope *scope, const char *name, int vtype, Value val);
void scope_assign(Scope *scope, const char *name, Value val, int line);

PortalEntry *portal_find(Scope *scope, const char *name);
PortalEntry *portal_find_in_scope(Scope *scope, const char *name);
void portal_define(Scope *scope, const char *name, int line);

void scope_free(Scope *s);

#endif
