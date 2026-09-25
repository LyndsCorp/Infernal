/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: embedded/embedded.h
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

//para los Lava
typedef struct {
    const char *name;
    const unsigned char *data;
    unsigned int *size_ptr;
} EmbeddedLavaModule;

extern EmbeddedLavaModule embedded_lava_modules[];

/* Busca un módulo Lava embebido por nombre (sin extensión).
 * Devuelve 1 si lo encuentra, 0 en caso contrario. */
int embedded_lava_find(const char *name, const unsigned char **data, size_t *size);

#endif
