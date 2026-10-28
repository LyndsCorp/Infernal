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
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include "variables.h"
#include "core/value.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "runtime/constants.h"
#include "runtime/evaluator/helpers.h"
#include "developer/debug.h"


/* ============================================================
 *  Helpers internos
 * ============================================================ */

static bool is_valid_var_name(const char *name) {
    if (!name || !*name) return false;
    unsigned char c0 = (unsigned char)name[0];
    if (!(isalpha(c0) || c0 == '_' || c0 >= 0x80)) return false;
    for (const char *p = name + 1; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '_' || c >= 0x80)) return false;
    }
    return true;
}

static int parse_type_name(const char *s) {
    if (!s || !*s) return -1;
    if (strcasecmp(s, "int")    == 0) return TOK_INT;
    if (strcasecmp(s, "uint")   == 0) return TOK_UINT;
    if (strcasecmp(s, "float")  == 0) return TOK_FLOAT;
    if (strcasecmp(s, "bool")   == 0) return TOK_BOOL;
    if (strcasecmp(s, "string") == 0) return TOK_STRING;
    if (strcasecmp(s, "list")   == 0) return TOK_LIST;
    if (strcasecmp(s, "map")    == 0) return TOK_MAP;
    return -1;
}

/*
 * Parseo del string de scope. Solo se aceptan tres valores:
 *
 *   "local"  / ""  → 0  (current_scope)
 *   "script"       → 1  (global_scope, el scope del script actual)
 *   "global"       → 2  (super_global_scope, compartido entre scripts)
 *   otro           → -1
 *
 * IMPORTANTE: en Infernal, el keyword `global` del lenguaje pone las
 * variables en super_global_scope, NO en global_scope. Aquí replicamos
 * exactamente esa semántica para que mkvar(..., "global") se comporte
 * igual que `global x = ...` en el código.
 */
static int parse_scope_name(const char *s) {
    if (!s || !*s) return 0;
    if (strcasecmp(s, "local")  == 0) return 0;
    if (strcasecmp(s, "script") == 0) return 1;
    if (strcasecmp(s, "global") == 0) return 2;
    return -1;
}

static const char *scope_name_str(int scope_type) {
    switch (scope_type) {
        case 0: return "local";
        case 1: return "script";
        case 2: return "global";
        default: return "?";
    }
}

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
        case TOK_UINT: {
            Value result = val_string(s);
            if (!try_convert_value(&result, TOK_UINT)) {
                value_free(&result);
                error(current_eval_line, "mkvar(): \"%s\" no es un uint válido", s);
            }
            return result;
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

static Scope *find_scope_owner(Scope *start, const char *name, VarEntry **out_entry) {
    /* Usamos scope_find_current() (hash O(1) promedio) en lugar de
     * recorrer la lista enlazada de variables. */
    for (Scope *s = start; s; s = s->parent) {
        VarEntry *e = scope_find_current(s, name);
        if (e) { if (out_entry) *out_entry = e; return s; }
    }
    if (global_scope && global_scope != super_global_scope) {
        VarEntry *e = scope_find_current(global_scope, name);
        if (e) { if (out_entry) *out_entry = e; return global_scope; }
    }
    if (super_global_scope) {
        VarEntry *e = scope_find_current(super_global_scope, name);
        if (e) { if (out_entry) *out_entry = e; return super_global_scope; }
    }
    if (out_entry) *out_entry = NULL;
    return NULL;
}

static void scope_remove_entry(Scope *scope, VarEntry *target) {
    VarEntry **link = &scope->vars;
    while (*link) {
        if (*link == target) {
            VarEntry *victim = *link;
            *link = victim->next;
            scope_remove_var_entry(scope, victim);
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

    /* ── Forma con ruta: delvar("mapa[clave1][clave2]...") ──
     *
     * Un `map` es una caja de variables: `pages[web]` se comporta como
     * una variable llamada "web" dentro de la caja `pages`. Esta forma
     * elimina la clave del contenedor, liberando ese "hueco" para que
     * pueda volver a asignarse con otro tipo.
     *
     * Soporta tanto claves entre comillas como identificadores simples:
     *     delvar("pages[web]")
     *     delvar("pages[\"web\"]")
     *     delvar("pages[web][sub]")
     */
    const char *open = strchr(name, '[');
    if (open) {
        size_t base_len = (size_t)(open - name);
        if (base_len == 0)
            error(current_eval_line, "delvar(): ruta inválida \"%s\"", name);

        jmp_buf saved_env;
        memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
        int saved_raised = exception_raised;

        char *base = NULL;
        char **keys = NULL;
        int key_count = 0;
        int key_cap = 0;

        if (setjmp(exception_env) != 0) {
            free(base);
            for (int i = 0; i < key_count; i++) free(keys[i]);
            free(keys);
            memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
            exception_raised = saved_raised;
            longjmp(exception_env, 1);
        }

        base = malloc(base_len + 1);
        if (!base) error(current_eval_line, "delvar(): memoria insuficiente");
        memcpy(base, name, base_len);
        base[base_len] = '\0';

        if (constants_is_reserved(base))
            error(current_eval_line,
                  "delvar(): '%s' es una constante y no se puede eliminar", base);
        if (!is_valid_var_name(base))
            error(current_eval_line,
                  "delvar(): \"%s\" no es un nombre de variable válido", base);

        /* Parsear las claves entre corchetes. */
        const char *p = open;
        while (*p) {
            if (*p != '[')
                error(current_eval_line,
                      "delvar(): ruta inválida \"%s\" (se esperaba '[' en posición %ld)",
                      name, (long)(p - name));
            p++;

            char quote = 0;
            if (*p == '"' || *p == '\'') {
                quote = *p;
                p++;
            }

            const char *key_start = p;
            if (quote) {
                while (*p && *p != quote) {
                    if (*p == '\\' && *(p + 1)) p++;
                    p++;
                }
                if (*p != quote)
                    error(current_eval_line,
                          "delvar(): falta cerrar la comilla '%c' en la ruta \"%s\"",
                          quote, name);
            } else {
                while (*p && *p != ']') p++;
            }
            size_t klen = (size_t)(p - key_start);
            if (klen == 0)
                error(current_eval_line,
                      "delvar(): clave vacía en la ruta \"%s\"", name);

            if (key_count >= key_cap) {
                int new_cap = key_cap == 0 ? 4 : key_cap * 2;
                char **tmp = realloc(keys, (size_t)new_cap * sizeof(char *));
                if (!tmp) error(current_eval_line, "delvar(): memoria insuficiente");
                keys = tmp;
                key_cap = new_cap;
            }
            char *key = malloc(klen + 1);
            if (!key) error(current_eval_line, "delvar(): memoria insuficiente");
            memcpy(key, key_start, klen);
            key[klen] = '\0';
            keys[key_count++] = key;

            if (quote) {
                if (*p != quote)
                    error(current_eval_line,
                          "delvar(): falta cerrar la comilla en la ruta \"%s\"", name);
                p++;
            }
            if (*p != ']')
                error(current_eval_line,
                      "delvar(): falta ']' en la ruta \"%s\"", name);
            p++;
        }

        if (key_count == 0)
            error(current_eval_line, "delvar(): ruta inválida \"%s\"", name);

        /* Buscar la variable base. */
        VarEntry *entry = NULL;
        Scope *owner = find_scope_owner(current_scope, base, &entry);
        if (!owner || !entry)
            error(current_eval_line,
                  "delvar(): la variable \"%s\" no existe (en la ruta \"%s\")",
                  base, name);

        /* Recorrer hasta el penúltimo nivel. */
        Value *container = &entry->value;
        for (int i = 0; i < key_count - 1; i++) {
            if (container->type != VAL_MAP)
                error(current_eval_line,
                      "delvar(): no se puede acceder a [\"%s\"] en la ruta \"%s\": "
                      "el contenedor no es un mapa",
                      keys[i], name);
            if (!val_map_has(*container, keys[i]))
                error(current_eval_line,
                      "delvar(): la clave \"%s\" no existe en la ruta \"%s\"",
                      keys[i], name);
            MapData *md = container->data.map;
            for (int j = 0; j < md->count; j++) {
                if (strcmp(md->pairs[j].key, keys[i]) == 0) {
                    container = &md->pairs[j].value;
                    break;
                }
            }
        }

        if (container->type != VAL_MAP)
            error(current_eval_line,
                  "delvar(): el contenedor final de \"%s\" no es un mapa", name);
        if (!val_map_has(*container, keys[key_count - 1]))
            error(current_eval_line,
                  "delvar(): la clave \"%s\" no existe en la ruta \"%s\"",
                  keys[key_count - 1], name);

        val_map_delete(container, keys[key_count - 1]);

        free(base);
        for (int i = 0; i < key_count; i++) free(keys[i]);
        free(keys);

        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
        exception_raised = saved_raised;

        DEBUG_INFO("delvar(): ruta \"%s\" eliminada", name);
        return val_make_null();
    }

    /* ── Forma simple: delvar("nombre") ── */
    if (constants_is_reserved(name))
        error(current_eval_line, "delvar(): '%s' es una constante y no se puede eliminar", name);
    if (!is_valid_var_name(name))
        error(current_eval_line,
              "delvar(): \"%s\" no es un nombre de variable válido", name);

    VarEntry *entry = NULL;
    Scope *owner = find_scope_owner(current_scope, name, &entry);

    if (!owner || !entry)
        error(current_eval_line, "delvar(): la variable \"%s\" no existe", name);

    scope_remove_entry(owner, entry);

    DEBUG_INFO("delvar(): variable \"%s\" eliminada", name);
    return val_make_null();
}


/* ============================================================
 *  mkvar()
 * ============================================================ */

static Value builtin_mkvar(int argc, Value *args) {
    if (argc < 3 || argc > 4)
        error(current_eval_line,
              "mkvar() espera 3 o 4 argumentos: "
              "mkvar(tipo, nombre, valor[, scope])");

    if (args[0].type != VAL_STRING)
        error(current_eval_line, "mkvar(): el tipo debe ser un string");
    if (args[1].type != VAL_STRING)
        error(current_eval_line, "mkvar(): el nombre debe ser un string");
    if (args[2].type != VAL_STRING)
        error(current_eval_line, "mkvar(): el valor debe ser un string");

    const char *type_str  = args[0].data.sval;
    const char *name      = args[1].data.sval;
    const char *value_str = args[2].data.sval;

    int tok_type = parse_type_name(type_str);
    if (tok_type == -1) {
        error(current_eval_line,
              "mkvar(): tipo \"%s\" no reconocido. "
              "Usa uno de: int, uint, float, bool, string, list, map",
              type_str ? type_str : "");
    }

    if (!name || !*name)
        error(current_eval_line, "mkvar(): el nombre de la variable está vacío");
    if (!is_valid_var_name(name))
        error(current_eval_line,
              "mkvar(): \"%s\" no es un nombre de variable válido", name);

    /*
     * Scope destino (4º argumento opcional). Solo se aceptan "local",
     * "script" y "global". El valor "global" va a super_global_scope,
     * igual que el keyword `global` del lenguaje.
     */
    int scope_type = 0;   /* 0 = local por defecto */
    if (argc == 4) {
        if (args[3].type != VAL_STRING)
            error(current_eval_line, "mkvar(): el scope debe ser un string");
        int parsed = parse_scope_name(args[3].data.sval);
        if (parsed == -1) {
            error(current_eval_line,
                  "mkvar(): scope \"%s\" no reconocido. "
                  "Usa uno de: local, script, global",
                  args[3].data.sval ? args[3].data.sval : "");
        }
        scope_type = parsed;
    }

    Value val = convert_string_to_type(value_str, tok_type);

    Scope *target = NULL;
    switch (scope_type) {
        case 0: target = current_scope;      break;
        case 1: target = global_scope;       break;
        case 2: target = super_global_scope; break;
    }
    if (!target) {
        value_free(&val);
        error(current_eval_line,
              "mkvar(): el scope \"%s\" no está disponible",
              scope_name_str(scope_type));
    }

    VarEntry *existing = scope_find_current(target, name);

    if (existing) {
        value_free(&existing->value);
        existing->value = val;
        existing->vtype = tok_type;
        DEBUG_INFO("mkvar(): variable \"%s\" actualizada en scope %s (tipo=%d)",
                   name, scope_name_str(scope_type), tok_type);
    } else {
        scope_define(target, name, tok_type, val);
        DEBUG_INFO("mkvar(): variable \"%s\" creada en scope %s (tipo=%d)",
                   name, scope_name_str(scope_type), tok_type);
    }
    return val_make_null();
}


/* ============================================================
 *  varexists()
 * ============================================================ */

static Value builtin_varexists(int argc, Value *args) {
    if (argc != 1)
        error(current_eval_line,
              "varexists() espera exactamente 1 argumento: varexists(nombre)");
    if (args[0].type != VAL_STRING)
        error(current_eval_line,
              "varexists() espera un string con el nombre de la variable");

    const char *name = args[0].data.sval;
    if (!name || !*name)
        error(current_eval_line, "varexists(): el nombre de la variable está vacío");

    /* scope_find ya recorre la cadena de scopes + global_scope
     * + super_global_scope. Da igual dónde viva la variable. */
    if (scope_find(current_scope, name) != NULL)
        return val_bool(true);

    /* Las constantes también se leen como si fueran variables,
     * así que varexists("_PID") debe devolver true. */
    if (constants_is_reserved(name))
        return val_bool(true);

    return val_bool(false);
}

/* ============================================================
 *  vartype()
 * ============================================================ */

static Value builtin_vartype(int argc, Value *args) {
    if (argc < 1) error(0, "vartype requiere un argumento");
    Value arg = args[0];
    const char *t = value_type_name(arg.type);
    return val_string(t);
}

static uint64_t value_payload_size(Value value, unsigned depth) {
    if (depth > 256)
        error(current_eval_line, "sizeof(): la estructura supera la profundidad máxima");

    switch (value.type) {
        case VAL_INT:
            return bigint_min_width(value_as_bigint(value), true);
        case VAL_UINT:
            return bigint_min_width(value_as_bigint(value), false);
        case VAL_BOOL:
            return 1; /* un bit se representa como un byte al medir tamaños */
        case VAL_FLOAT:
            return sizeof(double);
        case VAL_STRING:
            return value.data.sval ? (uint64_t)strlen(value.data.sval) : 0;
        case VAL_LIST: {
            uint64_t total = 0;
            for (int i = 0; i < value.data.list.count; ++i) {
                uint64_t item_size = value_payload_size(value.data.list.items[i], depth + 1);
                if (UINT64_MAX - total < item_size)
                    error(current_eval_line, "sizeof(): el tamaño de la lista excede el máximo admitido");
                total += item_size;
            }
            return total;
        }
        case VAL_MAP: {
            uint64_t total = 0;
            MapData *map = value.data.map;
            if (!map) return 0;
            for (int i = 0; i < map->count; ++i) {
                uint64_t key_size = map->pairs[i].key
                    ? (uint64_t)strlen(map->pairs[i].key) : 0;
                uint64_t item_size = value_payload_size(map->pairs[i].value, depth + 1);
                if (UINT64_MAX - total < key_size ||
                    UINT64_MAX - total - key_size < item_size)
                    error(current_eval_line, "sizeof(): el tamaño del mapa excede el máximo admitido");
                total += key_size + item_size;
            }
            return total;
        }
        case VAL_PTR:
            return sizeof(void *);
        case VAL_REFERENCE:
            return sizeof(void *) + (value.data.ref.map_key
                ? (uint64_t)strlen(value.data.ref.map_key) : 0);
        case VAL_NULL:
        case VAL_VOID:
        default:
            return 0;
    }
}

static Value builtin_sizeof(int argc, Value *args) {
    if (argc != 1)
        error(current_eval_line, "sizeof() espera exactamente 1 argumento: sizeof(variable)");
    return val_bigint(bigint_from_u64(value_payload_size(args[0], 0)), true);
}

/* ============================================================
 *  Registro
 * ============================================================ */

void register_variables_builtins(void) {
    func_register_builtin("delvar", builtin_delvar);
    func_register_builtin("mkvar",  builtin_mkvar);
    func_register_builtin("varexists", builtin_varexists);
    func_register_builtin("vartype",   builtin_vartype);
    func_register_builtin("sizeof",    builtin_sizeof);
}
