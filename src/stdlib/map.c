/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: stdlib/map.c
*/

#include <stdlib.h>
#include <string.h>
#include "map.h"
#include "core/value.h"
#include "runtime/error.h"
#include "runtime/globals.h"
#include "vm/vm.h"


/* ================================================
 *  Funciones de la biblioteca
 * ================================================ */

/* --- keys() --- */
static Value builtin_keys(int argc, Value *args) {
    if (argc != 1) error(0, "keys() espera exactamente 1 argumento");
    if (args[0].type != VAL_MAP) error(0, "keys() espera un mapa");

    Value list = val_list_empty();
    MapData *md = args[0].data.map;
    for (int i = 0; i < md->count; i++) {
        val_list_append(&list, val_string(md->pairs[i].key));
    }
    return list;
}

/* --- values() --- */
static Value builtin_values(int argc, Value *args) {
    if (argc != 1) error(0, "values() espera exactamente 1 argumento");
    if (args[0].type != VAL_MAP) error(0, "values() espera un mapa");

    Value list = val_list_empty();
    MapData *md = args[0].data.map;
    for (int i = 0; i < md->count; i++) {
        val_list_append(&list, copy_value_secure(md->pairs[i].value));
    }
    return list;
}

/* --- delete() --- */
static Value builtin_delete(int argc, Value *args) {
    if (argc != 2) error(0, "delete() espera exactamente 2 argumentos");
    if (args[0].type != VAL_MAP) error(0, "delete() espera un mapa como primer argumento");
    if (args[1].type != VAL_STRING) error(0, "delete() espera una cadena como segundo argumento");

    val_map_delete(&args[0], args[1].data.sval);
    return args[0];
}

/* --- size() --- */
static Value builtin_size(int argc, Value *args) {
    if (argc != 1) error(0, "size() espera exactamente 1 argumento");

    Value v = args[0];
    if (v.type == VAL_MAP) {
        MapData *md = v.data.map;
        return val_int(md->count);
    } else if (v.type == VAL_LIST) {
        return val_int(v.data.list.count);
    } else {
        error(0, "size() espera un mapa o una lista");
    }
    return val_make_null();
}


/* ================================================
 *  Registro de funciones
 * ================================================ */

void register_map_builtins(void) {
    func_register_builtin("keys", builtin_keys);
    func_register_builtin("values", builtin_values);
    func_register_builtin("delete", builtin_delete);
    func_register_builtin("size", builtin_size);

    vm_register_builtin("keys", builtin_keys);
    vm_register_builtin("values", builtin_values);
    vm_register_builtin("delete", builtin_delete);
    vm_register_builtin("size", builtin_size);
}
