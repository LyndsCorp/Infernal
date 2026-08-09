/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License.
 * Código fuente de Infernal: stdlib/builtins.c
*/

//AQUI SE AÑADEN LOS .h DE LOS STDLIB
#include "builtins.h"
#include "core/value.h"
#include "output.h"
#include "io.h"
#include "system.h"
#include "string.h"
#include "system.h"
#include "map.h"

//AQUI SE AÑADEN LOS STDLIB
void register_all_builtins(void) {
    register_output_builtins();
    register_io_builtins();
    register_system_builtins();
    register_string_builtins();
    register_map_builtins();
}
