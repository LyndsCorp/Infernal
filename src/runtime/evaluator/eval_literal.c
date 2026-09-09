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
#include <setjmp.h>

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
    Value result = copy_value_secure(e->value);
    if (result.type == VAL_REFERENCE)
        return resolve_reference(result, expr->line);
    return result;
}

Value eval_list(ASTNode *expr) {
    Value list = val_list_empty();
    for (int i = 0; i < expr->data.list_lit.count; i++) {
        val_list_append(&list, eval_expr(expr->data.list_lit.items[i]));
    }
    return list;
}

Value eval_map(ASTNode *expr) {
    Value *map = malloc(sizeof(*map));
    if (!map) error(expr->line, "Memoria insuficiente para crear mapa");
    *map = val_map_empty();

    Scope *old_scope = current_scope;
    Scope *map_scope = scope_new(old_scope, NULL);

    jmp_buf saved_env;
    memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
    int saved_raised = exception_raised;
    if (setjmp(exception_env) != 0) {
        current_scope = old_scope;
        scope_free(map_scope);
        value_free(map);
        free(map);
        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
        exception_raised = saved_raised;
        longjmp(exception_env, 1);
    }

    current_scope = map_scope;
    for (int i = 0; i < expr->data.map.pair_count; i++) {
        const char *key = expr->data.map.pairs[i].key;
        int declared_type = expr->data.map.pairs[i].value_type;
        ASTNode *value_node = expr->data.map.pairs[i].value;

        /* Dentro de un mapa, una variable externa o una entrada previa se clona
         * de forma explícita con $. Un identificador desnudo es un comando. */
        if (value_node && value_node->kind == NODE_VAR && !value_node->data.var.clone) {
            error(expr->line, "Comando '%s' no encontrado. Para usar una variable accesible dentro de un mapa, usa $%s",
                  value_node->data.var.name, value_node->data.var.name);
        }

        Value val = eval_expr(value_node);
        if (declared_type != 0 && valtype_to_tokentype(val.type) != declared_type) {
            int actual_type = valtype_to_tokentype(val.type);
            value_free(&val);
            error(expr->line, "Error de tipado del mapa: la clave '%s' requiere un valor %s pero se obtuvo %s",
                  key, type_name(declared_type), type_name(actual_type));
        }

        val_map_set_typed(map, key, val, declared_type);
        scope_define(map_scope, key, declared_type, copy_value_secure(val));
        value_free(&val);
    }

    Value result = *map;
    free(map);
    current_scope = old_scope;
    scope_free(map_scope);
    memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
    exception_raised = saved_raised;
    return result;
}
