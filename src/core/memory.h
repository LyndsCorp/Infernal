/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: core/memory.h
*/

#ifndef CORE_MEMORY_H
#define CORE_MEMORY_H

#include <stddef.h>

void *infernal_malloc(size_t size);
void *infernal_calloc(size_t count, size_t size);
void *infernal_realloc(void *ptr, size_t size);
char *infernal_strdup(const char *value);
#define infernal_free(ptr) free(ptr)

#endif
