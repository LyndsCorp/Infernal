/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: core/value.c
 */

#include "value.h"
#include "memory.h"
#include "runtime/error.h"
#include "runtime/evaluator/helpers.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* WideInteger está en types.h para que value.h pueda acceder a
 * box->number desde value_integer_is_zero (inline). */

Value val_make_null(void) {
    Value v = {0};
    v.type = VAL_NULL;
    return v;
}

Value val_void(void) {
    Value v = {0};
    v.type = VAL_VOID;
    return v;
}

Value val_int(int x) {
    Value v = {0};
    v.type = VAL_INT;
    v.data.ival = x;
    return v;
}

Value val_uint(uint32_t x) {
    Value v = {0};
    v.type = VAL_UINT;
    v.data.uval = x;
    return v;
}

InfernalBigInt value_as_bigint(Value value) {
    if (VALUE_HAS_WIDE_INTEGER(value))
        return value.data.wide_integer.box->number;
    return value.type == VAL_UINT ? bigint_from_u64(value.data.uval)
                                  : bigint_from_i64(value.data.ival);
}

static int64_t small_integer(Value value) {
    return value.type == VAL_UINT ? (int64_t)value.data.uval : (int64_t)value.data.ival;
}

/* value_integer_is_zero() está en value.h como static inline. */

int value_integer_compare(Value left, Value right) {
    bool left_wide = VALUE_HAS_WIDE_INTEGER(left);
    bool right_wide = VALUE_HAS_WIDE_INTEGER(right);
    if (!left_wide && !right_wide) {
        int64_t a = small_integer(left), b = small_integer(right);
        return (a > b) - (a < b);
    }
    if (left_wide && right_wide)
        return bigint_compare_ref(&left.data.wide_integer.box->number,
                                  &right.data.wide_integer.box->number);
    if (left_wide) {
        InfernalBigInt small = right.type == VAL_UINT ? bigint_from_u64(right.data.uval)
                                                      : bigint_from_i64(right.data.ival);
        return bigint_compare_ref(&left.data.wide_integer.box->number, &small);
    }
    InfernalBigInt small =
        left.type == VAL_UINT ? bigint_from_u64(left.data.uval) : bigint_from_i64(left.data.ival);
    return -bigint_compare_ref(&right.data.wide_integer.box->number, &small);
}

double value_integer_to_double(Value value) {
    if (VALUE_HAS_WIDE_INTEGER(value))
        return bigint_to_double_ref(&value.data.wide_integer.box->number);
    return value.type == VAL_UINT ? (double)value.data.uval : (double)value.data.ival;
}

bool value_integer_fits(Value value, size_t width, bool is_signed) {
    return bigint_fits_width(value_as_bigint(value), width, is_signed);
}

bool value_integer_to_i64(Value value, int64_t *out) {
    return (value.type == VAL_INT || value.type == VAL_UINT) &&
           bigint_to_i64(value_as_bigint(value), out);
}

Value val_bigint(InfernalBigInt x, bool is_unsigned) {
    size_t width = bigint_min_width(x, !is_unsigned);
    if (!width)
        error(0, "Entero fuera del rango máximo de 64 bytes");
    Value v = {0};
    if (width == 4) {
        uint64_t magnitude = 0;
        (void)bigint_to_u64(x, &magnitude);
        if (is_unsigned)
            return val_uint((uint32_t)magnitude);
        int64_t signed_value = 0;
        (void)bigint_to_i64(x, &signed_value);
        return val_int((int)signed_value);
    }
    v.type = is_unsigned ? VAL_UINT : VAL_INT;
    v.data.wide_integer.box = infernal_malloc(sizeof(*v.data.wide_integer.box));
    v.data.wide_integer.box->references = 1;
    v.data.wide_integer.box->number = x;
    v.flags |= VALUE_FLAG_WIDE_INTEGER;
    return v;
}

Value val_float(double x) {
    Value v = {0};
    v.type = VAL_FLOAT;
    v.data.fval = x;
    return v;
}

Value val_bool(bool x) {
    Value v = {0};
    v.type = VAL_BOOL;
    v.data.bval = x;
    return v;
}

Value val_string(const char *s) {
    Value v = {0};
    v.type = VAL_STRING;
    v.data.sval = infernal_strdup(s ? s : "");
    return v;
}

Value val_list_empty(void) {
    Value v = {0};
    v.type = VAL_LIST;
    v.data.list.items = NULL;
    v.data.list.count = v.data.list.cap = 0;
    return v;
}

void val_list_append(Value *list, Value item) {
    if (list->type != VAL_LIST)
        error(0, "val_list_append: el valor no es una lista");
    if (list->data.list.count < 0 || list->data.list.cap < 0 ||
        list->data.list.count > list->data.list.cap)
        error(0, "val_list_append: estado interno de lista inválido");
    if (list->data.list.count >= list->data.list.cap) {
        if (list->data.list.cap > INT_MAX / 2)
            error(0, "La lista ha alcanzado el límite máximo de elementos");
        int new_cap = list->data.list.cap == 0 ? 4 : list->data.list.cap * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(Value))
            error(0, "La lista es demasiado grande para reservar memoria");
        list->data.list.cap = new_cap;
        list->data.list.items =
            infernal_realloc(list->data.list.items, (size_t)new_cap * sizeof(Value));
    }
    list->data.list.items[list->data.list.count++] = item;
}

Value copy_value_secure(Value src) {
    if ((src.type == VAL_INT || src.type == VAL_UINT) && VALUE_HAS_WIDE_INTEGER(src)) {
        Value copy = src;
        copy.data.wide_integer.box->references++;
        return copy;
    }
    if (src.type == VAL_STRING) {
        return val_string(src.data.sval);
    } else if (src.type == VAL_LIST) {
        int n = src.data.list.count;
        if (n == 0)
            return val_list_empty();
        Value new_list = val_list_empty();
        new_list.data.list.items = infernal_malloc((size_t)n * sizeof(Value));
        new_list.data.list.cap = n;
        for (int i = 0; i < n; i++) {
            new_list.data.list.items[i] = copy_value_secure(src.data.list.items[i]);
        }
        new_list.data.list.count = n;
        return new_list;
    } else if (src.type == VAL_MAP) {
        return val_map_copy(&src);
    } else if (src.type == VAL_REFERENCE) {
        Value v = {0};
        v.type = VAL_REFERENCE;
        v.data.ref.container_name = infernal_strdup(src.data.ref.container_name);
        v.data.ref.map_key = src.data.ref.map_key ? infernal_strdup(src.data.ref.map_key) : NULL;
        v.data.ref.index = src.data.ref.index;
        v.data.ref.is_map = src.data.ref.is_map;
        return v;
    } else {
        return src;
    }
}

Value val_list_copy(Value *src) { return copy_value_secure(*src); }

/* --- CORREGIDO: incluye VAL_MAP --- */
int valtype_to_tokentype(int vtype) {
    switch (vtype) {
    case VAL_INT:
        return TOK_INT;
    case VAL_UINT:
        return TOK_UINT;
    case VAL_FLOAT:
        return TOK_FLOAT;
    case VAL_BOOL:
        return TOK_BOOL;
    case VAL_STRING:
        return TOK_STRING;
    case VAL_LIST:
        return TOK_LIST;
    case VAL_MAP:
        return TOK_MAP;
    default:
        return 0;
    }
}

Value val_reference(const char *list_name, int index) {
    Value v = {0};
    v.type = VAL_REFERENCE;
    v.data.ref.container_name = infernal_strdup(list_name);
    v.data.ref.map_key = NULL;
    v.data.ref.index = index;
    v.data.ref.is_map = false;
    return v;
}

Value val_map_reference(const char *map_name, const char *key) {
    Value v = {0};
    v.type = VAL_REFERENCE;
    v.data.ref.container_name = infernal_strdup(map_name);
    v.data.ref.map_key = infernal_strdup(key);
    v.data.ref.index = 0;
    v.data.ref.is_map = true;
    return v;
}

Value val_ptr(void *ptr) {
    Value v = {0};
    v.type = VAL_PTR;
    v.data.ptr = ptr;
    return v;
}

/* --- Implementación de mapas ------------------------------ */

static const char *map_type_name(int tok_type) {
    switch (tok_type) {
    case TOK_INT:
        return "int";
    case TOK_UINT:
        return "uint";
    case TOK_FLOAT:
        return "float";
    case TOK_BOOL:
        return "bool";
    case TOK_STRING:
        return "string";
    case TOK_LIST:
        return "list";
    case TOK_MAP:
        return "map";
    default:
        return "desconocido";
    }
}

Value val_map_typed(int value_type) {
    (void)value_type;
    Value v = {0};
    v.type = VAL_MAP;
    MapData *md = infernal_malloc(sizeof(MapData));
    md->pairs = NULL;
    md->count = md->cap = 0;
    v.data.map = md;
    return v;
}

Value val_map_empty(void) { return val_map_typed(0); }

static void validate_map_value_type(const char *key, Value value, int expected) {
    if (expected == 0)
        return;
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
    int existing_type = 0;
    for (int i = 0; i < md->count; i++) {
        if (strcmp(md->pairs[i].key, key) == 0) {
            existing_type = md->pairs[i].value_type;
            break;
        }
    }
    int requested_type = existing_type ? existing_type : value_type;
    if (existing_type && value_type && existing_type != value_type)
        requested_type = 0;
    Value normalized = val_make_null();
    bool owns_normalized = false;
    if (requested_type == TOK_UINT && value.type != VAL_UINT) {
        normalized = copy_value_secure(value);
        if (!try_convert_value(&normalized, TOK_UINT)) {
            value_free(&normalized);
            error(0, "El valor de la clave '%s' no cabe en uint", key);
        }
        value = normalized;
        owns_normalized = true;
    }
    for (int i = 0; i < md->count; i++) {
        if (strcmp(md->pairs[i].key, key) == 0) {
            int expected = md->pairs[i].value_type;
            if (value_type != 0) {
                if (expected != 0 && expected != value_type) {
                    error(0, "La clave '%s' ya tiene el tipo %s y no puede declararse como %s", key,
                          map_type_name(expected), map_type_name(value_type));
                }
                expected = value_type;
            }
            if (expected == 0)
                expected = valtype_to_tokentype(value.type);
            validate_map_value_type(key, value, expected);
            value_free(&md->pairs[i].value);
            md->pairs[i].value = copy_value_secure(value);
            md->pairs[i].value_type = expected;
            if (owns_normalized)
                value_free(&normalized);
            return;
        }
    }
    int expected = value_type != 0 ? value_type : valtype_to_tokentype(value.type);
    validate_map_value_type(key, value, expected);
    if (md->count < 0 || md->cap < 0 || md->count > md->cap)
        error(0, "val_map_set_typed: estado interno del mapa inválido");
    if (md->count >= md->cap) {
        if (md->cap > INT_MAX / 2)
            error(0, "El mapa ha alcanzado el límite máximo de elementos");
        int new_cap = md->cap == 0 ? 4 : md->cap * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(MapPair))
            error(0, "El mapa es demasiado grande para reservar memoria");
        md->cap = new_cap;
        md->pairs = infernal_realloc(md->pairs, (size_t)new_cap * sizeof(MapPair));
    }
    md->pairs[md->count].key = infernal_strdup(key);
    md->pairs[md->count].value = copy_value_secure(value);
    md->pairs[md->count].value_type = expected;
    md->count++;
    if (owns_normalized)
        value_free(&normalized);
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
    if (map.type != VAL_MAP)
        return 0;
    MapData *md = map.data.map;
    for (int i = 0; i < md->count; i++) {
        if (strcmp(md->pairs[i].key, key) == 0)
            return 1;
    }
    return 0;
}

void val_map_delete(Value *map, const char *key) {
    if (map->type != VAL_MAP)
        return;
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
    if (src->type != VAL_MAP)
        return val_make_null();
    Value dst = val_map_empty();
    MapData *md_src = src->data.map;
    int n = md_src->count;
    if (n == 0)
        return dst;
    /* Pre-dimensionamos al tamaño exacto: val_map_set_typed
     * crecería por duplicación. */
    dst.data.map->pairs = infernal_malloc((size_t)n * sizeof(MapPair));
    dst.data.map->cap = n;
    for (int i = 0; i < n; i++) {
        dst.data.map->pairs[i].key = infernal_strdup(md_src->pairs[i].key);
        dst.data.map->pairs[i].value = copy_value_secure(md_src->pairs[i].value);
        dst.data.map->pairs[i].value_type = md_src->pairs[i].value_type;
    }
    dst.data.map->count = n;
    return dst;
}

void value_free(Value *value) {
    if (!value)
        return;
    switch (value->type) {
    case VAL_INT:
    case VAL_UINT:
        if (VALUE_HAS_WIDE_INTEGER(*value) && --value->data.wide_integer.box->references == 0)
            free(value->data.wide_integer.box);
        break;
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
        free(value->data.ref.container_name);
        free(value->data.ref.map_key);
        break;
    default:
        break;
    }
    *value = val_make_null();
}
