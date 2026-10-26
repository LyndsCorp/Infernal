/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: lexer/keywords.c
*/

#include <string.h>
#include <strings.h>
#include "keywords.h"

/* FNV-1a permite descartar casi todos los keywords con un hash y una
 * comparación final. Las grafías alternativas comparten su TokenType. */
static unsigned int keyword_hash(const char *s) {
    unsigned int hash = 2166136261u;
    while (*s) {
        hash ^= (unsigned char)*s++;
        hash *= 16777619u;
    }
    return hash;
}

#define KEYWORD(word_, type_, hash_) { word_, type_, hash_ }
Keyword keywords[] = {
    KEYWORD("if", TOK_IF, 0x39386e06u), KEYWORD("then", TOK_THEN, 0xe522e976u), KEYWORD("fi", TOK_FI, 0x5c22ded0u),
    KEYWORD("else", TOK_ELSE, 0xbdbf5bf0u), KEYWORD("elseif", TOK_ELSEIF, 0x54554c87u), KEYWORD("elif", TOK_ELSEIF, 0xc0a5c8c3u),
    KEYWORD("while", TOK_WHILE, 0x0dc628ceu), KEYWORD("for", TOK_FOR, 0xacf38390u), KEYWORD("switch", TOK_SWITCH, 0x93e05f71u),
    KEYWORD("case", TOK_CASE, 0x9b2538b1u), KEYWORD("default", TOK_DEFAULT, 0x933b5bdeu),
    KEYWORD("function", TOK_FUNCTION, 0x9ed64249u), KEYWORD("func", TOK_FUNCTION, 0x0e7e4807u),
    KEYWORD("fn", TOK_FUNCTION, 0x6322e9d5u), KEYWORD("def", TOK_FUNCTION, 0xc5597e8cu),
    KEYWORD("return", TOK_RETURN, 0x85ee37bfu), KEYWORD("break", TOK_BREAK, 0xc9648178u),
    KEYWORD("repeat", TOK_REPEAT, 0xd99ba82au), KEYWORD("portal", TOK_PORTAL, 0x150c5d8bu),
    KEYWORD("continue", TOK_CONTINUE, 0xb1727e44u), KEYWORD("import", TOK_IMPORT, 0x112a90d4u),
    KEYWORD("try", TOK_TRY, 0xac1db00eu), KEYWORD("catch", TOK_CATCH, 0x4288e94cu),
    KEYWORD("int", TOK_INT, 0x95e97e5eu), KEYWORD("float", TOK_FLOAT, 0xa6c45d85u), KEYWORD("bool", TOK_BOOL, 0xc894953du),
    KEYWORD("string", TOK_STRING, 0x17c16538u), KEYWORD("list", TOK_LIST, 0x0cfb5881u), KEYWORD("map", TOK_MAP, 0xdfa2efb1u),
    KEYWORD("define", TOK_DEFINE, 0x6a9f8552u), KEYWORD("local", TOK_LOCAL, 0x9c436708u), KEYWORD("global", TOK_GLOBAL, 0x1dff06aeu),
    KEYWORD("and", TOK_AND, 0x0f29c2a6u), KEYWORD("or", TOK_OR, 0x5d342984u), KEYWORD("in", TOK_IN, 0x41387a9eu),
    KEYWORD("not", TOK_NOT, 0x29b19c8au), KEYWORD("flags", TOK_FLAG, 0x9c677a2cu), KEYWORD("line", TOK_LINE, 0x17db1627u),
    KEYWORD("execute", TOK_EXECUTE, 0xa01e3d98u)
};
#undef KEYWORD

TokenType lookup_keyword(const char *lexeme) {
    if (strcasecmp(lexeme, "true") == 0) return TOK_TRUE;
    if (strcasecmp(lexeme, "false") == 0) return TOK_FALSE;

    unsigned int hash = keyword_hash(lexeme);
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (keywords[i].hash == hash && strcmp(lexeme, keywords[i].word) == 0)
            return keywords[i].tok;
    }
    return TOK_IDENT;
}
