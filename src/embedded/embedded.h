/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, Lynds Corp., David Baña Szymaniak
 * Licencia GPL v3 o posterior
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
