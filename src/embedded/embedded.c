/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: stdlib/system.h
*/

#include "embedded.h"
#include <string.h>

int embedded_find(const char *name, const unsigned char **data, size_t *size, int *compressed) {
    for (int i = 0; embedded_modules[i].name != NULL; i++) {
        if (strcmp(embedded_modules[i].name, name) == 0) {
            if (data) *data = embedded_modules[i].data;
            if (size) *size = *embedded_modules[i].size_ptr;
            if (compressed) *compressed = embedded_modules[i].compressed;
            return 1;
        }
    }
    return 0;
}
