/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/globals.c
*/

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include "globals.h"
#include "developer/debug.h"
#include "core/memory.h"

Scope *global_scope = NULL;
Scope *super_global_scope = NULL;
Scope *current_scope = NULL;

FuncEntry *func_table = NULL;
FuncEntry *super_func_table = NULL;
#define FUNC_BUCKET_COUNT 1024u
static FuncEntry *func_buckets[FUNC_BUCKET_COUNT];
static FuncEntry *super_func_buckets[FUNC_BUCKET_COUNT];
char *current_import_prefix = NULL;
int max_loop_iterations = 10000;
bool command_fail_error = true;
int max_string_len = 16777216;

jmp_buf exception_env;
int exception_raised = 0;
char exception_msg[4096];
char **source_lines = NULL;
int source_line_count = 0;
int control_flow = CF_NONE;
Value return_value;

int script_argc;
char **script_argv;

int repeat_line_target = 0;

char *script_dir = NULL;
char *current_source_file = NULL;

int current_eval_line = 0;
int flags_arg_index = 2;   /* primer argumento después del script */

static unsigned long func_name_hash(const char *name) {
    unsigned long hash = 1469598103934665603UL;
    while (*name) {
        hash ^= (unsigned char)*name++;
        hash *= 1099511628211UL;
    }
    return hash;
}

static void func_index_insert(FuncEntry **buckets, FuncEntry *entry) {
    size_t bucket = func_name_hash(entry->name) % FUNC_BUCKET_COUNT;
    entry->hash_next = buckets[bucket];
    buckets[bucket] = entry;
}

/* --- Registro de modos de flags definidos ------------------ */
int defined_flags_modes[MAX_FLAGS_MODES] = {0};

/* --- Shell configurado ------------------------------------- */
char *infernal_shell = NULL;

/* --- Función auxiliar para parsear archivo de configuración -- */
static int parse_config_file(const char *path, char **shell_ptr) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';

        char *p = line;
        while (*p && isspace(*p)) p++;
        if (!*p) continue;

        if (strncasecmp(p, "SHELL", 5) == 0) {
            p += 5;
            while (*p && isspace(*p)) p++;
            if (*p == '=') {
                p++;
                while (*p && isspace(*p)) p++;
                char *end = p + strlen(p);
                while (end > p && isspace(*(end - 1))) end--;
                *end = '\0';
                if (*p) {
                    if (*shell_ptr) free(*shell_ptr);
                    *shell_ptr = infernal_strdup(p);
                    fclose(fp);
                    return 1;
                }
            }
        }
    }
    fclose(fp);
    return 0;
}

/* --- Carga de configuración de shell ---------------------- */
void load_infernal_config(void) {
    if (infernal_shell) {
        DEBUG_INFO("Shell ya configurado, omitiendo carga de configuraciones");
        return;
    }

    DEBUG_INFO("Cargando configuración de shell...");
    char *home = getenv("HOME");
    char user_path[PATH_MAX];
    if (home) {
        snprintf(user_path, sizeof(user_path), "%s/.infernalrc", home);
        if (access(user_path, R_OK) == 0) {
            if (parse_config_file(user_path, &infernal_shell)) {
                DEBUG_INFO("Cargado SHELL=%s desde ~/.infernalrc", infernal_shell);
            }
        }
    }

    if (!infernal_shell && access("/etc/infernalrc", R_OK) == 0) {
        if (parse_config_file("/etc/infernalrc", &infernal_shell)) {
            DEBUG_INFO("Cargado SHELL=%s desde /etc/infernalrc", infernal_shell);
        }
    }

    if (!infernal_shell) {
        infernal_shell = infernal_strdup("/bin/sh");
        DEBUG_INFO("Usando shell por defecto: /bin/sh");
    } else {
        DEBUG_INFO("Shell configurado: %s", infernal_shell);
    }
}

/* --- Mostrar información de configuración de shell -------- */
void show_shell_info(void) {
    char *user_shell = NULL;
    char *system_shell = NULL;
    char *home = getenv("HOME");
    char user_path[PATH_MAX];
    if (home) {
        snprintf(user_path, sizeof(user_path), "%s/.infernalrc", home);
        if (access(user_path, R_OK) == 0) {
            parse_config_file(user_path, &user_shell);
        }
    }
    if (access("/etc/infernalrc", R_OK) == 0) {
        parse_config_file("/etc/infernalrc", &system_shell);
    }

    printf("Configuración de shell para Infernal:\n");
    if (user_shell) {
        printf("  Usuario (~/.infernalrc): %s\n", user_shell);
        free(user_shell);
    } else {
        printf("  Usuario (~/.infernalrc): no configurado\n");
    }
    if (system_shell) {
        printf("  Sistema (/etc/infernalrc): %s\n", system_shell);
        free(system_shell);
    } else {
        printf("  Sistema (/etc/infernalrc): no configurado\n");
    }
    printf("  Fallback: /bin/sh\n");

    // Determinar cuál se usaría según prioridad
    char *effective = NULL;
    if (home) {
        snprintf(user_path, sizeof(user_path), "%s/.infernalrc", home);
        if (access(user_path, R_OK) == 0) {
            parse_config_file(user_path, &effective);
        }
    }
    if (!effective && access("/etc/infernalrc", R_OK) == 0) {
        parse_config_file("/etc/infernalrc", &effective);
    }
    if (!effective) effective = strdup("/bin/sh");

    printf("Shell efectivo (prioridad): %s\n", effective);
    free(effective);
}

static void func_register_user_entry(const char *name, ASTNode *def,
                                     bool in_super_table, const char *module,
                                     bool exported) {
    FuncObject *obj = infernal_malloc(sizeof(FuncObject));
    obj->kind = FUNC_USER;
    obj->def = def;
    FuncEntry *e = infernal_malloc(sizeof(FuncEntry));
    e->name = infernal_strdup(name);
    e->obj = obj;
    e->module = module ? infernal_strdup(module) : NULL;
    e->exported = exported;

    if (in_super_table) {
        e->next = super_func_table;
        super_func_table = e;
        func_index_insert(super_func_buckets, e);
    } else {
        e->next = func_table;
        func_table = e;
        func_index_insert(func_buckets, e);
    }
}

void func_register(const char *name, ASTNode *def) {
    /* Una función normal definida en un Fire es privada del módulo. Su
     * nombre ya llega cualificado desde el parser (alias.nombre). */
    func_register_user_entry(name, def, false, current_import_prefix, false);
}

void func_register_global(const char *name, ASTNode *def) {
    if (current_import_prefix && current_import_prefix[0] != '\0') {
        /* En un Fire, `global` significa exportar la función a través del
         * alias del módulo; NO registrarla en el espacio global sin prefijo. */
        size_t size = strlen(current_import_prefix) + 1 + strlen(name) + 1;
        char *qualified = infernal_malloc(size);
        snprintf(qualified, size, "%s.%s", current_import_prefix, name);
        func_register_user_entry(qualified, def, false, current_import_prefix, true);
        free(qualified);
        return;
    }

    /* Fuera de un Fire se conserva el comportamiento de global function. */
    func_register_user_entry(name, def, true, NULL, true);
}

void func_register_builtin(const char *name, BuiltinFunc fn) {
    FuncObject *obj = infernal_malloc(sizeof(FuncObject));
    obj->kind = FUNC_BUILTIN;
    obj->builtin = fn;
    FuncEntry *e = infernal_malloc(sizeof(FuncEntry));
    e->name = infernal_strdup(name);
    e->obj = obj;
    e->module = NULL;
    e->exported = true;
    e->next = func_table;
    func_table = e;
    func_index_insert(func_buckets, e);
}

FuncObject *func_lookup(const char *name) {
    size_t bucket = func_name_hash(name) % FUNC_BUCKET_COUNT;
    for (FuncEntry *e = func_buckets[bucket]; e; e = e->hash_next)
        if (strcmp(e->name, name) == 0)
            return e->obj;
    for (FuncEntry *e = super_func_buckets[bucket]; e; e = e->hash_next)
        if (strcmp(e->name, name) == 0)
            return e->obj;
    return NULL;
}

static FuncEntry *func_entry_lookup(const char *name) {
    if (!name) return NULL;
    size_t bucket = func_name_hash(name) % FUNC_BUCKET_COUNT;
    for (FuncEntry *e = func_buckets[bucket]; e; e = e->hash_next)
        if (strcmp(e->name, name) == 0)
            return e;
    for (FuncEntry *e = super_func_buckets[bucket]; e; e = e->hash_next)
        if (strcmp(e->name, name) == 0)
            return e;
    return NULL;
}

FuncObject *func_lookup_call(const char *name, const char *caller_module) {
    if (!name || !name[0]) return NULL;

    /* Una llamada cualificada (Modulo.funcion) solo puede salir del módulo
     * si el destino está exportado. El propio módulo sí puede usar helpers
     * privados mediante su nombre cualificado. */
    if (strchr(name, '.')) {
        FuncEntry *entry = func_entry_lookup(name);
        if (!entry) return NULL;
        if (!entry->module) return entry->obj;
        if (entry->exported ||
            (caller_module && strcmp(entry->module, caller_module) == 0))
            return entry->obj;
        return NULL;
    }

    /* Dentro de un Fire, primero resolver nombres desnudos en el módulo
     * actual, lo que permite que sus helpers sigan siendo privados. */
    if (caller_module && caller_module[0]) {
        size_t size = strlen(caller_module) + 1 + strlen(name) + 1;
        char *qualified = infernal_malloc(size);
        snprintf(qualified, size, "%s.%s", caller_module, name);
        FuncEntry *entry = func_entry_lookup(qualified);
        free(qualified);
        if (entry && entry->module &&
            strcmp(entry->module, caller_module) == 0)
            return entry->obj;
    }

    /* Las funciones del script principal y los builtins no pertenecen a un
     * módulo, así que continúan visibles con su nombre habitual. */
    FuncEntry *entry = func_entry_lookup(name);
    if (entry && !entry->module) return entry->obj;
    return NULL;
}
