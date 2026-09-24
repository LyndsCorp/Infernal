/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Apache 2.0 — Código fuente de Infernal: runtime/lava.h
*/

#ifndef RUNTIME_LAVA_H
#define RUNTIME_LAVA_H

#include <stddef.h>

int lava_try_import(const char *name, const char *prefix, char *tried, size_t tried_size);

int lava_try_import_path(const char *path, const char *prefix, char *tried, size_t tried_size);

void lava_cleanup(void);

#endif /* RUNTIME_LAVA_H */
