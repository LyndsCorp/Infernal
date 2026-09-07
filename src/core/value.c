/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: core/value.c
 */

#include <stdlib.h>
#include <string.h>
#include "value.h"
#include "memory.h"
#include "runtime/error.h"

Value val_make_null(void) {
    Value v; v.type = VAL_NULL; return v;
}

Value val_int(int x) {
    Value v; v.type = VAL_INT; v.data.ival = x; return v;
}

Value val_float(double x) {
    Value v; v.type = VAL_FLOAT; v.data.fval = x; return v;
}

Value val_bool(bool x) {
    Value v; v.type = VAL_BOOL; v.data.bval = x; return v;
}

Value val_string(const char *s) {
    Value v;
    v.type = VAL_STRING;
    v.data.sval = infernal_strdup(s ? s : "");
    return v;
}

Value val_list_empty(void) {
    Value v; v.type = VAL_LIST;
    v.data.list.items = NULL;
    v.data.list.count = v.data.list.cap = 0;
    return v;
}

void val_list_append(Value *list, Value item) {
    if (list->data.list.count >= list->data.list.cap) {
        list->data.list.cap = list->data.list.cap == 0 ? 4 : list->data.list.cap * 2;
        list->data.list.items = infernal_realloc(list->data.list.items,
                                                 list->data.list.cap * sizeof(Value));
    }
    list->data.list.items[list->data.list.count++] = item;
}

Value copy_value_secure(Value src) {
    if (src.type == VAL_STRING) {
        return val_string(src.data.sval);
    } else if (src.type == VAL_LIST) {
        Value new_list = val_list_empty();
        for (int i = 0; i < src.data.list.count; i++) {
            val_list_append(&new_list, copy_value_secure(src.data.list.items[i]));
        }
        return new_list;
    } else if (src.type == VAL_MAP) {
        return val_map_copy(&src);
    } else if (src.type == VAL_REFERENCE) {
        Value v;
        v.type = VAL_REFERENCE;
        v.data.ref.list_name = infernal_strdup(src.data.ref.list_name);
        v.data.ref.index = src.data.ref.index;
        return v;
    } else {
        return src;
    }
}

Value val_list_copy(Value *src) {
    return copy_value_secure(*src);
}

/* --- CORREGIDO: incluye VAL_MAP --- */
int valtype_to_tokentype(int vtype) {
    switch (vtype) {
        case VAL_INT:    return TOK_INT;
        case VAL_FLOAT:  return TOK_FLOAT;
        case VAL_BOOL:   return TOK_BOOL;
        case VAL_STRING: return TOK_STRING;
        case VAL_LIST:   return TOK_LIST;
        case VAL_MAP:    return TOK_MAP;
        default:         return 0;
    }
}

Value val_reference(const char *list_name, int index) {
    Value v;
    v.type = VAL_REFERENCE;
    v.data.ref.list_name = infernal_strdup(list_name);
    v.data.ref.index = index;
    return v;
}

Value val_ptr(void *ptr) {
    Value v;
    v.type = VAL_PTR;
    v.data.ptr = ptr;
    return v;
}

/* --- Implementación de mapas ------------------------------ */

static const char *map_type_name(int tok_type) {
    switch (tok_type) {
        case TOK_INT: return "int";
        case TOK_FLOAT: return "float";
        case TOK_BOOL: return "bool";
        case TOK_STRING: return "string";
        case TOK_LIST: return "list";
        case TOK_MAP: return "map";
        default: return "desconocido";
    }
}

Value val_map_typed(int value_type) {
    (void)value_type;
    Value v;
    v.type = VAL_MAP;
    MapData *md = infernal_malloc(sizeof(MapData));
    md->pairs = NULL;
    md->count = md->cap = 0;
    v.data.map = md;
    return v;
}

Value val_map_empty(void) {
    return val_map_typed(0);
}

static void validate_map_value_type(const char *key, Value value, int expected) {
    if (expected == 0) return;
    int actual = valtype_to_tokentype(value.type);
    if (actual != expected) {
        error(0, "Error de tipado del mapa: la clave '%s' requiere un valor %s pero se obtuvo %s",
              key, map_type_name(expected), map_type_name(actual));
    }
}

void val_map_set_typed(Value *map, const char *key, Value value, int value_type) {
    if (map->type != VAL_MAP) {
        error(0, "val_map_set_typed: el valor no es un mapa");
    }
    MapData *md = map->data.map;
    for (int i = 0; i < md->count; i++) {
        if (strcmp(md->pairs[i].key, key) == 0) {
            int expected = md->pairs[i].value_type;
            if (value_type != 0) {
                if (expected != 0 && expected != value_type) {
                    error(0, "La clave '%s' ya tiene el tipo %s y no puede declararse como %s",
                          key, map_type_name(expected), map_type_name(value_type));
                }
                expected = value_type;
            }
            if (expected == 0)
                expected = valtype_to_tokentype(value.type);
            validate_map_value_type(key, value, expected);
            value_free(&md->pairs[i].value);
            md->pairs[i].value = copy_value_secure(value);
            md->pairs[i].value_type = expected;
            return;
        }
    }
    int expected = value_type != 0 ? value_type : valtype_to_tokentype(value.type);
    validate_map_value_type(key, value, expected);
    if (md->count >= md->cap) {
        md->cap = md->cap == 0 ? 4 : md->cap * 2;
        md->pairs = infernal_realloc(md->pairs, md->cap * sizeof(MapPair));
    }
    md->pairs[md->count].key = infernal_strdup(key);
    md->pairs[md->count].value = copy_value_secure(value);
    md->pairs[md->count].value_type = expected;
    md->count++;
}

void val_map_set(Value *map, const char *key, Value value) {
    val_map_set_typed(map, key, value, 0);
}

Value val_map_get(Value map, const char *key) {
    if (map.type != VAL_MAP) {
        return val_make_null();
    }
    MapData *md = map.data.map;
    for (int i = 0; i < md->count; i++) {
        if (strcmp(md->pairs[i].key, key) == 0) {
            return copy_value_secure(md->pairs[i].value);
        }
    }
    return val_make_null();
}

int val_map_has(Value map, const char *key) {
    if (map.type != VAL_MAP) return 0;
    MapData *md = map.data.map;
    for (int i = 0; i < md->count; i++) {
        if (strcmp(md->pairs[i].key, key) == 0)
            return 1;
    }
    return 0;
}

void val_map_delete(Value *map, const char *key) {
    if (map->type != VAL_MAP) return;
    MapData *md = map->data.map;
    for (int i = 0; i < md->count; i++) {
        if (strcmp(md->pairs[i].key, key) == 0) {
            free(md->pairs[i].key);
            value_free(&md->pairs[i].value);
            for (int j = i; j < md->count - 1; j++) {
                md->pairs[j] = md->pairs[j + 1];
            }
            md->count--;
            return;
        }
    }
}

Value val_map_copy(Value *src) {
    if (src->type != VAL_MAP) return val_make_null();
    Value dst = val_map_empty();
    MapData *md_src = src->data.map;
    for (int i = 0; i < md_src->count; i++) {
        val_map_set_typed(&dst, md_src->pairs[i].key, md_src->pairs[i].value, md_src->pairs[i].value_type);
    }
    return dst;
}


void value_free(Value *value) {
    if (!value) return;
    switch (value->type) {
        case VAL_STRING:
            free(value->data.sval);
            break;
        case VAL_LIST:
            for (int i = 0; i < value->data.list.count; i++)
                value_free(&value->data.list.items[i]);
            free(value->data.list.items);
            break;
        case VAL_MAP:
            if (value->data.map) {
                for (int i = 0; i < value->data.map->count; i++) {
                    free(value->data.map->pairs[i].key);
                    value_free(&value->data.map->pairs[i].value);
                }
                free(value->data.map->pairs);
                free(value->data.map);
            }
            break;
        case VAL_REFERENCE:
            free(value->data.ref.list_name);
            break;
        default:
            break;
    }
    *value = val_make_null();
}
