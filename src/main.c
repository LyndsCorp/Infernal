/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: vm/main.c
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <stdbool.h>
#include "core/value.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "runtime/command.h"
#include "stdlib/builtins.h"
#include "vm/vm.h"
#include "vm/compiler.h"
#include "developer/debug.h"

extern const char* get_metadata(const char *type);

void chunk_free(Chunk *ch) {
    if (!ch) return;
    for (int i = 0; i < ch->const_count; i++) value_free(&ch->constants[i]);
    free(ch->constants);
    for (int i = 0; i < ch->local_count; i++) free(ch->local_names[i]);
    free(ch->local_names);
    free(ch->local_types);
    free(ch->code);
    free(ch);
}

static void cleanup_runtime_state(void) {
    while (current_scope && current_scope != global_scope) {
        Scope *parent = current_scope->parent;
        scope_free(current_scope);
        current_scope = parent;
    }
    if (global_scope) { scope_free(global_scope); global_scope = NULL; current_scope = NULL; }
    if (super_global_scope) { scope_free(super_global_scope); super_global_scope = NULL; }

    while (func_table) {
        FuncEntry *entry = func_table;
        FuncEntry *next = entry->next;
        FuncObject *obj = entry->obj;
        bool shared_obj = false;
        for (FuncEntry *scan = next; scan; scan = scan->next) {
            if (scan->obj == obj) {
                shared_obj = true;
                break;
            }
        }
        free(entry->name);
        if (!shared_obj && obj) {
            /* El Chunk ya fue liberado en vm_cleanup_state, así que no lo liberamos aquí */
            free(obj);
        }
        free(entry);
        func_table = next;
    }

    value_free(&return_value);
    if (infernal_shell) { free(infernal_shell); infernal_shell = NULL; }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        const char *welcome = get_metadata("WELCOME");
        if (welcome) printf("%s\n", welcome);
        else printf("Infernal: sin mensaje de bienvenida.\n");
        return 0;
    }

    if (strcmp(argv[1], "--version") == 0) {
        const char *version = get_metadata("VERSION");
        if (version) printf("%s", version);
        else printf("Infernal version desconocida\n");
        return 0;
    }

    if (strcmp(argv[1], "--edition") == 0) {
        const char *edition = get_metadata("EDITION");
        if (edition) printf("%s", edition);
        else printf("Infernal edición desconocida\n");
        return 0;
    }

    if (strcmp(argv[1], "--help") == 0) {
        const char *help = get_metadata("HELP");
        if (help) printf("%s\n", help);
        else printf("Ayuda no disponible.\n");
        return 0;
    }

    // --- Procesar argumentos: --shell y script --------------
    char *script_file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shell") == 0) {
            if (i + 1 < argc && argv[i+1][0] != '-') {
                // Con argumento
                if (infernal_shell) free(infernal_shell);
                infernal_shell = strdup(argv[i + 1]);
                DEBUG_INFO("Shell especificado por línea de comandos: %s", infernal_shell);
                i++; // saltar el valor
                continue;
            } else {
                // Sin argumento: mostrar info y salir
                show_shell_info();
                return 0;
            }
        }
        // El primer argumento que NO empieza con '-' se toma como script
        if (argv[i][0] != '-') {
            script_file = argv[i];
            break;
        }
    }

    if (!script_file) {
        fprintf(stderr, "Error: no se especificó ningún archivo de script.\n");
        return 1;
    }

    // Guardamos todos los argumentos para que el script los vea (incluyendo flags)
    script_argc = argc;
    script_argv = argv;

    char *script_path = realpath(script_file, NULL);
    if (script_path) {
        char *dir = strdup(script_path);
        char *last_slash = strrchr(dir, '/');
        if (last_slash) {
            *last_slash = '\0';
            set_embedded_tmp_dir(dir);
            script_dir = strdup(dir);
        } else {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                set_embedded_tmp_dir(cwd);
                script_dir = strdup(cwd);
            }
        }
        free(dir);
        free(script_path);
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            set_embedded_tmp_dir(cwd);
            script_dir = strdup(cwd);
        }
    }

    current_source_file = script_file;

    super_global_scope = scope_new(NULL, NULL);
    extern char **environ;
    for (char **env = environ; *env; env++) {
        if (strncmp(*env, "INFERNAL_VAR_", 13) == 0) {
            char *line = strdup(*env);
            char *name = line + 13;
            char *eq = strchr(name, '=');
            if (eq) {
                *eq = '\0';
                char *val = eq + 1;
                if (val[0] == 'i' && val[1] == ':') {
                    scope_define(super_global_scope, name, TOK_INT, val_int(atoi(val + 2)));
                } else if (val[0] == 'f' && val[1] == ':') {
                    scope_define(super_global_scope, name, TOK_FLOAT, val_float(atof(val + 2)));
                } else if (val[0] == 'b' && val[1] == ':') {
                    bool b = (strcmp(val + 2, "true") == 0);
                    scope_define(super_global_scope, name, TOK_BOOL, val_bool(b));
                } else if (val[0] == 's' && val[1] == ':') {
                    scope_define(super_global_scope, name, TOK_STRING, val_string(val + 2));
                } else {
                    scope_define(super_global_scope, name, TOK_STRING, val_string(val));
                }
            }
            free(line);
        }
    }

    global_scope = scope_new(super_global_scope, NULL);
    current_scope = global_scope;

    register_all_builtins();

    // --- Cargar configuración de shell (solo si no se especificó --shell) --
    if (!infernal_shell) {
        load_infernal_config();
    } else {
        DEBUG_INFO("Shell ya configurado por --shell, no se cargan configuraciones");
    }

    FILE *fp = fopen(script_file, "r");
    if (!fp) {
        perror("Error al abrir script");
        free(script_dir);
        return 1;
    }

    NodeList program = {NULL, 0, 0};
    Chunk *main_chunk = NULL;
    bool file_open = true;
    bool parse_complete = false;

    if (!setjmp(exception_env)) {
        ts_init();
        tokenize_file(fp);
        fclose(fp);
        file_open = false;

        program = parse_block(NULL);
        parse_complete = true;

        main_chunk = compile_program(&program);
        Value result = vm_run(main_chunk);
        value_free(&result);

        chunk_free(main_chunk);
        main_chunk = NULL;
        vm_cleanup_state();
        nodelist_free(&program);
        if (ts.tokens) {
            for (int i = 0; i < ts.count; i++) free(ts.tokens[i].lexeme);
            free(ts.tokens); ts.tokens = NULL; ts.count = ts.cap = ts.pos = 0;
        }
        for (int i = 0; i < source_line_count; i++) free(source_lines[i]);
        free(source_lines); source_lines = NULL; source_line_count = 0;
        cleanup_runtime_state();

        cleanup_embedded_temp_dir();
        free(script_dir);
        return 0;
    } else {
        if (file_open) fclose(fp);
        if (main_chunk) { chunk_free(main_chunk); main_chunk = NULL; }
        vm_cleanup_state();
        if (!parse_complete) {
            compiler_cleanup_on_error();
            parser_cleanup_on_error();
            program = (NodeList){NULL, 0, 0};
        } else {
            nodelist_free(&program);
        }
        if (ts.tokens) {
            for (int i = 0; i < ts.count; i++) free(ts.tokens[i].lexeme);
            free(ts.tokens); ts.tokens = NULL; ts.count = ts.cap = ts.pos = 0;
        }
        for (int i = 0; i < source_line_count; i++) free(source_lines[i]);
        free(source_lines); source_lines = NULL; source_line_count = 0;
        cleanup_runtime_state();
        fprintf(stderr, "%s\n", exception_msg);
        cleanup_embedded_temp_dir();
        free(script_dir);
        return 1;
    }
}
