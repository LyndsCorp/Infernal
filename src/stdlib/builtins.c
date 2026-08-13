/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License.
 * Código fuente de Infernal: stdlib/builtins.c
*/

//AQUI SE AÑADEN LOS .h DE LOS STDLIB
#include "builtins.h"
#include "core/value.h"
#include "output.h" //funciones de salida
#include "io.h" //funciones de entrada con salida
#include "system.h" //funciones del sistema
#include "string.h" //funciones de strings
#include "map.h" //funciones de maps
#include "bytes.h" //funciones de bytes crudos
#include "neutral.h" //funciones polimorficas

//AQUI SE AÑADEN LOS STDLIB
void register_all_builtins(void) {
    register_output_builtins();
    register_io_builtins();
    register_system_builtins();
    register_string_builtins();
    register_map_builtins();
    register_bytes_builtins();
    register_neutral_builtins();
}
