/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License.
 * Código fuente de Infernal: stdlib/builtins.c
 */

#include "builtins.h"
#include "output.h"
#include "io.h"
#include "system.h"
#include "string.h"

void register_all_builtins(void) {
    register_output_builtins();
    register_io_builtins();
    register_system_builtins();
    register_string_builtins();
