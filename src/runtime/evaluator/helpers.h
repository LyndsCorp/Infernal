/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/helpers.h
*/

#ifndef RUNTIME_EVALUATOR_HELPERS_H
#define RUNTIME_EVALUATOR_HELPERS_H

#include "core/types.h"
#include "core/value.h"

/* Inline en el header: se llama desde las condiciones de while,
 * for, for-in, if y desde and/or. Tenerla en línea ahorra un
 * call/ret por iteración en bucles apretados. */
static inline bool val_is_truthy(Value v) {
    switch (v.type) {
        case VAL_BOOL:   return v.data.bval;
        case VAL_INT: case VAL_UINT: return !value_integer_is_zero(v);
        case VAL_FLOAT:  return v.data.fval != 0.0;
        case VAL_STRING: return v.data.sval && v.data.sval[0] != '\0';
        case VAL_LIST:   return v.data.list.count > 0;
        case VAL_MAP:    return v.data.map && v.data.map->count > 0;
        case VAL_VOID:   return false;
        default:         return false;
    }
}
const char *type_name(int tok_type);
const char *value_type_name(int val_type);
int get_node_line(ASTNode *node);
Value resolve_reference(Value v, int line);
void assign_reference(Value *reference, Value value, int line);
int extract_integer_index(ASTNode *node, int line);
bool try_convert_value(Value *val, int target_tok_type);
void report_null_from_missing_return(ASTNode *expr, int expected_vtype, int line);

#endif
