/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: lexer/lexer.c
*/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "lexer.h"
#include "keywords.h"
#include "runtime/globals.h"
#include "runtime/error.h"

TokenStream ts;

void ts_init() { ts.tokens = NULL; ts.count = ts.cap = 0; ts.pos = 0; }

void ts_add(Token t) {
    if (ts.count >= ts.cap) {
        ts.cap = ts.cap == 0 ? 256 : ts.cap * 2;
        ts.tokens = realloc(ts.tokens, ts.cap * sizeof(Token));
    }
    ts.tokens[ts.count++] = t;
}

Token ts_peek() {
    if (ts.pos < ts.count) return ts.tokens[ts.pos];
    return (Token){TOK_EOF, "", 0, 0, 0};
}

Token ts_advance() {
    if (ts.pos < ts.count) return ts.tokens[ts.pos++];
    return (Token){TOK_EOF, "", 0, 0, 0};
}

bool ts_match(TokenType t) {
    if (ts_peek().type == t) { ts_advance(); return true; }
    return false;
}

void ts_skip_newlines() {
    while (ts_peek().type == TOK_NEWLINE) ts_advance();
}

char *extract_command_string(int line) {
    if (line < 1 || line > source_line_count) return strdup("");
    char *src = source_lines[line - 1];
    char *eq = strchr(src, '=');
    if (!eq) return strdup("");
    char *start = eq + 1;
    while (*start == ' ' || *start == '\t') start++;
    char *hash = strchr(start, '#');
    if (hash) *hash = '\0';
    int end = strlen(start) - 1;
    while (end >= 0 && (start[end] == ' ' || start[end] == '\t')) end--;
    start[end + 1] = '\0';
    return strdup(start);
}

void tokenize_buffer(const char *data, size_t len) {
    FILE *fp = fmemopen((void*)data, len, "r");
    if (!fp) return;
    tokenize_file(fp);
    fclose(fp);
}

void tokenize_file(FILE *fp) {
    char *line = NULL;
    size_t len = 0;
    int lineno = 0;
    bool in_block_comment = false;
    if (source_lines) {
        for (int i = 0; i < source_line_count; i++) free(source_lines[i]);
        free(source_lines);
        source_lines = NULL;
        source_line_count = 0;
    }

    while (getline(&line, &len, fp) != -1) {
        lineno++;
        source_lines = realloc(source_lines, (source_line_count + 1) * sizeof(char*));
        source_lines[source_line_count] = strdup(line);
        if (source_lines[source_line_count][strlen(source_lines[source_line_count])-1] == '\n')
            source_lines[source_line_count][strlen(source_lines[source_line_count])-1] = '\0';
        source_line_count++;

        char *p = line;
        if (in_block_comment) {
            char *close = strstr(p, "###");
            if (close) {
                in_block_comment = false;
                p = close + 3;
            } else {
                continue;
            }
        }

        while (*p) {
            if (*p == '#' && *(p+1) == '#' && *(p+2) == '#') {
                if (!in_block_comment) {
                    char *close = strstr(p + 3, "###");
                    if (close) {
                        p = close + 3;
                        continue;
                    } else {
                        in_block_comment = true;
                        break;
                    }
                } else {
                    in_block_comment = false;
                    p += 3;
                    continue;
                }
            }

            if (*p == '#') break;
            if (isspace(*p)) { p++; continue; }

            int start_col = (int)(p - line);

            // --- NUEVOS OPERADORES DE VARIOS CARACTERES ---
            if (*p == '+' && *(p+1) == '=') {
                Token t = {TOK_PLUS_EQ, strdup("+="), lineno, start_col, start_col + 2};
                ts_add(t); p += 2; continue;
            }
            if (*p == '-' && *(p+1) == '=') {
                Token t = {TOK_MINUS_EQ, strdup("-="), lineno, start_col, start_col + 2};
                ts_add(t); p += 2; continue;
            }
            if (*p == '*' && *(p+1) == '=') {
                Token t = {TOK_STAR_EQ, strdup("*="), lineno, start_col, start_col + 2};
                ts_add(t); p += 2; continue;
            }
            if (*p == '/' && *(p+1) == '=') {
                Token t = {TOK_SLASH_EQ, strdup("/="), lineno, start_col, start_col + 2};
                ts_add(t); p += 2; continue;
            }
            if (*p == '*' && *(p+1) == '*') {
                Token t = {TOK_POW, strdup("**"), lineno, start_col, start_col + 2};
                ts_add(t); p += 2; continue;
            }
            if (*p == '+' && *(p+1) == '+') {
                Token t = {TOK_INC, strdup("++"), lineno, start_col, start_col + 2};
                ts_add(t); p += 2; continue;
            }
            if (*p == '-' && *(p+1) == '-') {
                Token t = {TOK_DEC, strdup("--"), lineno, start_col, start_col + 2};
                ts_add(t); p += 2; continue;
            }

            // Operadores de dos caracteres existentes
            if (*p == '&' && *(p+1) == '&') {
                Token t = {TOK_AND, strdup("&&"), lineno, start_col, start_col + 2};
                ts_add(t); p += 2; continue;
            }
            if (*p == '|' && *(p+1) == '|') {
                Token t = {TOK_OR, strdup("||"), lineno, start_col, start_col + 2};
                ts_add(t); p += 2; continue;
            }
            if (*p == '=' && *(p+1) == '=') {
                Token t = {TOK_EEQ, strdup("=="), lineno, start_col, start_col + 2};
                ts_add(t); p += 2; continue;
            }
            if (*p == '!' && *(p+1) == '=') {
                Token t = {TOK_NEQ, strdup("!="), lineno, start_col, start_col + 2};
                ts_add(t); p += 2; continue;
            }
            // CORRECCIÓN: <= debe ser '<' seguido de '=', no '<' seguido de '<'
            if (*p == '<' && *(p+1) == '=') {
                Token t = {TOK_LE, strdup("<="), lineno, start_col, start_col + 2};
                ts_add(t); p += 2; continue;
            }
            if (*p == '>' && *(p+1) == '=') {
                Token t = {TOK_GE, strdup(">="), lineno, start_col, start_col + 2};
                ts_add(t); p += 2; continue;
            }
            if (*p == '>' && *(p+1) == '>') {
                Token t = {TOK_GGT, strdup(">>"), lineno, start_col, start_col + 2};
                ts_add(t); p += 2; continue;
            }

            // Operadores de un carácter (resto igual)
            if (*p == '|') { Token t = {TOK_PIPE, strdup("|"), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '>') { Token t = {TOK_GT_OP, strdup(">"), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '<') { Token t = {TOK_LT_OP, strdup("<"), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '(') { Token t = {TOK_LPAREN, strdup("("), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == ')') { Token t = {TOK_RPAREN, strdup(")"), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '[') { Token t = {TOK_LBRACKET, strdup("["), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == ']') { Token t = {TOK_RBRACKET, strdup("]"), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '{') { Token t = {TOK_LBRACE, strdup("{"), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '}') { Token t = {TOK_RBRACE, strdup("}"), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == ';') { Token t = {TOK_SEMI, strdup(";"), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == ',') { Token t = {TOK_COMMA, strdup(","), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '+') { Token t = {TOK_PLUS, strdup("+"), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '-') { Token t = {TOK_MINUS, strdup("-"), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '*') { Token t = {TOK_STAR, strdup("*"), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '%') { Token t = {TOK_PERCENT, strdup("%"), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == ':') { Token t = {TOK_COLON, strdup(":"), lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }

            if (*p == '/') {
                char *next = p + 1;
                if (*next != '\0' && (isalnum(*next) || *next == '_' || *next == '/' || *next == '.' || *next == '-' || *next == '~')) {
                    char *start = p;
                    while (*p && (isalnum(*p) || *p == '_' || *p == '/' || *p == '.' || *p == '-' || *p == '~')) p++;
                    size_t len = (size_t)(p - start);
                    char *lexeme = strndup(start, len);
                    if (!lexeme) error(lineno, "Memoria insuficiente para ruta");
                    TokenType k = lookup_keyword(lexeme);
                    Token t;
                    t.type = (k != TOK_IDENT) ? k : TOK_IDENT;
                    t.lexeme = lexeme;
                    t.line = lineno;
                    t.start_col = start_col;
                    t.end_col = (int)(p - line);
                    ts_add(t);
                    continue;
                } else {
                    Token t = {TOK_SLASH, strdup("/"), lineno, start_col, start_col + 1};
                    ts_add(t);
                    p++;
                    continue;
                }
            }

            if (*p == '=') {
                Token t = {TOK_EQ, strdup("="), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }

            if (*p == '!') {
                Token t = {TOK_BANG, strdup("!"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }

            if (*p == '\'' || *p == '"') {
                char quote = *p++;
                size_t cap = 64, bi = 0;
                char *buf = malloc(cap);
                if (!buf) error(lineno, "Memoria insuficiente para cadena");
                while (*p && *p != quote && *p != '\n') {
                    char pending[2];
                    size_t pending_len = 0;
                    if (*p == '\\' && *(p+1)) {
                        p++;
                        char esc = *p++;
                        switch (esc) {
                            case 'n': pending[0] = '\n'; pending_len = 1; break;
                            case 't': pending[0] = '\t'; pending_len = 1; break;
                            case 'r': pending[0] = '\r'; pending_len = 1; break;
                            case '\\': pending[0] = '\\'; pending_len = 1; break;
                            case '"': pending[0] = '"'; pending_len = 1; break;
                            case '\'': pending[0] = '\''; pending_len = 1; break;
                            case 'N': pending[0] = 0x1A; pending_len = 1; break;
                            case '0': case '1': case '2': case '3':
                            case '4': case '5': case '6': case '7': {
                                int val = esc - '0';
                                for (int i = 0; i < 2 && *p >= '0' && *p <= '7'; i++) {
                                    val = val * 8 + (*p - '0');
                                    p++;
                                }
                                pending[0] = (char)val; pending_len = 1;
                                break;
                            }
                            default:
                                pending[0] = '\\'; pending[1] = esc; pending_len = 2;
                                break;
                        }
                    } else {
                        pending[0] = *p++;
                        pending_len = 1;
                    }
                    if (bi + pending_len + 1 > cap) {
                        while (bi + pending_len + 1 > cap) cap *= 2;
                        char *tmp = realloc(buf, cap);
                        if (!tmp) { free(buf); error(lineno, "Memoria insuficiente para cadena"); }
                        buf = tmp;
                    }
                    memcpy(buf + bi, pending, pending_len);
                    bi += pending_len;
                }
                buf[bi] = '\0';
                if (*p == quote) {
                    p++;
                } else {
                    free(buf);
                    error(lineno, "Cadena de texto sin cerrar");
                }
                Token t = {TOK_STRING_LITERAL, buf, lineno, start_col, (int)(p - line)};
                ts_add(t);
                continue;
            }

            if (isdigit(*p) || (*p == '.' && isdigit(*(p+1)))) {
                char *start = p;
                while (isdigit(*p)) p++;
                if (*p == '.' && isdigit(*(p+1))) {
                    p++;
                    while (isdigit(*p)) p++;
                }
                size_t len = (size_t)(p - start);
                char *lexeme = strndup(start, len);
                if (!lexeme) error(lineno, "Memoria insuficiente para número");
                Token t = {TOK_NUMBER, lexeme, lineno, start_col, (int)(p - line)};
                ts_add(t);
                continue;
            }

            if ((*p == '$' || *p == '?') && (isalpha(*(p+1)) || *(p+1) == '_')) {
                char *start = p;
                p++;
                while (isalnum(*p) || *p == '_') p++;
                size_t len = (size_t)(p - start);
                char *lexeme = strndup(start, len);
                if (!lexeme) error(lineno, "Memoria insuficiente para identificador");
                Token t = {TOK_IDENT, lexeme, lineno, start_col, (int)(p - line)};
                ts_add(t);
                continue;
            }

            if (isalpha(*p) || *p == '_' || *p == '.' || *p == '~') {
                char *start = p;
                while (isalnum((unsigned char)*p) || *p == '_' || *p == '/' || *p == '.' ||
                       *p == '~' || (*p == '-' && *(p + 1) != '-')) {
                    p++;
                }
                size_t len = (size_t)(p - start);
                char *lexeme = strndup(start, len);
                if (!lexeme) error(lineno, "Memoria insuficiente para identificador");
                TokenType k = lookup_keyword(lexeme);
                Token t;
                t.type = (k != TOK_IDENT) ? k : TOK_IDENT;
                t.lexeme = lexeme;
                t.line = lineno;
                t.start_col = start_col;
                t.end_col = (int)(p - line);
                ts_add(t);
                continue;
            }
            p++;
        }
        Token t = {TOK_NEWLINE, strdup("\n"), lineno, 0, 0};
        ts_add(t);
    }
    free(line);
}
