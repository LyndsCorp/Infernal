/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: stdlib/builtins.c
 */

// AQUI SE AÑADEN LOS .h DE LOS STDLIB
#include "builtins.h"
#include "core/value.h"
#include "cut.h"       //funciones de cortar strings
#include "database.h"  //funciones de persistencia
#include "io.h"        //funciones de entrada con salida
#include "list.h"      //funciones de listas
#include "map.h"       //funciones de maps
#include "neutral.h"   //funciones polimorficas
#include "output.h"    //funciones de salida
#include "string.h"    //funciones de strings
#include "system.h"    //funciones del sistema
#include "variables.h" //funciones de variables

// AQUI SE AÑADEN LOS STDLIB
void register_all_builtins(void) {
    register_output_builtins();
    register_io_builtins();
    register_system_builtins();
    register_string_builtins();
    register_map_builtins();
    register_neutral_builtins();
    register_database_builtins();
    register_list_builtins();
    register_cut_builtins();
    register_variables_builtins();
}
