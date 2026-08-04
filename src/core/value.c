/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License.
 * Código fuente de Infernal: core/value.c
*/

#include <stdlib.h>
#include <string.h>
#include "value.h"
#include "memory.h"

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
    Value v; v.type = VAL_STRING;
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

/* ─── Copia profunda de un valor ────────────────────────────── */
Value value_copy(Value src) {
    if (src.type == VAL_STRING) {
        return val_string(src.data.sval);
    } else if (src.type == VAL_LIST) {
        Value new_list = val_list_empty();
        for (int i = 0; i < src.data.list.count; i++) {
            val_list_append(&new_list, value_copy(src.data.list.items[i]));
        }
        return new_list;
    } else {
        return src; // int, float, bool, null, ptr se copian por valor
    }
}

Value val_list_copy(Value *src) {
    return value_copy(*src);
}

/* ─── Liberar recursivamente un valor ──────────────────────── */
void free_value(Value v) {
    if (v.type == VAL_STRING) {
        free(v.data.sval);
    } else if (v.type == VAL_LIST) {
        for (int i = 0; i < v.data.list.count; i++) {
            free_value(v.data.list.items[i]);
        }
        free(v.data.list.items);
    }
    // otros tipos no requieren liberación
}

int valtype_to_tokentype(int vtype) {
    switch (vtype) {
        case VAL_INT:    return TOK_INT;
        case VAL_FLOAT:  return TOK_FLOAT;
        case VAL_BOOL:   return TOK_BOOL;
        case VAL_STRING: return TOK_STRING;
        case VAL_LIST:   return TOK_LIST;
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
