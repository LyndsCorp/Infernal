/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License.
 * Código fuente de Infernal: core/value.h
*/

#ifndef CORE_VALUE_H
#define CORE_VALUE_H

#include "types.h"

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

/* ─── Copia profunda y liberación ──────────────────────────── */
Value value_copy(Value src);
void  free_value(Value v);

#endif
