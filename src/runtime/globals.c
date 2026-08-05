/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License, Lynds Corp., Aros Legendarios, David Baña Szymaniak.
 * Código fuente de Infernal: runtime/globals.c
*/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include "globals.h"
#include "developer/debug.h"

Scope *global_scope = NULL;
Scope *super_global_scope = NULL;
Scope *current_scope = NULL;

FuncEntry *func_table = NULL;
char *current_import_prefix = NULL;
int max_loop_iterations = 10000;

jmp_buf exception_env;
int exception_raised = 0;
char exception_msg[512];
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
int flags_arg_index = 2;   /* <-- NUEVO: primer argumento después del script */

/* ─── Shell configurado ───────────────────────────────────── */
char *infernal_shell = NULL;

/* ─── Función auxiliar para parsear archivo de configuración ── */
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
                    *shell_ptr = strdup(p);
                    fclose(fp);
                    return 1;
                }
            }
        }
    }
    fclose(fp);
    return 0;
}

/* ─── Carga de configuración de shell ────────────────────── */
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
        infernal_shell = strdup("/bin/sh");
        DEBUG_INFO("Usando shell por defecto: /bin/sh");
    } else {
        DEBUG_INFO("Shell configurado: %s", infernal_shell);
    }
}

/* ─── Mostrar información de configuración de shell ──────── */
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

void func_register(const char *name, ASTNode *def) {
    FuncObject *obj = malloc(sizeof(FuncObject));
    obj->kind = FUNC_USER;
    obj->def = def;
    obj->code = NULL;
    FuncEntry *e = malloc(sizeof(FuncEntry));
    e->name = strdup(name);
    e->obj = obj;
    e->next = func_table;
    func_table = e;
}

void func_register_builtin(const char *name, BuiltinFunc fn) {
    FuncObject *obj = malloc(sizeof(FuncObject));
    obj->kind = FUNC_BUILTIN;
    obj->builtin = fn;
    obj->code = NULL;
    FuncEntry *e = malloc(sizeof(FuncEntry));
    e->name = strdup(name);
    e->obj = obj;
    e->next = func_table;
    func_table = e;
}

FuncObject *func_lookup(const char *name) {
    for (FuncEntry *e = func_table; e; e = e->next)
        if (strcmp(e->name, name) == 0)
            return e->obj;
    return NULL;
}
