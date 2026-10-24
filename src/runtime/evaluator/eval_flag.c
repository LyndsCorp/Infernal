/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/eval_flag.c
*/

#include "eval_flag.h"
#include "evaluator.h"
#include "helpers.h"
#include "core/value.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>

static char *clean_flag_value(const char *val_str) {
    if (!val_str) return strdup("");
    const char *start = val_str;
    const char *end = val_str + strlen(val_str);
    if ((val_str[0] == '"' || val_str[0] == '\'') && end > start + 1) {
        char quote = val_str[0];
        if (end[-1] == quote) { start++; end--; }
    }
    size_t len = (size_t)(end - start);
    char *cleaned = malloc(len + 1);
    if (!cleaned) return NULL;
    memcpy(cleaned, start, len);
    cleaned[len] = '\0';
    return cleaned;
}

/* --- Ejecutar un spec individual (ya existente) --- */
void exec_flag_spec_impl(FlagSpec *spec) {
    if (spec->body_count == 0) return;

    TokenStream saved_ts = ts;
    Token *saved_body_tokens = spec->body_tokens;
    int saved_body_count = spec->body_count;

    ts.tokens = spec->body_tokens;
    ts.count  = spec->body_count;
    ts.pos    = 0;

    /* El spec cede temporalmente la propiedad del array de tokens
     * mientras se ejecuta su body. Si el body llama a exit(), cleanup_all()
     * libera ts.tokens (== saved_body_tokens). Si spec->body_tokens no
     * fuese NULL aquí, free_flag_spec() lo volvería a liberar al vaciar
     * el AST y habría un doble free ("double free or corruption").
     *
     * La propiedad se restaura en los dos caminos que NO terminan el
     * proceso: el retorno normal y el longjmp desde error(). Sólo el
     * camino de exit() no la restaura, lo cual es correcto porque el
     * proceso muere justo después. */
    spec->body_tokens = NULL;
    spec->body_count = 0;

    jmp_buf saved_env;
    memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
    int saved_raised = exception_raised;

    if (setjmp(exception_env) != 0) {
        /* Error via longjmp: recuperar la propiedad antes de re-lanzar,
         * porque el programa sigue vivo y el spec sigue en el AST. */
        spec->body_tokens = saved_body_tokens;
        spec->body_count = saved_body_count;
        ts = saved_ts;
        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
        exception_raised = saved_raised;
        longjmp(exception_env, 1);
    }

    NodeList flag_body = parse_block(NULL);
    exec_block(&flag_body);
    nodelist_free(&flag_body);

    /* Camino normal: recuperar la propiedad. */
    spec->body_tokens = saved_body_tokens;
    spec->body_count = saved_body_count;
    ts = saved_ts;

    memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
    exception_raised = saved_raised;
}

/* --- Convierte el string de valor y lo asigna a la variable del spec ---
 *
 * Si el spec no tiene vtype/var_name (p. ej. --help|-h), no hace nada. */
static void bind_flag_value(FlagSpec *spec, const char *val_str, int line) {
    if (!spec->vtype || !spec->var_name) return;

    char *cleaned = clean_flag_value(val_str);
    if (!cleaned) error(line, "Memoria insuficiente para valor de flag");
    if (spec->vtype == TOK_FLOAT) {
        char *coma = strchr(cleaned, ',');
        if (coma) *coma = '.';
    }
    Value v;
    switch (spec->vtype) {
        case TOK_INT:    v = val_int(atoi(cleaned)); break;
        case TOK_FLOAT:  v = val_float(atof(cleaned)); break;
        case TOK_BOOL:   v = val_bool(strcmp(cleaned, "0") != 0 &&
                                      strlen(cleaned) > 0); break;
        case TOK_STRING: v = val_string(cleaned); break;
        default:         v = val_string(cleaned);
    }
    free(cleaned);

    if (spec->is_global) {
        scope_define(super_global_scope, spec->var_name, spec->vtype, v);
    } else {
        scope_define(global_scope, spec->var_name, spec->vtype, v);
    }
}

/* Actualiza o crea la variable '_' en current_scope. Si ya existe, la
 * reutiliza en vez de crear una entrada nueva; así no acumulamos entradas
 * '_' duplicadas cuando catch_all se ejecuta varias veces (p. ej. en un
 * bucle). */
static void set_catch_all_var(const char *value) {
    VarEntry *existing = scope_find_current(current_scope, "_");
    if (existing) {
        Value new_val = val_string(value);
        value_free(&existing->value);
        existing->value = new_val;
        existing->vtype = TOK_STRING;
    } else {
        scope_define(current_scope, "_", 0, val_string(value));
    }
}

/* --- Ejecutar un nodo flags completo (orquestador) ---
 *
 * Modelo unificado para todos los modos (0 y > 0):
 *
 *   - Cualquier spec con names[] se matchea por nombre si el argumento
 *     coincide (--help, -h, o, o="x", o "x").
 *
 *   - Cualquier spec con var_name o is_empty es slot posicional. Los
 *     slots se llenan en orden de aparición en el código fuente, con
 *     el siguiente argumento que no empiece por '-' (o sea '-').
 *
 *   - Los specs con body pero sin var (--help|-h {...}) NO son slot
 *     posicional: solo se ejecutan cuando su nombre coincide. Esto
 *     evita que `prog.fire gata` dispare `--help`.
 *
 *   - Clúster corto `-abc` se expande a `-a -b -c`.
 *
 *   - catch_all (`*`) recoge flags desconocidos y posicionales sin slot.
 */
void exec_flags(ASTNode *node) {
    FlagSpec *specs = node->data.flags.specs;
    int spec_count = node->data.flags.spec_count;

    bool *handled = calloc(script_argc > 0 ? (size_t)script_argc : 1, sizeof(bool));
    if (!handled) error(node->line, "Memoria insuficiente al procesar flags");

    /* Error via longjmp dentro de un body: liberar `handled` antes de
     * re-lanzar. */
    jmp_buf saved_env;
    memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
    int saved_raised = exception_raised;
    if (setjmp(exception_env) != 0) {
        free(handled);
        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
        exception_raised = saved_raised;
        longjmp(exception_env, 1);
    }

    FlagSpec *catch_all = NULL;
    for (int s = 0; s < spec_count; s++) {
        if (specs[s].catch_all) { catch_all = &specs[s]; break; }
    }

    /* Siguiente spec que puede llenarse posicionalmente. Avanza solo
     * cuando un slot se consume. */
    int slot_idx = 0;

    for (int a = flags_arg_index; a < script_argc; a++) {
        if (handled[a]) continue;
        char *arg = script_argv[a];
        if (!arg) continue;

        /* ──────────────────────────────────────────────────────────
         * 1. Match por nombre.
         *
         * Se separa `nombre` de `=valor` (si lo hay) y se busca un
         * spec con ese nombre entre sus aliases.
         * ────────────────────────────────────────────────────────── */
        char *arg_dup = strdup(arg);
        if (!arg_dup) error(node->line, "Memoria insuficiente al procesar flags");
        char *eq_pos = strchr(arg_dup, '=');
        if (eq_pos) *eq_pos = '\0';

        bool matched = false;
        for (int s = 0; s < spec_count && !matched; s++) {
            FlagSpec *spec = &specs[s];
            if (spec->catch_all) continue;
            for (int n = 0; n < spec->name_count; n++) {
                if (strcmp(arg_dup, spec->names[n]) != 0) continue;

                if (spec->var_name) {
                    if (eq_pos) {
                        bind_flag_value(spec, eq_pos + 1, node->line);
                        handled[a] = true;
                    } else if (a + 1 < script_argc && !handled[a + 1]) {
                        a++;
                        handled[a] = true;
                        bind_flag_value(spec, script_argv[a], node->line);
                    } else {
                        free(arg_dup);
                        error(node->line,
                              "Falta el valor para el flag '%s'", arg);
                    }
                } else {
                    /* Sin var: solo body (--help|-h). */
                    handled[a] = true;
                }

                if (spec->body_count > 0) {
                    exec_flag_spec_impl(spec);
                }
                matched = true;
                break;
            }
        }
        free(arg_dup);
        if (matched) continue;

        /* ──────────────────────────────────────────────────────────
         * 2. Clúster corto -abc -> -a -b -c.
         * ────────────────────────────────────────────────────────── */
        if (arg[0] == '-' && arg[1] != '-' && arg[1] != '\0' && arg[2] != '\0') {
            for (int c = 1; arg[c]; c++) {
                char sn[3] = { '-', arg[c], '\0' };
                bool found = false;
                for (int s = 0; s < spec_count; s++) {
                    FlagSpec *spec = &specs[s];
                    if (spec->catch_all) continue;
                    for (int n = 0; n < spec->name_count; n++) {
                        if (strcmp(sn, spec->names[n]) == 0) {
                            if (spec->body_count > 0) {
                                exec_flag_spec_impl(spec);
                            }
                            found = true;
                            break;
                        }
                    }
                    if (found) break;
                }
                if (!found && catch_all) {
                    set_catch_all_var(sn);
                    exec_flag_spec_impl(catch_all);
                }
            }
            handled[a] = true;
            continue;
        }

        /* ──────────────────────────────────────────────────────────
         * 3. Posicional: llenar siguiente slot.
         * ────────────────────────────────────────────────────────── */
        if (arg[0] != '-' || strcmp(arg, "-") == 0) {
            FlagSpec *slot = NULL;
            while (slot_idx < spec_count) {
                FlagSpec *candidate = &specs[slot_idx];
                slot_idx++;
                if (candidate->catch_all) continue;
                if (candidate->is_empty || candidate->var_name) {
                    slot = candidate;
                    break;
                }
            }

            if (slot) {
                if (slot->var_name) {
                    bind_flag_value(slot, arg, node->line);
                }
                if (slot->body_count > 0) {
                    exec_flag_spec_impl(slot);
                }
                handled[a] = true;
                continue;
            }

            if (catch_all) {
                set_catch_all_var(arg);
                exec_flag_spec_impl(catch_all);
                handled[a] = true;
                continue;
            }
            error(node->line,
                  "Argumento posicional inesperado: '%s'.\n"
                  "    No hay ningún 'arg'/'empty' disponible para consumirlo.",
                  arg);
        }

        /* ──────────────────────────────────────────────────────────
         * 4. Flag desconocido.
         * ────────────────────────────────────────────────────────── */
        if (catch_all) {
            set_catch_all_var(arg);
            exec_flag_spec_impl(catch_all);
            handled[a] = true;
            continue;
        }
        error(node->line,
              "Flag desconocido: '%s'.\n"
              "    Usa --help si está disponible para ver las opciones.",
              arg);
    }

    flags_arg_index = script_argc;

    free(handled);
    memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
    exception_raised = saved_raised;
}

