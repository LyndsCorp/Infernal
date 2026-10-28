/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: lexer/lexer.c
 */

#include "lexer.h"
#include "core/memory.h"
#include "keywords.h"
#include "runtime/error.h"
#include "runtime/globals.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TokenStream ts;

/* Arena de lexemes en chunks. Cada chunk es un bloque de memoria
 * independiente que no se mueve, así que los punteros devueltos por
 * arena_strndup() (que acaban en ts.tokens[*].lexeme y en
 * source_lines[*]) siguen siendo válidos aunque la arena crezca
 * después.
 *
 * La versión anterior usaba realloc() sobre un único buffer: cada
 * crecimiento podía mover el bloque e invalidar los lexemes de TODOS
 * los tokens anteriores, provocando use-after-free en cualquier
 * script con más de 64 KB de tokens o de líneas fuente. */
typedef struct ArenaChunk {
    struct ArenaChunk *next;
    size_t cap;
    size_t used;
    char *data;
} ArenaChunk;

static ArenaChunk *g_arena_head = NULL;
static ArenaChunk *g_arena_current = NULL;
static size_t g_arena_current_seq = 0;

#define ARENA_FIRST_CHUNK_SIZE (64u * 1024u)
#define ARENA_GROW_CHUNK_SIZE (4u * 1024u * 1024u)

void ts_init() {
    ts.tokens = NULL;
    ts.count = ts.cap = 0;
    ts.pos = 0;
}

static ArenaChunk *arena_new_chunk(size_t cap) {
    ArenaChunk *c = infernal_malloc(sizeof(ArenaChunk));
    c->next = NULL;
    c->cap = cap;
    c->used = 0;
    c->data = infernal_malloc(cap);
    return c;
}

void ts_free_arena(void) {
    ArenaChunk *c = g_arena_head;
    while (c) {
        ArenaChunk *next = c->next;
        free(c->data);
        free(c);
        c = next;
    }
    g_arena_head = NULL;
    g_arena_current = NULL;
    g_arena_current_seq = 0;
}

size_t ts_arena_mark(void) {
    if (!g_arena_current)
        return 0;
    if (g_arena_current_seq > 0xFFFFFFFFu || g_arena_current->used > 0xFFFFFFFFu)
        return SIZE_MAX;
    return (g_arena_current_seq << 32) | (size_t)g_arena_current->used;
}

void ts_arena_restore(size_t mark) {
    if (mark == SIZE_MAX)
        return;
    size_t seq = mark >> 32;
    size_t offset = mark & 0xFFFFFFFFu;
    ArenaChunk *c = g_arena_head;
    for (size_t i = 0; i < seq && c; i++)
        c = c->next;
    if (!c)
        return;
    if (offset > c->cap)
        offset = c->cap;
    c->used = offset;
    g_arena_current = c;
    g_arena_current_seq = seq;
}

static char *arena_strndup(const char *src, size_t n) {
    size_t needed = n + 1;
    if (!g_arena_current) {
        size_t cap = ARENA_FIRST_CHUNK_SIZE;
        if (cap < needed)
            cap = needed;
        g_arena_head = arena_new_chunk(cap);
        g_arena_current = g_arena_head;
        g_arena_current_seq = 0;
    }
    while (g_arena_current->used + needed > g_arena_current->cap) {
        if (g_arena_current->next) {
            g_arena_current = g_arena_current->next;
            g_arena_current->used = 0;
            g_arena_current_seq++;
        } else {
            size_t cap = ARENA_GROW_CHUNK_SIZE;
            if (cap < needed)
                cap = needed;
            ArenaChunk *new_chunk = arena_new_chunk(cap);
            g_arena_current->next = new_chunk;
            g_arena_current = new_chunk;
            g_arena_current_seq++;
        }
    }
    char *out = g_arena_current->data + g_arena_current->used;
    if (n)
        memcpy(out, src, n);
    out[n] = '\0';
    g_arena_current->used += needed;
    return out;
}

static char *arena_strdup(const char *src) {
    return arena_strndup(src ? src : "", src ? strlen(src) : 0);
}

void ts_add(Token t) {
    if (ts.count >= ts.cap) {
        ts.cap = ts.cap == 0 ? 256 : ts.cap * 2;
        ts.tokens = infernal_realloc(ts.tokens, ts.cap * sizeof(Token));
    }
    ts.tokens[ts.count++] = t;
}

static Token eof_token(void) {
    Token t = {TOK_EOF, "", source_line_count > 0 ? source_line_count : 1, 1, 1};
    if (source_line_count > 0 && source_lines) {
        size_t len = strlen(source_lines[source_line_count - 1]);
        t.start_col = (int)len + 1;
        t.end_col = t.start_col;
    }
    return t;
}

Token ts_peek() {
    if (ts.pos < ts.count)
        return ts.tokens[ts.pos];
    return eof_token();
}

Token ts_advance() {
    if (ts.pos < ts.count)
        return ts.tokens[ts.pos++];
    return eof_token();
}

bool ts_match(TokenType t) {
    if (ts_peek().type == t) {
        ts_advance();
        return true;
    }
    return false;
}

void ts_skip_newlines() {
    while (ts_peek().type == TOK_NEWLINE)
        ts_advance();
}

char *extract_command_string(int line) {
    if (line < 1 || line > source_line_count || !source_lines || !source_lines[line - 1])
        return infernal_strdup("");

    const char *src = source_lines[line - 1];
    const char *eq = strchr(src, '=');
    if (!eq)
        return arena_strdup("");

    const char *start = eq + 1;
    while (*start == ' ' || *start == '\t')
        start++;

    /* Encuentra un comentario solo cuando '#' está fuera de comillas.
     * No modificamos source_lines: esas líneas se reutilizan para los
     * diagnósticos con columna/marcador. */
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;
    const char *end_ptr = start + strlen(start);
    for (const char *p = start; *p; ++p) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (in_double && *p == '\\') {
            escaped = true;
            continue;
        }
        if (!in_double && *p == '\'') {
            in_single = !in_single;
            continue;
        }
        if (!in_single && *p == '\"') {
            in_double = !in_double;
            continue;
        }
        if (!in_single && !in_double && *p == '#') {
            end_ptr = p;
            break;
        }
    }

    while (end_ptr > start && (end_ptr[-1] == ' ' || end_ptr[-1] == '\t'))
        end_ptr--;

    size_t len = (size_t)(end_ptr - start);
    char *result = malloc(len + 1);
    if (!result)
        error(line, "Memoria insuficiente para extraer comando");
    memcpy(result, start, len);
    result[len] = '\0';
    return result;
}

void tokenize_buffer(const char *data, size_t len) {
    FILE *fp = fmemopen((void *)data, len, "r");
    if (!fp)
        return;
    tokenize_file(fp);
    fclose(fp);
}

void tokenize_file(FILE *fp) {
    char *line = NULL;
    size_t len = 0;
    int lineno = 0;
    bool in_block_comment = false;
    if (source_lines) {
        for (int i = 0; i < source_line_count; i++)
            free(source_lines[i]);
        free(source_lines);
        source_lines = NULL;
        source_line_count = 0;
    }

    while (getline(&line, &len, fp) != -1) {
        lineno++;
        source_lines = infernal_realloc(source_lines, (source_line_count + 1) * sizeof(char *));
        source_lines[source_line_count] = arena_strdup(line);
        size_t line_len = strlen(source_lines[source_line_count]);
        if (line_len > 0 && source_lines[source_line_count][line_len - 1] == '\n')
            source_lines[source_line_count][line_len - 1] = '\0';
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
            if (*p == '#' && *(p + 1) == '#' && *(p + 2) == '#') {
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

            if (*p == '#')
                break;
            if (isspace(*p)) {
                p++;
                continue;
            }

            int start_col = (int)(p - line);

            // Operadores varios caracteres
            if (*p == '+' && *(p + 1) == '=') {
                Token t = {TOK_PLUS_EQ, arena_strdup("+="), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '-' && *(p + 1) == '=') {
                Token t = {TOK_MINUS_EQ, arena_strdup("-="), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '*' && *(p + 1) == '=') {
                Token t = {TOK_STAR_EQ, arena_strdup("*="), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '/' && *(p + 1) == '=') {
                Token t = {TOK_SLASH_EQ, arena_strdup("/="), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '%' && *(p + 1) == '=') {
                Token t = {TOK_PERCENT_EQ, arena_strdup("%="), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '*' && *(p + 1) == '*') {
                Token t = {TOK_POW, arena_strdup("**"), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '+' && *(p + 1) == '+') {
                Token t = {TOK_INC, arena_strdup("++"), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '-' && *(p + 1) == '-') {
                Token t = {TOK_DEC, arena_strdup("--"), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }

            if (*p == '=' && *(p + 1) == '=') {
                Token t = {TOK_EEQ, arena_strdup("=="), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '!' && *(p + 1) == '=') {
                Token t = {TOK_NEQ, arena_strdup("!="), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }

            if (*p == '<' && *(p + 1) == '=') {
                Token t = {TOK_LE, arena_strdup("<="), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '>' && *(p + 1) == '=') {
                Token t = {TOK_GE, arena_strdup(">="), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '>' && *(p + 1) == '>') {
                Token t = {TOK_IDENT, arena_strdup(">>"), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }

            // Operadores de un carácter
            if (*p == '&' && *(p + 1) == '&') {
                Token t = {TOK_IDENT, arena_strdup("&&"), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }
            if (*p == '&') {
                Token t = {TOK_IDENT, arena_strdup("&"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }
            if (*p == '|') {
                Token t = {TOK_PIPE, arena_strdup("|"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            } // se usa en flags ()
            if (*p == '>') {
                Token t = {TOK_GT_OP, arena_strdup(">"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }
            if (*p == '<') {
                Token t = {TOK_LT_OP, arena_strdup("<"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }
            if (*p == '(') {
                Token t = {TOK_LPAREN, arena_strdup("("), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }
            if (*p == ')') {
                Token t = {TOK_RPAREN, arena_strdup(")"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }
            if (*p == '[') {
                Token t = {TOK_LBRACKET, arena_strdup("["), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }
            if (*p == ']') {
                Token t = {TOK_RBRACKET, arena_strdup("]"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }
            if (*p == '{') {
                Token t = {TOK_LBRACE, arena_strdup("{"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }
            if (*p == '}') {
                Token t = {TOK_RBRACE, arena_strdup("}"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }
            if (*p == ';') {
                Token t = {TOK_NEWLINE, arena_strdup(";"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }
            if (*p == ',') {
                Token t = {TOK_COMMA, arena_strdup(","), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }
            if (*p == '+') {
                Token t = {TOK_PLUS, arena_strdup("+"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }
            if (*p == '-') {
                Token t = {TOK_MINUS, arena_strdup("-"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }
            if (*p == '*') {
                Token t = {TOK_STAR, arena_strdup("*"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }
            if (*p == '%') {
                Token t = {TOK_PERCENT, arena_strdup("%"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }
            if (*p == ':') {
                Token t = {TOK_COLON, arena_strdup(":"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }

            if (*p == '/') {
                /* '/' es división cuando estamos justo después de un token
                 * que puede terminar una expresión. En cualquier otro lugar
                 * puede iniciar una ruta (/tmp, /usr/bin, ...). Esto hace que
                 * 10/2, a/b y foo()[0]/2 funcionen sin romper rutas absolutas.
                 * Los comandos shell siguen reconstruyéndose con el texto y
                 * conservan `cd /tmp`, incluso cuando / y tmp sean tokens
                 * separados. */
                TokenType prev_type = TOK_EOF;
                if (ts.count > 0)
                    prev_type = ts.tokens[ts.count - 1].type;
                bool after_expr = prev_type == TOK_NUMBER || prev_type == TOK_IDENT ||
                                  prev_type == TOK_STRING_LITERAL || prev_type == TOK_RPAREN ||
                                  prev_type == TOK_RBRACKET || prev_type == TOK_TRUE ||
                                  prev_type == TOK_FALSE;

                char *next = p + 1;
                bool can_start_path =
                    *next != '\0' && (isalnum((unsigned char)*next) || *next == '_' ||
                                      *next == '/' || *next == '.' || *next == '-' || *next == '~');

                if (!after_expr && can_start_path) {
                    char *start = p;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '/' ||
                                  *p == '.' || *p == '-' || *p == '~'))
                        p++;
                    size_t path_len = (size_t)(p - start);
                    char *lexeme = arena_strndup(start, path_len);
                    if (!lexeme)
                        error(lineno, "Memoria insuficiente para ruta");
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

                Token t = {TOK_SLASH, arena_strdup("/"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }

            if (*p == '=') {
                Token t = {TOK_EQ, arena_strdup("="), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }

            if (*p == '!') {
                Token t = {TOK_BANG, arena_strdup("!"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }

            if (*p == '\'' || *p == '"') {
                char quote = *p++;
                size_t cap = 64, bi = 0;
                char *buf = malloc(cap);
                if (!buf)
                    error(lineno, "Memoria insuficiente para cadena");
                while (*p && *p != quote && *p != '\n') {
                    char pending[2];
                    size_t pending_len = 0;
                    if (*p == '\\' && *(p + 1)) {
                        p++;
                        char esc = *p++;
                        switch (esc) {
                        case 'n':
                            pending[0] = '\n';
                            pending_len = 1;
                            break;
                        case 't':
                            pending[0] = '\t';
                            pending_len = 1;
                            break;
                        case 'r':
                            pending[0] = '\r';
                            pending_len = 1;
                            break;
                        case '\\':
                            pending[0] = '\\';
                            pending_len = 1;
                            break;
                        case '"':
                            pending[0] = '"';
                            pending_len = 1;
                            break;
                        case '\'':
                            pending[0] = '\'';
                            pending_len = 1;
                            break;
                        case 'N':
                            pending[0] = 0x1A;
                            pending_len = 1;
                            break;
                        case '0':
                        case '1':
                        case '2':
                        case '3':
                        case '4':
                        case '5':
                        case '6':
                        case '7': {
                            int val = esc - '0';
                            for (int i = 0; i < 2 && *p >= '0' && *p <= '7'; i++) {
                                val = val * 8 + (*p - '0');
                                p++;
                            }
                            pending[0] = (char)val;
                            pending_len = 1;
                            break;
                        }
                        default:
                            pending[0] = '\\';
                            pending[1] = esc;
                            pending_len = 2;
                            break;
                        }
                    } else {
                        pending[0] = *p++;
                        pending_len = 1;
                    }
                    if (bi + pending_len + 1 > cap) {
                        while (bi + pending_len + 1 > cap)
                            cap *= 2;
                        char *tmp = realloc(buf, cap);
                        if (!tmp) {
                            free(buf);
                            error(lineno, "Memoria insuficiente para cadena");
                        }
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
                char *arena_lex = arena_strndup(buf, bi);
                free(buf);
                Token t = {TOK_STRING_LITERAL, arena_lex, lineno, start_col, (int)(p - line)};
                ts_add(t);
                continue;
            }

            if (isdigit(*p) || (*p == '.' && isdigit(*(p + 1)))) {
                char *start = p;
                while (isdigit(*p))
                    p++;
                if (*p == '.' && isdigit(*(p + 1))) {
                    p++;
                    while (isdigit(*p))
                        p++;
                }
                /* Notación científica: 1.5e2, 3E-4, 2.5e+10, 1.0e29.
                 * Solo consume la 'e'/'E' si va seguida de un dígito
                 * (o de un signo + un dígito). Así evitamos romper
                 * identificadores como `error` cuando aparecen tras un
                 * número, por ejemplo en `3error` (que sigue siendo un
                 * error de sintaxis, pero no un error del lexer). */
                if ((*p == 'e' || *p == 'E') &&
                    (isdigit((unsigned char)*(p + 1)) ||
                     ((*(p + 1) == '+' || *(p + 1) == '-') && isdigit((unsigned char)*(p + 2))))) {
                    p++; /* consumir 'e' o 'E' */
                    if (*p == '+' || *p == '-')
                        p++; /* consumir signo */
                    while (isdigit((unsigned char)*p))
                        p++;
                }
                size_t len = (size_t)(p - start);
                char *lexeme = arena_strndup(start, len);
                if (!lexeme)
                    error(lineno, "Memoria insuficiente para número");
                Token t = {TOK_NUMBER, lexeme, lineno, start_col, (int)(p - line)};
                ts_add(t);
                continue;
            }

            /* \? → se emite tal cual para que el shell lo reciba como '?' literal */
            if (*p == '\\' && *(p + 1) == '?') {
                Token t = {TOK_IDENT, arena_strdup("\\?"), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }

            /* ?? → un solo token "??" (antes se tiraba silenciosamente) */
            if (*p == '?' && *(p + 1) == '?') {
                Token t = {TOK_IDENT, arena_strdup("??"), lineno, start_col, start_col + 2};
                ts_add(t);
                p += 2;
                continue;
            }

            /* ? suelto → token "?" (antes caía al p++ final y se perdía) */
            if (*p == '?') {
                Token t = {TOK_IDENT, arena_strdup("?"), lineno, start_col, start_col + 1};
                ts_add(t);
                p++;
                continue;
            }

            if ((*p == '$' || *p == '?') &&
                (isalpha(*(p + 1)) || *(p + 1) == '_' || (unsigned char)*(p + 1) >= 0x80)) {
                char *start = p;
                p++;
                while (isalnum(*p) || *p == '_' || (unsigned char)*p >= 0x80)
                    p++;
                size_t len = (size_t)(p - start);
                char *lexeme = arena_strndup(start, len);
                if (!lexeme)
                    error(lineno, "Memoria insuficiente para identificador");
                Token t = {TOK_IDENT, lexeme, lineno, start_col, (int)(p - line)};
                ts_add(t);
                continue;
            }

            if (isalpha((unsigned char)*p) || *p == '_' || *p == '.' || *p == '~' ||
                (unsigned char)*p >= 0x80) {
                char *start = p;
                bool path_prefix = (*start == '.' || *start == '~');
                while (isalnum((unsigned char)*p) || *p == '_' || *p == '.' || *p == '~' ||
                       *p == '&' || (unsigned char)*p >= 0x80 || (path_prefix && *p == '/') ||
                       (*p == '-' && *(p + 1) != '-')) {
                    p++;
                }
                size_t len = (size_t)(p - start);
                char *lexeme = arena_strndup(start, len);
                if (!lexeme)
                    error(lineno, "Memoria insuficiente para identificador");
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
        Token t = {TOK_NEWLINE, arena_strdup("\n"), lineno, 0, 0};
        ts_add(t);
    }
    free(line);
}
