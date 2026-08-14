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

            // Operadores de dos caracteres
            if (*p == '&' && *(p+1) == '&') {
                Token t = {TOK_AND, "&&", lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '|' && *(p+1) == '|') {
                Token t = {TOK_OR, "||", lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '=' && *(p+1) == '=') {
                Token t = {TOK_EEQ, "==", lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '!' && *(p+1) == '=') {
                Token t = {TOK_NEQ, "!=", lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '<' && *(p+1) == '<') {  // Nota: corregido de TOK_LE
                Token t = {TOK_LE, "<=", lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '>' && *(p+1) == '=') {
                Token t = {TOK_GE, ">=", lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '>' && *(p+1) == '>') {
                Token t = {TOK_GGT, ">>", lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }

            // Operadores de un carácter
            if (*p == '|') { Token t = {TOK_PIPE, "|", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '>') { Token t = {TOK_GT_OP, ">", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '<') { Token t = {TOK_LT_OP, "<", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '(') { Token t = {TOK_LPAREN, "(", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == ')') { Token t = {TOK_RPAREN, ")", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '[') { Token t = {TOK_LBRACKET, "[", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == ']') { Token t = {TOK_RBRACKET, "]", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '{') { Token t = {TOK_LBRACE, "{", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '}') { Token t = {TOK_RBRACE, "}", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == ';') { Token t = {TOK_SEMI, ";", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == ',') { Token t = {TOK_COMMA, ",", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '+') { Token t = {TOK_PLUS, "+", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '-') { Token t = {TOK_MINUS, "-", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '*') { Token t = {TOK_STAR, "*", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == '%') { Token t = {TOK_PERCENT, "%", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }
            if (*p == ':') { Token t = {TOK_COLON, ":", lineno, start_col, start_col + 1}; ts_add(t); p++; continue; }

            if (*p == '/') {
                char *next = p + 1;
                if (*next != '\0' && (isalnum(*next) || *next == '_' || *next == '/' || *next == '.' || *next == '-' || *next == '~')) {
                    char *start = p;
                    while (*p && (isalnum(*p) || *p == '_' || *p == '/' || *p == '.' || *p == '-' || *p == '~')) p++;
                    char buf[256]; int len = p - start;
                    memcpy(buf, start, len); buf[len] = '\0';
                    TokenType k = lookup_keyword(buf);
                    Token t;
                    t.type = (k != TOK_IDENT) ? k : TOK_IDENT;
                    t.lexeme = strdup(buf);
                    t.line = lineno;
                    t.start_col = start_col;
                    t.end_col = (int)(p - line);
                    ts_add(t);
                    continue;
                } else {
                    Token t = {TOK_SLASH, "/", lineno, start_col, start_col + 1};
                    ts_add(t);
                    p++;
                    continue;
                }
            }

            if (*p == '=') {
                Token t = {TOK_EQ, "=", lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }

            if (*p == '!') {
                Token t = {TOK_BANG, "!", lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }

            if (*p == '@') {
                Token t = {TOK_AT, "@", lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }

            if (*p == '\'' || *p == '"') {
                char quote = *p++;
                char buf[4096]; int bi = 0;
                while (*p && *p != quote && *p != '\n') {
                    if (*p == '\\' && *(p+1)) {
                        p++; // saltar la barra invertida
                        char esc = *p++;
                        switch (esc) {
                            case 'n': buf[bi++] = '\n'; break;
                            case 't': buf[bi++] = '\t'; break;
                            case 'r': buf[bi++] = '\r'; break;
                            case '\\': buf[bi++] = '\\'; break;
                            case '"': buf[bi++] = '"'; break;
                            case '\'': buf[bi++] = '\''; break;
                            case 'N': buf[bi++] = 0x1A; break;   // marcador para suprimir newline
                            case '0': case '1': case '2': case '3':
                            case '4': case '5': case '6': case '7': {
                                // secuencia octal (ej. \033)
                                int val = esc - '0';
                                for (int i = 0; i < 2 && *p >= '0' && *p <= '7'; i++) {
                                    val = val * 8 + (*p - '0');
                                    p++;
                                }
                                buf[bi++] = (char)val;
                                break;
                            }
                            default:
                                // secuencia desconocida: mantener la barra y el carácter
                                buf[bi++] = '\\';
                                buf[bi++] = esc;
                                break;
                        }
                    } else {
                        buf[bi++] = *p++;
                    }
                }
                buf[bi] = '\0';
                if (*p == quote) {
                    p++;
                } else {
                    error(lineno, "Cadena de texto sin cerrar: %c%s", quote, buf);
                }
                Token t = {TOK_STRING_LITERAL, strdup(buf), lineno, start_col, (int)(p - line)};
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
                char buf[128]; int len = p - start;
                memcpy(buf, start, len); buf[len] = '\0';
                Token t = {TOK_NUMBER, strdup(buf), lineno, start_col, (int)(p - line)};
                ts_add(t);
                continue;
            }

            if ((*p == '$' || *p == '?') && (isalpha(*(p+1)) || *(p+1) == '_')) {
                char *start = p;
                p++;
                while (isalnum(*p) || *p == '_') p++;
                int len = p - start;
                char buf[256];
                memcpy(buf, start, len);
                buf[len] = '\0';
                Token t = {TOK_IDENT, strdup(buf), lineno, start_col, (int)(p - line)};
                ts_add(t);
                continue;
            }

            if (isalpha(*p) || *p == '_' || *p == '.' || *p == '~') {
                char *start = p;
                while (isalnum(*p) || *p == '_' || *p == '/' || *p == '.' || *p == '-' || *p == '~') p++;
                char buf[256]; int len = p - start;
                memcpy(buf, start, len); buf[len] = '\0';
                TokenType k = lookup_keyword(buf);
                Token t;
                t.type = (k != TOK_IDENT) ? k : TOK_IDENT;
                t.lexeme = strdup(buf);
                t.line = lineno;
                t.start_col = start_col;
                t.end_col = (int)(p - line);
                ts_add(t);
                continue;
            }
            p++;
        }
        Token t = {TOK_NEWLINE, "\n", lineno, 0, 0};
        ts_add(t);
    }
    free(line);
}
