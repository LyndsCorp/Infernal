/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License, Lynds Corp., Aros Legendarios, David Baña Szymaniak.
 * Código fuente de Infernal: main.c
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
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

extern const char* get_metadata(const char *type);

// ─── Función auxiliar para liberar un chunk ──────────────────
void chunk_free(Chunk *ch) {
    if (!ch) return;
    free(ch->constants);
    for (int i = 0; i < ch->local_count; i++) free(ch->local_names[i]);
    free(ch->local_names);
    free(ch->code);
    free(ch);
}
// ─────────────────────────────────────────────────────────────────

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

    script_argc = argc;
    script_argv = argv;

    char *script_path = realpath(argv[1], NULL);
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

    current_source_file = argv[1];

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

    FILE *fp = fopen(argv[1], "r");
    if (!fp) {
        perror("Error al abrir script");
        free(script_dir);
        return 1;
    }

    if (!setjmp(exception_env)) {
        ts_init();
        tokenize_file(fp);
        fclose(fp);

        NodeList program = parse_block(NULL);

        // Compilar a bytecode
        Chunk *main_chunk = compile_program(&program);

        // Ejecutar en la VM
        Value result = vm_run(main_chunk);
        (void)result; // descartar

        // LIBERAR MEMORIA
        chunk_free(main_chunk);

        cleanup_embedded_temp_dir();
        free(script_dir);
        return 0;
    } else {
        fprintf(stderr, "%s\n", exception_msg);
        cleanup_embedded_temp_dir();
        free(script_dir);
        return 1;
    }
}
