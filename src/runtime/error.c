/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/error.c
*/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "error.h"
#include "runtime/globals.h"

void error(int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    // Primero formateamos el mensaje base
    char base[384];
    vsnprintf(base, sizeof(base), fmt, ap);
    va_end(ap);

    // Ahora construimos el mensaje final con archivo (si existe) y línea
    if (current_source_file) {
        snprintf(exception_msg, sizeof(exception_msg),
                 "Error en '%.96s', línea %d: %s", current_source_file, line, base);
    } else {
        snprintf(exception_msg, sizeof(exception_msg),
                 "Error, línea %d: %s", line, base);
    }

    exception_raised = 1;
    longjmp(exception_env, 1);
}
