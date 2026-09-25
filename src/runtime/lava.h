/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/lava.h
*/

#ifndef RUNTIME_LAVA_H
#define RUNTIME_LAVA_H

#include <stddef.h>
#include "core/value.h"

int lava_try_import(const char *name, const char *prefix, char *tried, size_t tried_size);
int lava_try_import_path(const char *path, const char *prefix, char *tried, size_t tried_size);
void lava_cleanup(void);

//Constantes mutables desde Lava.
Value *make_infernal_const(const char *name);
Value *get_infernal_const(const char *name);
const char *value_type_name(int type);

#endif /* RUNTIME_LAVA_H */
