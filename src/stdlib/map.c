/*
 * Infernal: el lenguaje de programación.
 * Copyright (C) 2026, Lynds Corp., David Baña Szymaniak, GPL v3+ License.
 * Código fuente de Infernal: stdlib/map.c
 */

#include <stdlib.h>
#include <string.h>
#include "map.h"
#include "core/value.h"
#include "runtime/error.h"
#include "runtime/globals.h"
#include "vm/vm.h"

/* ─── keys() ────────────────────────────────────────────────── */
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

/* ─── values() ──────────────────────────────────────────────── */
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

/* ─── has() ──────────────────────────────────────────────────── */
static Value builtin_has(int argc, Value *args) {
    if (argc != 2) error(0, "has() espera exactamente 2 argumentos");
    if (args[0].type != VAL_MAP) error(0, "has() espera un mapa como primer argumento");
    if (args[1].type != VAL_STRING) error(0, "has() espera una cadena como segundo argumento");

    MapData *md = args[0].data.map;
    const char *key = args[1].data.sval;
    for (int i = 0; i < md->count; i++) {
        if (strcmp(md->pairs[i].key, key) == 0)
            return val_bool(true);
    }
    return val_bool(false);
}

/* ─── delete() ──────────────────────────────────────────────── */
static Value builtin_delete(int argc, Value *args) {
    if (argc != 2) error(0, "delete() espera exactamente 2 argumentos");
    if (args[0].type != VAL_MAP) error(0, "delete() espera un mapa como primer argumento");
    if (args[1].type != VAL_STRING) error(0, "delete() espera una cadena como segundo argumento");

    MapData *md = args[0].data.map;
    const char *key = args[1].data.sval;
    for (int i = 0; i < md->count; i++) {
        if (strcmp(md->pairs[i].key, key) == 0) {
            free(md->pairs[i].key);
            // Liberar el valor si es string (o lista/mapa si se desea, pero por simplicidad solo string)
            if (md->pairs[i].value.type == VAL_STRING)
                free(md->pairs[i].value.data.sval);
            // Desplazar los siguientes elementos
            for (int j = i; j < md->count - 1; j++) {
                md->pairs[j] = md->pairs[j + 1];
            }
            md->count--;
            return val_make_null();
        }
    }
    return val_make_null();  // clave no encontrada, no hace nada
}

/* ─── size() ── MODIFICADO PARA ACEPTAR LISTAS Y MAPAS ──────── */
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

/* ─── Registro ──────────────────────────────────────────────── */
void register_map_builtins(void) {
    func_register_builtin("keys", builtin_keys);
    func_register_builtin("values", builtin_values);
    func_register_builtin("has", builtin_has);
    func_register_builtin("delete", builtin_delete);
    func_register_builtin("size", builtin_size);

    vm_register_builtin("keys", builtin_keys);
    vm_register_builtin("values", builtin_values);
    vm_register_builtin("has", builtin_has);
    vm_register_builtin("delete", builtin_delete);
    vm_register_builtin("size", builtin_size);
}
