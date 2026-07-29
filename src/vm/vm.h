/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License, Lynds Corp., Aros Legendarios, David Baña Szymaniak.
 * Código fuente de Infernal: vm/vm.h
*/

#ifndef VM_VM_H
#define VM_VM_H

#include "bytecode.h"

Value vm_run(Chunk *chunk);

#define MAX_GLOBALS 256
extern Value vm_globals[MAX_GLOBALS];
extern int vm_global_count;
extern char *vm_global_names[MAX_GLOBALS];

typedef Value (*VmBuiltin)(int argc, Value *args);
extern VmBuiltin vm_builtins[256];
extern int vm_builtin_count;

int vm_register_global(const char *name, Value val);
int vm_register_builtin(const char *name, VmBuiltin func);
int vm_find_global_index(const char *name);
int vm_find_builtin_index(const char *name);

// Nuevas para funciones de usuario
int vm_register_user_function(const char *name, Chunk *code);
Chunk *vm_get_user_function(int index);

#endif
