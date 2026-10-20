/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: main.c
*/

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <stdbool.h>
#include <signal.h>
#include "core/value.h"
#include "core/ast.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "runtime/command.h"
#include "runtime/lava.h"
#include "stdlib/builtins.h"
#include "developer/debug.h"
#include "runtime/constants.h"
#include "runtime/evaluator/evaluator.h"

extern const char* get_metadata(const char *type);

static void infernal_internal_signal(int sig) {
    static const char msg[] =
    "Error interno de Infernal: se produjo un fallo durante la ejecución y el proceso no pudo continuar.\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);

    /* SIGABRT lo levanta glibc desde abort() cuando detecta
     * corrupción del heap: double free, free() inválido, overflow
     * de tcache, etc. Salimos con 42 para distinguirlo desde fuera. */
    if (sig == SIGABRT) {
        _exit(42);
    }
    _exit(1);
}

static void install_internal_error_handlers(void) {
    #ifndef DEBUG
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = infernal_internal_signal;
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    #endif
}

static bool cleanup_done = false;

static void cleanup_runtime_state(void);

static void cleanup_all(void) {
    if (cleanup_done) return;
    cleanup_done = true;

    if (ts.tokens) {
        for (int i = 0; i < ts.count; i++) free(ts.tokens[i].lexeme);
        free(ts.tokens);
        ts.tokens = NULL;
        ts.count = ts.cap = ts.pos = 0;
    }
    if (source_lines) {
        for (int i = 0; i < source_line_count; i++) free(source_lines[i]);
        free(source_lines);
        source_lines = NULL;
        source_line_count = 0;
    }
    ast_free_all();
    cleanup_runtime_state();
    cleanup_embedded_temp_dir();
    free(script_dir);
    script_dir = NULL;
}

static void cleanup_runtime_state(void) {
    lava_cleanup();
    constants_cleanup();
    scope_free_all();
    current_scope = NULL;
    global_scope = NULL;
    super_global_scope = NULL;

    while (func_table) {
        FuncEntry *entry = func_table;
        func_table = entry->next;
        FuncObject *obj = entry->obj;
        if (obj) {
            free(obj);
        }
        free(entry->name);
        free(entry);
    }

    // liberar la tabla superglobal de funciones
    while (super_func_table) {
        FuncEntry *entry = super_func_table;
        super_func_table = entry->next;
        FuncObject *obj = entry->obj;
        if (obj) {
            free(obj);
        }
        free(entry->name);
        free(entry);
    }

    value_free(&return_value);
    if (infernal_shell) { free(infernal_shell); infernal_shell = NULL; }
}

int main(int argc, char **argv) {
    install_internal_error_handlers();
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

    // --- Procesar argumentos: --shell, -c y script ----------
    char *script_file = NULL;
    const char *code_string = NULL;   // código inline pasado con -c
    int code_start_idx = -1;          // posición de "-c" en argv

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

        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -c requiere un argumento con el código a ejecutar.\n");
                fprintf(stderr, "Uso: %s -c \"codigo\" [args...]\n", argv[0]);
                return 1;
            }
            code_string = argv[i + 1];
            code_start_idx = i;
            // Los argumentos restantes (i+2 .. argc-1) son los args del script.
            // No escaneamos más: a partir de aquí tratamos el resto tal cual.
            break;
        }

        // El primer argumento que NO empieza con '-' se toma como script
        if (argv[i][0] != '-') {
            script_file = argv[i];
            break;
        }
    }

    if (!script_file && !code_string) {
        fprintf(stderr, "Error: no se especificó ningún archivo de script ni código con -c.\n");
        fprintf(stderr, "Uso: %s [opciones] archivo.fire [args...]\n", argv[0]);
        fprintf(stderr, "     %s [opciones] -c \"codigo\"   [args...]\n", argv[0]);
        return 1;
    }

    // --- Construir script_argc/script_argv ------------------
    //
    // En modo normal:     script_argv = argv, script_argc = argc
    //                     (argv[0]=intérprete, argv[1]=script, argv[2+]=args)
    //
    // En modo -c:         fabricamos un argv sintético para que:
    //                       script_argv[0] = intérprete
    //                       script_argv[1] = "<inline>"   (marcador del "script")
    //                       script_argv[2+] = args del usuario
    //                     con lo cual flags_arg_index = 2 sigue siendo correcto
    //                     y `flags(...)` funciona igual que en modo archivo.
    if (code_string) {
        int trailing = argc - code_start_idx - 2;
        if (trailing < 0) trailing = 0;
        script_argc = 2 + trailing;
        script_argv = malloc((size_t)(script_argc + 1) * sizeof(char *));
        if (!script_argv) {
            fprintf(stderr, "Error: memoria insuficiente al preparar argv inline.\n");
            return 1;
        }
        script_argv[0] = argv[0];
        script_argv[1] = (char *)"<inline>";
        for (int k = 0; k < trailing; k++) {
            script_argv[2 + k] = argv[code_start_idx + 2 + k];
        }
        script_argv[script_argc] = NULL;
    } else {
        script_argc = argc;
        script_argv = argv;
    }

    // --- Resolver directorio de trabajo --------------------
    //
    // En modo archivo: el directorio del propio script (para rutas
    // relativas de `execute`, `tofile`, `fromfile`, etc.).
    // En modo -c: no hay script, usamos el cwd.
    if (script_file) {
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
    } else {
        // modo -c
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            set_embedded_tmp_dir(cwd);
            script_dir = strdup(cwd);
        }
        current_source_file = (char *)"<command-line>";
    }

    super_global_scope = scope_new(NULL, NULL);
    if (!infernal_shell)
        load_infernal_config();
    register_all_constants();

    extern char **environ;
    for (char **env = environ; *env; env++) {
        if (strncmp(*env, "INFERNAL_VAR_", 13) == 0) {
            char *line = strdup(*env);
            char *name = line + 13;
            if (constants_is_reserved(name)) {
                free(line);
                continue;
            }
            char *eq = strchr(name, '=');
            if (eq) {
                *eq = '\0';
                char *val = eq + 1;
                if (val[0] == 'i' && val[1] == ':') {
                    char *end = NULL;
                    errno = 0;
                    long parsed = strtol(val + 2, &end, 10);
                    if (errno != 0 || end == val + 2 || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX) {
                        fprintf(stderr, "Valor inválido para INFERNAL_VAR_%s: se esperaba int en rango %d..%d\n",
                                name, INT_MIN, INT_MAX);
                        free(line);
                        continue;
                    }
                    scope_define(super_global_scope, name, TOK_INT, val_int((int)parsed));
                } else if (val[0] == 'f' && val[1] == ':') {
                    char *end = NULL;
                    errno = 0;
                    double parsed = strtod(val + 2, &end);
                    if (errno != 0 || end == val + 2 || *end != '\0') {
                        fprintf(stderr, "Valor inválido para INFERNAL_VAR_%s: se esperaba float\n", name);
                        free(line);
                        continue;
                    }
                    scope_define(super_global_scope, name, TOK_FLOAT, val_float(parsed));
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
    atexit(cleanup_all);

    register_all_builtins();

    /* ~/.infernalrc ya no selecciona el shell: _INFERNAL_SHELL es la fuente de verdad. */
    DEBUG_INFO("Shell efectivo de Infernal: %s", infernal_shell);

    // --- Abrir archivo (solo si no es modo -c) ------------
    FILE *volatile fp = NULL;
    if (!code_string) {
        fp = fopen(script_file, "r");
        if (!fp) {
            perror("Error al abrir script");
            cleanup_all();
            return 1;
        }
    }
    ts_init();

    NodeList program = {NULL, 0, 0};

    if (!setjmp(exception_env)) {
        if (code_string) {
            tokenize_buffer(code_string, strlen(code_string));
        } else {
            tokenize_file((FILE *)fp);
            fclose((FILE *)fp);
            fp = NULL;
        }

        program = parse_block(NULL);

        DEBUG_INFO("main: parse_block devolvió %d sentencias (stmts=%p)",
                   program.count, (void*)program.stmts);

        /* Ejecutar directamente con el evaluador (más estable que el compilador) */
        if (program.count > 0 && program.stmts != NULL) {
            exec_block(&program);
        } else {
            fprintf(stderr, "Advertencia: el script no contiene sentencias válidas\n");
        }

        nodelist_free(&program);
        cleanup_all();
        return 0;
    } else {
        /* Cerrar el archivo si la tokenización no llegó a cerrarlo.
         * `fp` es volatile precisamente para que este valor sea fiable
         * después del longjmp. */
        if (fp) { fclose((FILE *)fp); fp = NULL; }

        /* No usamos `program` aquí: su valor es indeterminado tras el
         * longjmp. ast_free_all() ya se encarga de liberar todo el AST
         * registrado, así como los NodeList huérfanos. */
        parser_cleanup_on_error();      /* = ast_free_all() */
        cleanup_all();
        fprintf(stderr, "%s\n", exception_msg);
        return 1;
    }
}
