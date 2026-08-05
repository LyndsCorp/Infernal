/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License, Lynds Corp., Aros Legendarios, David Baña Szymaniak.
 * Código fuente de Infernal: parser/flags.c
*/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "flags.h"
#include "core/ast.h"
#include "lexer/lexer.h"
#include "runtime/error.h"
#include "expression.h"

/* ─── Control de definiciones de flags en modo 1 ── */
static int flags_mode1_line = 0;   /* 0 = no definido aún */

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

/* ─── Función auxiliar para parsear el nombre de un flag ─── */
static char *parse_flag_name(int line) {
    char name_buf[128] = "";
    if (ts_peek().type == TOK_MINUS) {
        Token tok = ts_advance();
        strcat(name_buf, tok.lexeme);                  // "-"
        if (ts_peek().type == TOK_MINUS) {
            tok = ts_advance();
            strcat(name_buf, tok.lexeme);              // "--"
        }
        if (ts_peek().type != TOK_IDENT) {
            error(line, "Se esperaba nombre del flag después de '-'/'--'");
        }
        strcat(name_buf, ts_advance().lexeme);
    } else if (ts_peek().type == TOK_IDENT && is_valid_flag_name(ts_peek().lexeme)) {
        strcat(name_buf, ts_advance().lexeme);
    } else {
        error(line, "Se esperaba un flag (--nombre, -n o nombre)");
    }
    return strdup(name_buf);
}

void parse_flag_body_tokens(Token **body_tokens, int *body_count) {
    int depth = 1;
    *body_tokens = NULL;
    *body_count = 0;
    int cap = 0;

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
    node->data.flags.mode = 0;
    if (mode_expr->kind == NODE_LITERAL && mode_expr->data.lit.type == TOK_INT &&
        (mode_expr->data.lit.ival == 0 || mode_expr->data.lit.ival == 1))
        node->data.flags.mode = mode_expr->data.lit.ival;
    else
        error(mode_expr->line, "El modo de flags debe ser 0 o 1");

    /* ─── Verificación de duplicidad para modo 1 ─── */
    if (node->data.flags.mode == 1 && flags_mode1_line != 0) {
        error(node->line,
              "Ya existe una definición de flags en modo 1 en la línea %d. "
              "Usa ese flags en lugar de crear uno nuevo.", flags_mode1_line);
    }

    node->data.flags.specs = NULL;
    node->data.flags.spec_count = 0;
    int empty_count = 0;

    while (!ts_match(TOK_RPAREN)) {
        ts_skip_newlines();
        FlagSpec spec;
        memset(&spec, 0, sizeof(spec));
        spec.is_empty = false;

        /* ─── DETECTAR 'empty' ─────────────────────────────────── */
        if (ts_peek().type == TOK_IDENT && strcmp(ts_peek().lexeme, "empty") == 0) {
            if (node->data.flags.mode != 1) {
                error(ts_peek().line, "'empty' solo se puede usar en modo 1 (flags posicionales)");
            }
            if (++empty_count > 1) {
                error(ts_peek().line, "No puede haber más de un 'empty' en la misma definición de flags");
            }
            ts_advance();  // consumir 'empty'
            spec.is_empty = true;
            /* Se espera '= tipo var' */
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
            /* Bloque opcional */
            if (ts_peek().type == TOK_LBRACE) {
                ts_advance();
                parse_flag_body_tokens(&spec.body_tokens, &spec.body_count);
            } else {
                spec.body_tokens = NULL;
                spec.body_count = 0;
            }
        } else if (ts_peek().type == TOK_STAR) {
            ts_advance();
            spec.catch_all = true;
            if (!ts_match(TOK_LBRACE)) error(ts_peek().line, "Se esperaba '{' después de '*'");
            parse_flag_body_tokens(&spec.body_tokens, &spec.body_count);
        } else {
            /* ─── FLAG NORMAL (con nombre) ────────────────────── */
            char *name = parse_flag_name(ts_peek().line);
            add_flag_name(&spec, name, ts_peek().line);
            free(name);

            while (ts_peek().type == TOK_PIPE) {
                ts_advance();
                char *alias = parse_flag_name(ts_peek().line);
                add_flag_name(&spec, alias, ts_peek().line);
                free(alias);
            }

            if (ts_match(TOK_EQ)) {
                TokenType t = ts_peek().type;
                if (t == TOK_INT || t == TOK_FLOAT || t == TOK_BOOL || t == TOK_STRING || t == TOK_LIST) {
                    spec.vtype = ts_advance().type;
                    if (!is_valid_flag_name(ts_peek().lexeme))
                        error(ts_peek().line, "Se esperaba nombre de variable para el flags");
                    spec.var_name = strdup(ts_advance().lexeme);
                } else {
                    error(ts_peek().line, "Los flags no tienen tipado automático, por lo que se esperaba tipo después de '=' en flags (int, float, bool, string, list)");
                }
            }

            /* Bloque obligatorio para flags normales */
            if (!ts_match(TOK_LBRACE))
                error(ts_peek().line, "Se esperaba '{' después de la especificación del flag");
            parse_flag_body_tokens(&spec.body_tokens, &spec.body_count);
        }

        node->data.flags.specs = realloc(node->data.flags.specs,
                                         (node->data.flags.spec_count+1)*sizeof(FlagSpec));
        node->data.flags.specs[node->data.flags.spec_count++] = spec;

        ts_skip_newlines();
        if (ts_peek().type == TOK_COMMA) { ts_advance(); ts_skip_newlines(); }
    }

    /* ─── Registrar esta definición de flags modo 1 ─── */
    if (node->data.flags.mode == 1) {
        flags_mode1_line = node->line;
    }

    return node;
}
