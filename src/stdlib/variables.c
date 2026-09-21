/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: stdlib/variables.c
 *
 * Funciones para manipular variables en tiempo de ejecución.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdbool.h>
#include "variables.h"
#include "core/value.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "vm/vm.h"
#include "developer/debug.h"


/* ============================================================
 *  Helpers internos
 * ============================================================ */

/* --- Validación del nombre de variable ---
 *
 * Debe ser un identificador de Infernal: empieza por letra o '_' y
 * continúa con letras, dígitos o '_'. Así una librería no puede crear
 * variables con nombres raros ("3x", "mi var", "a-b") que después no
 * se podrían usar desde el lenguaje. */
static bool is_valid_var_name(const char *name) {
    if (!name || !*name) return false;
    unsigned char c0 = (unsigned char)name[0];
    if (!(isalpha(c0) || c0 == '_')) return false;
    for (const char *p = name + 1; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '_')) return false;
    }
    return true;
}

/* --- Parseo del string de tipo ---
 *
 * Devuelve el TOK_* correspondiente, o -1 si el string no es un tipo
 * reconocido. La comparación es case-insensitive. */
static int parse_type_name(const char *s) {
    if (!s || !*s) return -1;
    if (strcasecmp(s, "int")    == 0) return TOK_INT;
    if (strcasecmp(s, "float")  == 0) return TOK_FLOAT;
    if (strcasecmp(s, "bool")   == 0) return TOK_BOOL;
    if (strcasecmp(s, "string") == 0) return TOK_STRING;
    if (strcasecmp(s, "list")   == 0) return TOK_LIST;
    if (strcasecmp(s, "map")    == 0) return TOK_MAP;
    return -1;
}

/* --- Conversión de string al tipo indicado ---
 *
 * Lanza error() si el string no se puede interpretar como el tipo pedido.
 * Para list y map solo se acepta el contenedor vacío ("" o "[]"), porque
 * mkvar está pensado para crear contenedores que luego se rellenan desde
 * el lenguaje. */
static Value convert_string_to_type(const char *s, int tok_type) {
    if (!s) s = "";

    switch (tok_type) {
        case TOK_INT: {
            char *end = NULL;
            long n = strtol(s, &end, 10);
            if (!end || *end != '\0' || end == s ||
                n < -2147483647L - 1L || n > 2147483647L) {
                error(current_eval_line,
                      "mkvar(): \"%s\" no es un int válido", s);
            }
            return val_int((int)n);
        }

        case TOK_FLOAT: {
            char *normalized = strdup(s);
            if (!normalized)
                error(current_eval_line, "mkvar(): memoria insuficiente");
            for (char *p = normalized; *p; p++) if (*p == ',') *p = '.';
            char *end = NULL;
            double f = strtod(normalized, &end);
            bool ok = (end && *end == '\0' && end != normalized);
            free(normalized);
            if (!ok) {
                error(current_eval_line,
                      "mkvar(): \"%s\" no es un float válido", s);
            }
            return val_float(f);
        }

        case TOK_BOOL:
            if (strcasecmp(s, "true") == 0 || strcmp(s, "1") == 0)
                return val_bool(true);
            if (strcasecmp(s, "false") == 0 || strcmp(s, "0") == 0)
                return val_bool(false);
            error(current_eval_line,
                  "mkvar(): \"%s\" no es un bool válido "
                  "(usa \"true\", \"false\", \"1\" o \"0\")", s);

        case TOK_STRING:
            return val_string(s);

        case TOK_LIST:
            if (*s == '\0' || strcmp(s, "[]") == 0 || strcmp(s, "[ ]") == 0)
                return val_list_empty();
            error(current_eval_line,
                  "mkvar(): para list solo se admite \"\" o \"[]\". "
                  "Para crear una lista con contenido, constrúyela en el lenguaje.");

        case TOK_MAP:
            if (*s == '\0' || strcmp(s, "[]") == 0 || strcmp(s, "[ ]") == 0)
                return val_map_empty();
            error(current_eval_line,
                  "mkvar(): para map solo se admite \"\" o \"[]\". "
                  "Para crear un mapa con contenido, constrúyelo en el lenguaje.");

        default:
            error(current_eval_line,
                  "mkvar(): tipo no soportado (%d)", tok_type);
    }
}

/* --- Búsqueda del scope que contiene la variable ---
 *
 * Reproduce el mismo orden que scope_find (cadena de padres, luego
 * global_scope, luego super_global_scope) pero además devuelve el Scope*
 * dueño, que scope_find no expone. Se usa solo desde delvar(). */
static Scope *find_scope_owner(Scope *start, const char *name, VarEntry **out_entry) {
    for (Scope *s = start; s; s = s->parent) {
        for (VarEntry *e = s->vars; e; e = e->next) {
            if (strcmp(e->name, name) == 0) {
                if (out_entry) *out_entry = e;
                return s;
            }
        }
    }
    if (global_scope && global_scope != super_global_scope) {
        for (VarEntry *e = global_scope->vars; e; e = e->next) {
            if (strcmp(e->name, name) == 0) {
                if (out_entry) *out_entry = e;
                return global_scope;
            }
        }
    }
    if (super_global_scope) {
        for (VarEntry *e = super_global_scope->vars; e; e = e->next) {
            if (strcmp(e->name, name) == 0) {
                if (out_entry) *out_entry = e;
                return super_global_scope;
            }
        }
    }
    if (out_entry) *out_entry = NULL;
    return NULL;
}

/* --- Desenlaza y libera un VarEntry concreto de un scope --- */
static void scope_remove_entry(Scope *scope, VarEntry *target) {
    VarEntry **link = &scope->vars;
    while (*link) {
        if (*link == target) {
            VarEntry *victim = *link;
            *link = victim->next;
            free(victim->name);
            value_free(&victim->value);
            free(victim);
            return;
        }
        link = &(*link)->next;
    }
}


/* ============================================================
 *  delvar()
 * ============================================================ */

static Value builtin_delvar(int argc, Value *args) {
    if (argc != 1)
        error(current_eval_line,
              "delvar() espera exactamente 1 argumento: delvar(nombre)");
    if (args[0].type != VAL_STRING)
        error(current_eval_line,
              "delvar() espera un string con el nombre de la variable");

    const char *name = args[0].data.sval;
    if (!name || !*name)
        error(current_eval_line, "delvar(): el nombre de la variable está vacío");
    if (!is_valid_var_name(name))
        error(current_eval_line,
              "delvar(): \"%s\" no es un nombre de variable válido", name);

    VarEntry *entry = NULL;
    Scope *owner = find_scope_owner(current_scope, name, &entry);

    if (!owner || !entry)
        error(current_eval_line, "delvar(): la variable \"%s\" no existe", name);

    scope_remove_entry(owner, entry);

    /* Si la variable era global (script o superglobal), reflejamos el
     * borrado en el estado de la VM para que las lecturas posteriores
     * desde bytecode no vean valores huérfanos. */
    if (owner == global_scope || owner == super_global_scope) {
        int gidx = vm_find_global_index(name);
        if (gidx >= 0) {
            value_free(&vm_globals[gidx]);
            vm_globals[gidx] = val_make_null();
        }
    }

    DEBUG_INFO("delvar(): variable \"%s\" eliminada", name);
    return val_make_null();
}


/* ============================================================
 *  mkvar()
 * ============================================================ */

static Value builtin_mkvar(int argc, Value *args) {
    if (argc != 3)
        error(current_eval_line,
              "mkvar() espera exactamente 3 argumentos: "
              "mkvar(tipo, nombre, valor)");

    if (args[0].type != VAL_STRING)
        error(current_eval_line, "mkvar(): el tipo debe ser un string");
    if (args[1].type != VAL_STRING)
        error(current_eval_line, "mkvar(): el nombre debe ser un string");
    if (args[2].type != VAL_STRING)
        error(current_eval_line, "mkvar(): el valor debe ser un string");

    const char *type_str  = args[0].data.sval;
    const char *name      = args[1].data.sval;
    const char *value_str = args[2].data.sval;

    /* --- Validar tipo --- */
    int tok_type = parse_type_name(type_str);
    if (tok_type == -1) {
        error(current_eval_line,
              "mkvar(): tipo \"%s\" no reconocido. "
              "Usa uno de: int, float, bool, string, list, map",
              type_str ? type_str : "");
    }

    /* --- Validar nombre --- */
    if (!name || !*name)
        error(current_eval_line, "mkvar(): el nombre de la variable está vacío");
    if (!is_valid_var_name(name))
        error(current_eval_line,
              "mkvar(): \"%s\" no es un nombre de variable válido", name);

    /* --- Convertir el valor --- */
    Value val = convert_string_to_type(value_str, tok_type);

    /* --- Crear o actualizar en el scope actual ---
     *
     * Si el script llama a mkvar desde el nivel superior, current_scope
     * es global_scope; si es desde dentro de una función, es el scope
     * local de esa función. El comportamiento es el mismo que asignar
     * con `=` en ese punto del código. */
    Scope *target = current_scope;
    if (!target)
        error(current_eval_line, "mkvar(): no hay scope activo");

    VarEntry *existing = scope_find_current(target, name);
    VarEntry *target_entry = NULL;

    if (existing) {
        value_free(&existing->value);
        existing->value = val;
        existing->vtype = tok_type;
        target_entry = existing;
        DEBUG_INFO("mkvar(): variable \"%s\" actualizada (tipo=%d)", name, tok_type);
    } else {
        scope_define(target, name, tok_type, val);
        target_entry = scope_find_current(target, name);
        DEBUG_INFO("mkvar(): variable \"%s\" creada (tipo=%d)", name, tok_type);
    }

    /* --- Sincronizar con la VM si estamos en un scope global ---
     *
     * El bytecode accede a las variables globales vía vm_globals[], no
     * vía la cadena de scopes. Si mkvar se llamó en el nivel superior
     * (current_scope == global_scope) hay que reflejar el valor en la
     * tabla de la VM para que el bytecode lo vea. */
    if (target_entry && (target == global_scope || target == super_global_scope)) {
        int gidx = vm_find_global_index(name);
        if (gidx < 0) {
            gidx = vm_register_global(
                name,
                target == super_global_scope ? GLOBAL_SUPER : GLOBAL_SCRIPT,
                tok_type
            );
        }
        if (gidx >= 0) {
            value_free(&vm_globals[gidx]);
            vm_globals[gidx] = copy_value_secure(target_entry->value);
            vm_global_types[gidx] = tok_type;
        }
    }

    return val_make_null();
}


/* ============================================================
 *  Registro
 * ============================================================ */

void register_variables_builtins(void) {
    func_register_builtin("delvar", builtin_delvar);
    func_register_builtin("mkvar",  builtin_mkvar);

    vm_register_builtin("delvar", builtin_delvar);
    vm_register_builtin("mkvar",  builtin_mkvar);
}
