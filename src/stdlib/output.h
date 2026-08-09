/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License, Lynds Corp., Aros Legendarios, David Baña Szymaniak.
 * Código fuente de Infernal: stdlib/output.h
*/

#ifndef STDLIB_OUTPUT_H
#define STDLIB_OUTPUT_H

#include "core/value.h"   /* para Value */

// Función global de impresión de valores
void print_value(Value v);

// Registro de builtins de salida
void register_output_builtins(void);

#endif
