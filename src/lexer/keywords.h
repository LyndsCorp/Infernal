/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: lexer/keywords.h
*/

#ifndef LEXER_KEYWORDS_H
#define LEXER_KEYWORDS_H

#include "core/types.h"

typedef struct {
    const char *word;
    TokenType tok;
} Keyword;

extern Keyword keywords[];
TokenType lookup_keyword(const char *s);

#endif
