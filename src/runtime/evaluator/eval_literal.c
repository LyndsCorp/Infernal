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

    /*
     * Un mapa tiene su propio ámbito léxico durante la construcción.
     * Esto permite que los valores de las entradas anteriores sean
     * referenciados mediante $nombre, igual que cualquier otra variable
     * accesible, pero sin convertir la clave en una expresión evaluable.
     *
     * Ejemplo:
     *   var = "abc",
     *   numeros = $var
     *
     * $var se resuelve primero en este ámbito del mapa y, si no existe,
     * continúa por los scopes padres mediante scope_find().
     */
    Scope *map_scope = scope_new(current_scope, NULL);
    Scope *old_scope = current_scope;
    current_scope = map_scope;

    for (int i = 0; i < expr->data.map.pair_count; i++) {
        const char *key = expr->data.map.pairs[i].key;
        int declared_type = expr->data.map.pairs[i].value_type;
        Value val = eval_expr(expr->data.map.pairs[i].value);
        if (declared_type != 0 && valtype_to_tokentype(val.type) != declared_type) {
            int actual_type = valtype_to_tokentype(val.type);
            current_scope = old_scope;
            scope_free(map_scope);
            error(expr->line, "Error de tipado del mapa: la clave '%s' requiere un valor %s pero se obtuvo %s",
                  key, type_name(declared_type), type_name(actual_type));
        }

        /* El valor almacenado en el mapa es una copia independiente. */
        val_map_set_typed(&map, key, val, declared_type);

        /*
         * Publicamos la entrada en el scope del mapa para que las siguientes
         * entradas puedan usar $key. También respetamos su tipo explícito o
         * el tipo inferido por el valor.
         */
        scope_define(map_scope, key, declared_type, copy_value_secure(val));
        value_free(&val);
    }

    current_scope = old_scope;
    scope_free(map_scope);
    return map;
}
