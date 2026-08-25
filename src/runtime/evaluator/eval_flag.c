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
    ts.tokens = spec->body_tokens;
    ts.count = spec->body_count;
    ts.pos = 0;
    NodeList flag_body = parse_block(NULL);
    exec_block(&flag_body);
    ts = saved_ts;
}

/* --- Ejecutar un nodo flags completo (orquestador) --- */
void exec_flags(ASTNode *node) {
    int mode = node->data.flags.mode;
    bool *handled = calloc(script_argc, sizeof(bool));
    FlagSpec *catch_all = NULL;
    for (int s = 0; s < node->data.flags.spec_count; s++) {
        if (node->data.flags.specs[s].catch_all) {
            catch_all = &node->data.flags.specs[s];
            break;
        }
    }

    int total_matched = 0;

    if (mode > 0) {
        int arg_idx = flags_arg_index;
        int consumed = 0;
        for (int s = 0; s < node->data.flags.spec_count; s++) {
            FlagSpec *spec = &node->data.flags.specs[s];
            if (spec->catch_all) continue;
            if (arg_idx < script_argc) {
                char *val_str = script_argv[arg_idx];
                if (spec->vtype && spec->var_name) {
                    char *cleaned = clean_flag_value(val_str);
                    if (!cleaned) error(node->line, "Memoria insuficiente para valor de flag");
                    if (spec->vtype == TOK_FLOAT) {
                        char *coma = strchr(cleaned, ',');
                        if (coma) *coma = '.';
                    }
                    Value v;
                    switch (spec->vtype) {
                        case TOK_INT: v = val_int(atoi(cleaned)); break;
                        case TOK_FLOAT: v = val_float(atof(cleaned)); break;
                        case TOK_BOOL: v = val_bool(strcmp(cleaned,"0")!=0 && strlen(cleaned)>0); break;
                        case TOK_STRING: v = val_string(cleaned); break;
                        default: v = val_string(cleaned);
                    }
                    free(cleaned);
                    if (spec->is_global) {
                        scope_define(super_global_scope, spec->var_name, spec->vtype, v);
                    } else {
                        scope_define(global_scope, spec->var_name, spec->vtype, v);
                    }
                }
                handled[arg_idx] = true;
                total_matched++;
                arg_idx++;
                consumed++;
            }
            if (spec->body_count > 0) {
                exec_flag_spec_impl(spec);
            }
        }
        flags_arg_index = arg_idx;

        if (catch_all) {
            for (int a = 2; a < script_argc; a++)
                if (!handled[a]) {
                    scope_define(current_scope, "_", 0, val_string(script_argv[a]));
                    exec_flag_spec_impl(catch_all);
                    total_matched++;
                }
        }
    } else {
        for (int a = 2; a < script_argc; a++) {
            char *arg = script_argv[a];
            char *arg_dup = strdup(arg);
            char *eq_pos = strchr(arg_dup, '=');
            if (eq_pos) *eq_pos = '\0';
            bool matched = false;
            for (int s = 0; s < node->data.flags.spec_count; s++) {
                FlagSpec *spec = &node->data.flags.specs[s];
                if (spec->catch_all) continue;
                for (int n = 0; n < spec->name_count; n++) {
                    if (strcmp(arg_dup, spec->names[n]) == 0) {
                        if (spec->vtype && spec->var_name) {
                            if (!eq_pos && a + 1 >= script_argc)
                                error(node->line, "Falta el valor para el flag '%s'", arg_dup);
                            char *val_str = eq_pos ? eq_pos + 1 : script_argv[++a];
                    char *cleaned = clean_flag_value(val_str);
                    if (!cleaned) error(node->line, "Memoria insuficiente para valor de flag");
                            if (spec->vtype == TOK_FLOAT) {
                                char *coma = strchr(cleaned, ',');
                                if (coma) *coma = '.';
                            }
                            Value v;
                            switch (spec->vtype) {
                                case TOK_INT: v = val_int(atoi(cleaned)); break;
                                case TOK_FLOAT: v = val_float(atof(cleaned)); break;
                                case TOK_BOOL: v = val_bool(strcmp(cleaned,"0")!=0 && strlen(cleaned)>0); break;
                                case TOK_STRING: v = val_string(cleaned); break;
                                default: v = val_string(cleaned);
                            }
                            if (spec->is_global) {
                                scope_define(super_global_scope, spec->var_name, spec->vtype, v);
                            } else {
                                scope_define(global_scope, spec->var_name, spec->vtype, v);
                            }
                        }
                        if (spec->body_count > 0) {
                            exec_flag_spec_impl(spec);
                        }
                        handled[a] = true;
                        matched = true;
                        total_matched++;
                        break;
                    }
                }
                if (matched) break;
            }
            if (!matched && arg_dup[0] == '-' && arg_dup[1] != '-' && strlen(arg_dup) > 2) {
                for (int c = 1; arg_dup[c]; c++) {
                    char sn[3] = {'-', arg_dup[c], '\0'};
                    bool found = false;
                    for (int s = 0; s < node->data.flags.spec_count; s++) {
                        FlagSpec *spec = &node->data.flags.specs[s];
                        if (spec->catch_all) continue;
                        for (int n = 0; n < spec->name_count; n++) {
                            if (strcmp(sn, spec->names[n]) == 0) {
                                if (spec->body_count > 0) {
                                    exec_flag_spec_impl(spec);
                                }
                                found = true;
                                total_matched++;
                                break;
                            }
                        }
                        if (found) break;
                    }
                    if (!found && catch_all) {
                        scope_define(current_scope, "_", 0, val_string(sn));
                        exec_flag_spec_impl(catch_all);
                        total_matched++;
                    }
                }
                handled[a] = true;
            } else if (!matched && catch_all) {
                scope_define(current_scope, "_", 0, val_string(arg));
                exec_flag_spec_impl(catch_all);
                handled[a] = true;
                total_matched++;
            }
            free(arg_dup);
        }
    }
    if (total_matched == 0 && catch_all != NULL) exec_flag_spec_impl(catch_all);
    free(handled);
}
