/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/error.h
*/

#ifndef RUNTIME_ERROR_H
#define RUNTIME_ERROR_H

#include <setjmp.h>

void error(int line, const char *fmt, ...);

/* Helper para cuando falta 'then' en estructuras de control */
#define error_missing_then(line, context) \
    error(line, "Se esperaba 'then' después de la condición del %s. " \
                "Los operadores de comparación válidos son ==, !=, <=, >= . " \
                "Asegúrate de no usar '<' o '>' sueltos.", context)

#endif
