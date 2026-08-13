/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: core/memory.c
*/

#include "memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void allocation_failed(size_t size) {
    fprintf(stderr, "Infernal: no se pudo reservar memoria (%zu bytes)\n", size);
    exit(EXIT_FAILURE);
}

void *infernal_malloc(size_t size) {
    void *ptr = malloc(size ? size : 1);
    if (!ptr) allocation_failed(size);
    return ptr;
}

void *infernal_calloc(size_t count, size_t size) {
    void *ptr = calloc(count ? count : 1, size ? size : 1);
    if (!ptr) allocation_failed(count * size);
    return ptr;
}

void *infernal_realloc(void *ptr, size_t size) {
    void *next = realloc(ptr, size ? size : 1);
    if (!next) allocation_failed(size);
    return next;
}

char *infernal_strdup(const char *value) {
    const char *source = value ? value : "";
    size_t size = strlen(source) + 1;
    char *copy = infernal_malloc(size);
    memcpy(copy, source, size);
    return copy;
}
