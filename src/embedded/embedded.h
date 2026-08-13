/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: stdlib/system.h
*/

#ifndef STDLIB_EMBEDDED_H
#define STDLIB_EMBEDDED_H

#include <stddef.h>

typedef struct {
    const char *name;
    const unsigned char *data;
    unsigned int *size_ptr;
    int compressed;
} EmbeddedModule;

extern EmbeddedModule embedded_modules[];

int embedded_find(const char *name, const unsigned char **data, size_t *size, int *compressed);

#endif
