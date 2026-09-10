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

static void error_build(int line, int column, const char *fmt, va_list ap) {
    char base[384];
    vsnprintf(base, sizeof(base), fmt, ap);

    const char *file = current_source_file ? current_source_file : "<entrada>";
    const char *source = NULL;
    if (line > 0 && line <= source_line_count && source_lines) {
        source = source_lines[line - 1];
    }

    if (source && column > 0) {
        char marker[96];
        int spaces = column - 1;
        if (spaces > 80) spaces = 80;
        memset(marker, ' ', (size_t)spaces);
        marker[spaces] = '^';
        marker[spaces + 1] = '\0';
        snprintf(exception_msg, sizeof(exception_msg),
                 "Error en '%-.72s', línea %d, columna %d:\n    %-.150s\n    %s\n    %-.210s",
                 file, line, column, source, marker, base);
    } else if (source) {
        snprintf(exception_msg, sizeof(exception_msg),
                 "Error en '%-.72s', línea %d:\n    %-.180s\n    %-.220s",
                 file, line, source, base);
    } else {
        snprintf(exception_msg, sizeof(exception_msg),
                 "Error en '%-.72s', línea %d: %-.390s", file, line, base);
    }

    exception_raised = 1;
    longjmp(exception_env, 1);
}

void error(int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    error_build(line, 0, fmt, ap);
    va_end(ap);
}

void error_at(int line, int column, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    error_build(line, column, fmt, ap);
    va_end(ap);
}
