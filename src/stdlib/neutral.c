/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, Lynds Corp., David Baña Szymaniak
 * Licencia GPL v3 o posterior
 * Código fuente de Infernal: stdlib/neutral.c
 *
 * Funciones polimórficas que trabajan con varios tipos (string, lista, mapa).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "neutral.h"
#include "core/value.h"
#include "runtime/error.h"
#include "runtime/globals.h"
#include "vm/vm.h"


/* ================================================
 *  Funciones de la biblioteca
 * ================================================ */

/* --- has() --- */
static Value builtin_has(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "has() espera exactamente 2 argumentos");

    Value container = args[0];
    Value element   = args[1];

    /* Caso: string */
    if (container.type == VAL_STRING) {
        if (element.type != VAL_STRING)
            error(current_eval_line, "has() para string requiere segundo argumento string");
        const char *s = container.data.sval;
        const char *sub = element.data.sval;
        return val_bool(strstr(s, sub) != NULL);
    }

    /* Caso: lista */
    if (container.type == VAL_LIST) {
        for (int i = 0; i < container.data.list.count; i++) {
            Value item = container.data.list.items[i];
            if (item.type == element.type) {
                switch (item.type) {
                    case VAL_INT:    if (item.data.ival == element.data.ival) return val_bool(true); break;
                    case VAL_FLOAT:  if (item.data.fval == element.data.fval) return val_bool(true); break;
                    case VAL_BOOL:   if (item.data.bval == element.data.bval) return val_bool(true); break;
                    case VAL_STRING: if (strcmp(item.data.sval, element.data.sval) == 0) return val_bool(true); break;
                    default: /* listas y mapas no se comparan por simplicidad */ break;
                }
            }
        }
        return val_bool(false);
    }

    /* Caso: mapa */
    if (container.type == VAL_MAP) {
        if (element.type != VAL_STRING)
            error(current_eval_line, "has() para mapa requiere clave string");
        MapData *md = container.data.map;
        const char *key = element.data.sval;
        for (int i = 0; i < md->count; i++) {
            if (strcmp(md->pairs[i].key, key) == 0)
                return val_bool(true);
        }
        return val_bool(false);
    }

    error(current_eval_line, "has() espera string, lista o mapa como primer argumento");
    return val_make_null();
}

/* --- size() --- */
static Value builtin_size(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "size() espera exactamente 1 argumento");

    Value v = args[0];
    if (v.type == VAL_MAP) {
        MapData *md = v.data.map;
        return val_int(md->count);
    } else if (v.type == VAL_LIST) {
        return val_int(v.data.list.count);
    } else {
        error(current_eval_line, "size() espera un mapa o una lista");
    }
    return val_make_null();
}


/* ================================================
 *  Registro de funciones
 * ================================================ */

void register_neutral_builtins(void) {
    func_register_builtin("has", builtin_has);
    func_register_builtin("size", builtin_size);

    vm_register_builtin("has", builtin_has);
    vm_register_builtin("size", builtin_size);
}
