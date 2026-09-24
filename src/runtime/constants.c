/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/constants.c
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "constants.h"
#include "core/memory.h"
#include "runtime/error.h"
#include "runtime/evaluator/helpers.h"
#include "runtime/globals.h"


typedef struct ConstantEntry {
    char *name;
    int vtype;
    Value value;
    bool internal;
    bool user_defined;
    ConstantGetter getter;
    ConstantSetter setter;
    struct ConstantEntry *next;
} ConstantEntry;

static ConstantEntry *constant_table = NULL;
static bool definition_allowed = true;

static ConstantEntry *find_constant(const char *name) {
    for (ConstantEntry *entry = constant_table; entry; entry = entry->next) {
        if (strcmp(entry->name, name) == 0)
            return entry;
    }
    return NULL;
}

static void internal_registration_error(const char *message) {
    fprintf(stderr, "Error interno de Infernal: %s\n", message);
    abort();
}

static void register_internal_constant(const char *name,
                                       int vtype,
                                       Value default_value,
                                       ConstantGetter getter,
                                       ConstantSetter setter) {
    if (!name || name[0] != '_')
        internal_registration_error("las constantes internas deben empezar por '_'");
    if (vtype == 0)
        internal_registration_error("una constante interna debe tener un tipo fijo");
    if (find_constant(name))
        internal_registration_error("se intentó registrar dos veces una constante interna");

    ConstantEntry *entry = infernal_malloc(sizeof(*entry));
    entry->name = infernal_strdup(name);
    entry->vtype = vtype;
    entry->value = default_value;
    entry->internal = true;
    entry->user_defined = false;
    entry->getter = getter;
    entry->setter = setter;
    entry->next = constant_table;
    constant_table = entry;
}

bool constants_lookup(const char *name, Value *out) {
    if (!name) return false;

    ConstantEntry *entry = find_constant(name);
    if (!entry) return false;

    if (entry->getter) {
        Value current = entry->getter();
        value_free(&entry->value);
        entry->value = copy_value_secure(current);
        if (out) *out = current;
        else value_free(&current);
        return true;
    }

    if (out) *out = copy_value_secure(entry->value);
    return true;
}

bool constants_is_reserved(const char *name) {
    return name && find_constant(name) != NULL;
}

void constants_define(const char *name, Value value, int line) {
    if (!definition_allowed) {
        value_free(&value);
        error(line,
              "Las constantes solo se pueden definir en el script principal; "
              "los scripts ejecutados con 'execute' no pueden definirlas");
    }

    if (!name || !*name) {
        value_free(&value);
        error(line, "define: el nombre de la constante está vacío");
    }

    ConstantEntry *entry = find_constant(name);
    int actual_type = valtype_to_tokentype(value.type);

    if (!entry && current_scope && scope_find(current_scope, name)) {
        value_free(&value);
        error(line, "El nombre '%s' ya está usado por una variable y no se puede convertir en constante", name);
    }

    if (entry) {
        if (entry->internal && !entry->user_defined) {
            if (actual_type != entry->vtype) {
                value_free(&value);
                error(line,
                      "La constante interna \"%s\" espera un valor de tipo %s, "
                      "pero se recibió %s",
                      name, type_name(entry->vtype), type_name(actual_type));
            }

            value_free(&entry->value);
            entry->value = copy_value_secure(value);
            entry->user_defined = true;

            if (entry->setter)
                entry->setter(&value);

            value_free(&value);
            return;
        }

        value_free(&value);
        error(line, "La constante \"%s\" ya está definida y no se puede volver a definir ni sobreescribir", name);
    }

    /* Los nombres con '_' quedan reservados para constantes internas. */
    if (name[0] == '_') {
        value_free(&value);
        error(line,
              "La constante \"%s\" usa el prefijo reservado '_' pero no existe como constante interna",
              name);
    }

    if (actual_type == 0) {
        value_free(&value);
        error(line, "define: no se puede determinar el tipo de \"%s\"", name);
    }

    entry = infernal_malloc(sizeof(*entry));
    entry->name = infernal_strdup(name);
    entry->vtype = actual_type;
    entry->value = value;
    entry->internal = false;
    entry->user_defined = true;
    entry->getter = NULL;
    entry->setter = NULL;
    entry->next = constant_table;
    constant_table = entry;
}

void constants_set_definition_allowed(bool allowed) {
    definition_allowed = allowed;
}

bool constants_definition_allowed(void) {
    return definition_allowed;
}

/* -------------------------------------------------------------------------
 * Constantes internas de Infernal
 * ------------------------------------------------------------------------- */

static Value get_max_loop_limit(void) {
    return val_int(max_loop_iterations);
}

static void set_max_loop_limit(const Value *value) {
    if (!value || value->type != VAL_INT)
        internal_registration_error("_MAX_LOOP_LIMIT recibió un valor con tipo inválido");
    max_loop_iterations = value->data.ival;
}

static Value get_command_fail_error(void) {
    return val_bool(command_fail_error);
}

static void set_command_fail_error(const Value *value) {
    if (!value || value->type != VAL_BOOL)
        internal_registration_error("_COMMAND_FAIL_ERROR recibió un valor con tipo inválido");
    command_fail_error = value->data.bval;
}

static Value get_infernal_shell(void) {
    return val_string(infernal_shell ? infernal_shell : "/bin/sh");
}

static void set_infernal_shell(const Value *value) {
    if (!value || value->type != VAL_STRING)
        internal_registration_error("_INFERNAL_SHELL recibió un valor con tipo inválido");
    if (!value->data.sval || !value->data.sval[0])
        internal_registration_error("_INFERNAL_SHELL no puede estar vacío");

    char *new_shell = infernal_strdup(value->data.sval);
    free(infernal_shell);
    infernal_shell = new_shell;
}

extern const char *get_metadata(const char *type);

static Value get_infernal_version(void) {
    const char *version = get_metadata("VERSION");
    return val_string(version ? version : "");
}

static Value get_max_string_len(void) {
    return val_int(max_string_len);
}

static void set_max_string_len(const Value *value) {
    if (!value || value->type != VAL_INT)
        internal_registration_error("_MAX_STRING_LEN recibió un valor con tipo inválido");
    if (value->data.ival < 0)
        internal_registration_error("_MAX_STRING_LEN no puede ser negativo");
    max_string_len = value->data.ival;
}

static Value get_pid(void) {
    return val_int((int)getpid());
}

void register_all_constants(void) {
    register_internal_constant("_COMMAND_FAIL_ERROR",
                               TOK_BOOL,
                               val_bool(command_fail_error),
                               get_command_fail_error,
                               set_command_fail_error);
    register_internal_constant("_INFERNAL_SHELL",
                               TOK_STRING,
                               val_string(infernal_shell ? infernal_shell : "/bin/sh"),
                               get_infernal_shell,
                               set_infernal_shell);
    register_internal_constant("_INFERNAL_VERSION",
                               TOK_STRING,
                               get_infernal_version(),
                               get_infernal_version,
                               NULL);
    register_internal_constant("_MAX_LOOP_LIMIT",
                               TOK_INT,
                               val_int(max_loop_iterations),
                               get_max_loop_limit,
                               set_max_loop_limit);
    register_internal_constant("_MAX_STRING_LEN",
                               TOK_INT,
                               val_int(max_string_len),
                               get_max_string_len,
                               set_max_string_len);
    register_internal_constant("_PID",
                               TOK_INT,
                               val_int((int)getpid()),
                               get_pid,
                               NULL);
}

void constants_cleanup(void) {
    while (constant_table) {
        ConstantEntry *entry = constant_table;
        constant_table = entry->next;
        free(entry->name);
        value_free(&entry->value);
        free(entry);
    }
    definition_allowed = true;
}

void constants_foreach(ConstantVisitor visitor, void *user_data) {
    if (!visitor) return;
    for (ConstantEntry *entry = constant_table; entry; entry = entry->next) {
        Value v;
        if (entry->getter) {
            v = entry->getter();
        } else {
            v = copy_value_secure(entry->value);
        }
        visitor(entry->name, entry->vtype, &v, entry->internal, user_data);
        value_free(&v);
    }
}
