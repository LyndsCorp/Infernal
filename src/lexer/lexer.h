/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: lexer/lexer.h
*/

#ifndef LEXER_LEXER_H
#define LEXER_LEXER_H

#include <stdio.h>
#include "core/types.h"

typedef struct {
    Token *tokens;
    int count, cap;
    int pos;
} TokenStream;

extern TokenStream ts;

void ts_init(void);
void ts_add(Token t);
Token ts_peek(void);
Token ts_advance(void);
bool ts_match(TokenType t);
void ts_skip_newlines(void);

void tokenize_file(FILE *fp);
void tokenize_buffer(const char *data, size_t len);
char *extract_command_string(int line);

#endif
