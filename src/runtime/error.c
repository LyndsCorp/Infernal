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

static void __attribute__((noreturn)) error_build(int line, int column, const char *fmt, va_list ap) {
    char base[1024];
    vsnprintf(base, sizeof(base), fmt, ap);

    const char *file = current_source_file ? current_source_file : "<entrada>";
    const char *source = NULL;
    if (line > 0 && line <= source_line_count && source_lines) {
        source = source_lines[line - 1];
    }

    if (source && column > 0) {
        char marker[256];
        size_t source_pos = 0;
        size_t marker_len = 0;
        int target = column - 1;

        /* Las columnas del lexer son offsets de bytes. Para que un tabulador
         * no desplace el '^' visualmente, reproducimos los tabs del prefijo
         * en el marcador en lugar de convertirlos en espacios. */
        while (source[source_pos] && source_pos < (size_t)target && marker_len < sizeof(marker) - 2) {
            marker[marker_len++] = source[source_pos] == '\t' ? '\t' : ' ';
            source_pos++;
        }
        marker[marker_len++] = '^';
        marker[marker_len] = '\0';
        snprintf(exception_msg, sizeof(exception_msg),
                 "Error en '%-.240s', línea %d, columna %d:\n    %-.240s\n    %s\n    %-.900s",
                 file, line, column, source, marker, base);
    } else if (source) {
        snprintf(exception_msg, sizeof(exception_msg),
                 "Error en '%-.240s', línea %d:\n    %-.240s\n    %-.900s",
                 file, line, source, base);
    } else {
        snprintf(exception_msg, sizeof(exception_msg),
                 "Error en '%-.240s', línea %d: %-.900s",
                 file, line, base);
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
