/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: vm/vm.h
*/

#ifndef VM_VM_H
#define VM_VM_H

#include "bytecode.h"

Value vm_run(Chunk *chunk);
void vm_cleanup_state(void);

#define MAX_GLOBALS 256
extern Value vm_globals[MAX_GLOBALS];
extern int vm_global_types[MAX_GLOBALS];   // <-- NUEVO: tipos de globales
extern int vm_global_count;
extern char *vm_global_names[MAX_GLOBALS];

/* --- Ámbitos de variables globales -------------------------- */
#define GLOBAL_SCRIPT 0   // variable global del script actual
#define GLOBAL_SUPER  1   // variable global compartida entre scripts

typedef Value (*VmBuiltin)(int argc, Value *args);
extern VmBuiltin vm_builtins[256];
extern int vm_builtin_count;

/* Registrar una variable global con su ámbito y tipo (vtype: TOK_INT, etc.; 0 si sin tipo) */
int vm_register_global(const char *name, int scope_type, int vtype);

/* Registrar funciones nativas (builtins) */
int vm_register_builtin(const char *name, VmBuiltin func);

/* Buscar índice de una global por nombre */
int vm_find_global_index(const char *name);

/* Buscar índice de un builtin por nombre */
int vm_find_builtin_index(const char *name);

/* Registrar funciones de usuario compiladas */
int vm_register_user_function(const char *name, Chunk *code);
Chunk *vm_get_user_function(int index);

#endif
