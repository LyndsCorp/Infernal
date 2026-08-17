/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: core/value.h
*/

#ifndef CORE_VALUE_H
#define CORE_VALUE_H

#include "types.h"

/* --- Definición de MapPair y MapData ---------------------- */
typedef struct MapPair {
    char *key;
    Value value;
} MapPair;

typedef struct MapData {
    MapPair *pairs;   // ahora es un puntero a MapPair
    int count, cap;
} MapData;

/* --- Funciones básicas de Value ---------------------------- */
Value val_make_null(void);
Value val_int(int x);
Value val_float(double x);
Value val_bool(bool x);
Value val_string(const char *s);
Value val_list_empty(void);
void  val_list_append(Value *list, Value item);
Value val_list_copy(Value *src);
int   valtype_to_tokentype(int vtype);
Value val_reference(const char *list_name, int index);
Value val_ptr(void *ptr);
Value copy_value_secure(Value src);
void  value_free(Value *value);

/* --- Funciones para mapas ---------------------------------- */
Value val_map_empty(void);
void  val_map_set(Value *map, const char *key, Value value);
Value val_map_get(Value map, const char *key);
int   val_map_has(Value map, const char *key);
void  val_map_delete(Value *map, const char *key);
Value val_map_copy(Value *src);

#endif
