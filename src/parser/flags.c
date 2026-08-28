/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: parser/flags.c
*/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include "flags.h"
#include "core/ast.h"
#include "lexer/lexer.h"
#include "runtime/error.h"
#include "runtime/globals.h"
#include "expression.h"

static bool is_valid_flag_name(const char *s) {
    if (!s || !(isalpha((unsigned char)s[0]) || s[0] == '_'))
        return false;
    for (const char *p = s + 1; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_'))
            return false;
    }
    return true;
}

static void add_flag_name(FlagSpec *spec, const char *name, int line) {
    for (int i = 0; i < spec->name_count; i++)
        if (strcmp(spec->names[i], name) == 0)
            error(line, "Flag o alias repetido: %s", name);
    char **names = realloc(spec->names, (size_t)(spec->name_count + 1) * sizeof(char *));
    if (!names) error(line, "Memoria insuficiente al registrar flag");
    spec->names = names;
    spec->names[spec->name_count++] = strdup(name);
}

/* --- Función auxiliar para parsear el nombre de un flag --- */
static char *parse_flag_name(int line) {
    char *name = strdup("");
    size_t len = 0;
    size_t cap = 1;

    const char *pieces[3];
    int piece_count = 0;
    if (ts_peek().type == TOK_MINUS) {
        Token tok = ts_advance();
        pieces[piece_count++] = tok.lexeme;
        if (ts_peek().type == TOK_MINUS) {
            tok = ts_advance();
            pieces[piece_count++] = tok.lexeme;
        }
        if (ts_peek().type != TOK_IDENT)
            error(line, "Se esperaba nombre del flag después de '-'/'--'");
        pieces[piece_count++] = ts_advance().lexeme;
    } else if (ts_peek().type == TOK_IDENT && is_valid_flag_name(ts_peek().lexeme)) {
        pieces[piece_count++] = ts_advance().lexeme;
    } else {
        error(line, "Se esperaba un flag (--nombre, -n o nombre)");
    }

    for (int i = 0; i < piece_count; i++) {
        size_t add = strlen(pieces[i]);
        if (len > SIZE_MAX - add - 1) {
            free(name);
            error(line, "Nombre de flag demasiado largo");
        }
        if (len + add + 1 > cap) {
            while (cap < len + add + 1) cap *= 2;
            char *tmp = realloc(name, cap);
            if (!tmp) { free(name); error(line, "Memoria insuficiente al construir flag"); }
            name = tmp;
        }
        memcpy(name + len, pieces[i], add);
        len += add;
        name[len] = '\0';
    }
    return name;
}

void parse_flag_body_tokens(Token **body_tokens, int *body_count, int already_consumed_brace) {
    int depth = 1;
    *body_tokens = NULL;
    *body_count = 0;
    int cap = 0;

    if (!already_consumed_brace) {
        if (ts_peek().type != TOK_LBRACE)
            error(ts_peek().line, "Se esperaba '{' para el bloque del flag");
        ts_advance(); // consumir el '{'
    }

    while (depth > 0 && ts.pos < ts.count) {
        Token t = ts_advance();
        if (t.type == TOK_LBRACE) depth++;
        else if (t.type == TOK_RBRACE) {
            depth--;
            if (depth == 0) break;
        }
        if (*body_count >= cap) {
            cap = cap == 0 ? 64 : cap * 2;
            *body_tokens = realloc(*body_tokens, cap * sizeof(Token));
        }
        (*body_tokens)[(*body_count)++] = t;
    }
    if (depth != 0) {
        error(ts.tokens[ts.pos-1].line, "No se encontró '}' que cierra el bloque del flag");
    }
}

ASTNode *parse_flags() {
    if (!ts_match(TOK_LPAREN)) error(ts_peek().line, "Se esperaba '(' después de 'flags'");
    ASTNode *mode_expr = parse_expression(0);
    if (!ts_match(TOK_COMMA)) error(ts_peek().line, "Se esperaba ',' después del modo");

    ASTNode *node = node_create(NODE_FLAGS, mode_expr->line);
    node->data.flags.mode = -1;

    if (mode_expr->kind == NODE_LITERAL && mode_expr->data.lit.type == TOK_INT) {
        int mode = mode_expr->data.lit.ival;
        if (mode < 0) {
            error(mode_expr->line, "El modo de flags no puede ser negativo");
        }
        if (mode >= MAX_FLAGS_MODES) {
            error(mode_expr->line, "Modo de flags demasiado grande (máximo %d)", MAX_FLAGS_MODES - 1);
        }

        if (mode == 0) {
            /* Modo 0: siempre permitido */
        } else {
            if (mode > 1 && !defined_flags_modes[mode - 1]) {
                error(mode_expr->line,
                      "No se puede definir flags modo %d sin haber definido el modo %d antes",
                      mode, mode - 1);
            }
            if (defined_flags_modes[mode]) {
                error(mode_expr->line,
                      "El modo %d de flags ya fue definido anteriormente", mode);
            }
            defined_flags_modes[mode] = 1;
        }
        node->data.flags.mode = mode;
    } else {
        error(mode_expr->line, "El modo de flags debe ser un número entero");
    }

    node->data.flags.specs = NULL;
    node->data.flags.spec_count = 0;
    int empty_count = 0;

    while (!ts_match(TOK_RPAREN)) {
        ts_skip_newlines();
        FlagSpec spec;
        memset(&spec, 0, sizeof(spec));
        spec.is_empty = false;
        spec.is_global = false;

        /* --- DETECTAR 'empty' ----------------------------------- */
        if (ts_peek().type == TOK_IDENT && strcmp(ts_peek().lexeme, "empty") == 0) {
            if (node->data.flags.mode == 0) {
                error(ts_peek().line, "'empty' solo se puede usar en modos > 0 (flags posicionales)");
            }
            if (++empty_count > 1) {
                error(ts_peek().line, "No puede haber más de un 'empty' en la misma definición de flags");
            }
            ts_advance();
            spec.is_empty = true;
            if (!ts_match(TOK_EQ)) {
                error(ts_peek().line, "Se esperaba '=' después de 'empty'");
            }
            TokenType t = ts_peek().type;
            if (!(t == TOK_INT || t == TOK_FLOAT || t == TOK_BOOL || t == TOK_STRING || t == TOK_LIST)) {
                error(ts_peek().line, "Se esperaba un tipo (int, float, bool, string, list) después de '='");
            }
            spec.vtype = ts_advance().type;
            if (ts_peek().type != TOK_IDENT) {
                error(ts_peek().line, "Se esperaba nombre de variable para el empty");
            }
            spec.var_name = strdup(ts_advance().lexeme);
            if (ts_peek().type == TOK_LBRACE) {
                ts_advance();
                parse_flag_body_tokens(&spec.body_tokens, &spec.body_count, 0);
            } else {
                spec.body_tokens = NULL;
                spec.body_count = 0;
            }
        } else if (ts_peek().type == TOK_STAR) {
            ts_advance();
            spec.catch_all = true;
            if (!ts_match(TOK_LBRACE)) error(ts_peek().line, "Se esperaba '{' después de '*'");
            parse_flag_body_tokens(&spec.body_tokens, &spec.body_count, 0);
        } else {
            /* --- FLAG NORMAL (con nombre) ---------------------- */
            Token t = ts_peek();
            char *name = NULL;
            char **aliases = NULL;
            int alias_count = 0;
            int has_block = 0;

            // Verificar si el token contiene '|' o '{'
            if (t.type == TOK_IDENT && (strchr(t.lexeme, '|') || strchr(t.lexeme, '{'))) {
                char *lex = strdup(t.lexeme);
                char *p = lex;
                // Extraer nombre (antes de '|' o '{')
                char *name_start = p;
                while (*p && *p != '|' && *p != '{') p++;
                if (p == name_start) {
                    free(lex);
                    error(t.line, "Nombre de flag vacío");
                }
                size_t name_len = p - name_start;
                name = strndup(name_start, name_len);

                // Procesar alias y detectar bloque
                while (*p) {
                    if (*p == '|') {
                        p++;
                        char *alias_start = p;
                        while (*p && *p != '|' && *p != '{') p++;
                        if (p > alias_start) {
                            char *alias = strndup(alias_start, p - alias_start);
                            aliases = realloc(aliases, (alias_count + 1) * sizeof(char*));
                            aliases[alias_count++] = alias;
                        }
                    } else if (*p == '{') {
                        has_block = 1;
                        p++;
                        break;
                    } else {
                        p++;
                    }
                }
                free(lex);
                ts_advance(); // consumir el token
            } else {
                name = parse_flag_name(t.line);
            }

            // Añadir el nombre y los alias al spec
            add_flag_name(&spec, name, t.line);
            free(name);
            for (int i = 0; i < alias_count; i++) {
                add_flag_name(&spec, aliases[i], t.line);
                free(aliases[i]);
            }
            free(aliases);

            // Procesar el '=' y el tipo/variable
            if (ts_match(TOK_EQ)) {
                bool is_global = false;
                TokenType peek = ts_peek().type;
                if (peek == TOK_GLOBAL || peek == TOK_LOCAL) {
                    if (peek == TOK_GLOBAL) is_global = true;
                    ts_advance();
                }
                TokenType ttype = ts_peek().type;
                if (ttype == TOK_INT || ttype == TOK_FLOAT || ttype == TOK_BOOL ||
                    ttype == TOK_STRING || ttype == TOK_LIST) {
                    spec.vtype = ts_advance().type;
                if (ts_peek().type != TOK_IDENT || !is_valid_flag_name(ts_peek().lexeme))
                    error(ts_peek().line, "Se esperaba nombre de variable para el flags");
                    spec.var_name = strdup(ts_advance().lexeme);
                spec.is_global = is_global;
                    } else {
                        error(ts_peek().line, "Se esperaba un tipo (int, float, bool, string, list) después de '='");
                    }
            }

            // Procesar el bloque (si no se ha procesado ya y existe)
            if (!has_block && ts_peek().type == TOK_LBRACE) {
                parse_flag_body_tokens(&spec.body_tokens, &spec.body_count, 0);
            } else if (has_block) {
                // Ya hemos consumido el '{' pegado, leer el bloque
                parse_flag_body_tokens(&spec.body_tokens, &spec.body_count, 1);
            } else {
                spec.body_tokens = NULL;
                spec.body_count = 0;
            }
        }

        node->data.flags.specs = realloc(node->data.flags.specs,
                                         (node->data.flags.spec_count+1)*sizeof(FlagSpec));
        node->data.flags.specs[node->data.flags.spec_count++] = spec;

        ts_skip_newlines();
        if (ts_peek().type == TOK_COMMA) { ts_advance(); ts_skip_newlines(); }
    }

    return node;
}
