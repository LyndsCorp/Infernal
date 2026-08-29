/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/eval_literal.c
*/

#include "eval_literal.h"
#include "helpers.h"
#include "evaluator.h"
#include "core/value.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "developer/debug.h"
#include <string.h>
#include <stdlib.h>

Value eval_literal(ASTNode *expr) {
    if (expr->data.lit.type == TOK_INT)    return val_int(expr->data.lit.ival);
    if (expr->data.lit.type == TOK_FLOAT)  return val_float(expr->data.lit.fval);
    if (expr->data.lit.type == TOK_BOOL)   return val_bool(expr->data.lit.bval);
    if (expr->data.lit.type == TOK_STRING) return val_string(expr->data.lit.sval);
    return val_make_null();
}

Value eval_var(ASTNode *expr) {
    const char *name = expr->data.var.name;
    if (name[0] == '$' || name[0] == '?') name++;
    if (*name == '\0')
        error(expr->line, "Nombre de variable vacío");

    DEBUG_INFO("eval_var: buscando variable '%s' en current_scope=%p", name, (void*)current_scope);

    if (strchr(name, '/') != NULL) {
        VarEntry *e = scope_find(current_scope, name);
        if (!e) {
            error(expr->line,
                  "La variable '%s' no existe. Si intentabas concatenar una variable con una cadena, "
                  "usa el operador '+', por ejemplo: $%s + '/ruta'. La barra '/' directa solo es válida "
                  "en comandos shell, no en nombres de variable.",
                  name, name);
        }
        return copy_value_secure(e->value);
    }

    VarEntry *e = scope_find(current_scope, name);
    if (!e) {
        error(expr->line, "Variable '%s' no definida", name);
    }
    DEBUG_INFO("eval_var: variable '%s' encontrada, valor tipo %d", name, e->value.type);
    return copy_value_secure(e->value);
}

Value eval_list(ASTNode *expr) {
    Value list = val_list_empty();
    for (int i = 0; i < expr->data.list_lit.count; i++) {
        val_list_append(&list, eval_expr(expr->data.list_lit.items[i]));
    }
    return list;
}

Value eval_map(ASTNode *expr) {
    Value map = val_map_empty();
    for (int i = 0; i < expr->data.map.pair_count; i++) {
        Value key = eval_expr(expr->data.map.pairs[i].key);
        Value val = eval_expr(expr->data.map.pairs[i].value);
        if (key.type != VAL_STRING) {
            value_free(&key);
            value_free(&val);
            value_free(&map);
            error(expr->line, "La clave de un mapa debe ser string (obtenido tipo %d)", key.type);
        }
        val_map_set(&map, key.data.sval, val);
        value_free(&key);
        value_free(&val);
    }
    return map;
}
