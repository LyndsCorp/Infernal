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
    KEYWORD("int", TOK_INT, 0x95e97e5eu), KEYWORD("uint", TOK_UINT, 0xcb9836a1u), KEYWORD("float", TOK_FLOAT, 0xa6c45d85u), KEYWORD("bool", TOK_BOOL, 0xc894953du),
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

    switch (keyword_hash(lexeme)) {
        case 0x39386e06u: return strcmp(lexeme, "if") == 0 ? TOK_IF : TOK_IDENT;
        case 0xe522e976u: return strcmp(lexeme, "then") == 0 ? TOK_THEN : TOK_IDENT;
        case 0x5c22ded0u: return strcmp(lexeme, "fi") == 0 ? TOK_FI : TOK_IDENT;
        case 0xbdbf5bf0u: return strcmp(lexeme, "else") == 0 ? TOK_ELSE : TOK_IDENT;
        case 0x54554c87u: return strcmp(lexeme, "elseif") == 0 ? TOK_ELSEIF : TOK_IDENT;
        case 0xc0a5c8c3u: return strcmp(lexeme, "elif") == 0 ? TOK_ELSEIF : TOK_IDENT;
        case 0x0dc628ceu: return strcmp(lexeme, "while") == 0 ? TOK_WHILE : TOK_IDENT;
        case 0xacf38390u: return strcmp(lexeme, "for") == 0 ? TOK_FOR : TOK_IDENT;
        case 0x93e05f71u: return strcmp(lexeme, "switch") == 0 ? TOK_SWITCH : TOK_IDENT;
        case 0x9b2538b1u: return strcmp(lexeme, "case") == 0 ? TOK_CASE : TOK_IDENT;
        case 0x933b5bdeu: return strcmp(lexeme, "default") == 0 ? TOK_DEFAULT : TOK_IDENT;
        case 0x9ed64249u: return strcmp(lexeme, "function") == 0 ? TOK_FUNCTION : TOK_IDENT;
        case 0x0e7e4807u: return strcmp(lexeme, "func") == 0 ? TOK_FUNCTION : TOK_IDENT;
        case 0x6322e9d5u: return strcmp(lexeme, "fn") == 0 ? TOK_FUNCTION : TOK_IDENT;
        case 0xc5597e8cu: return strcmp(lexeme, "def") == 0 ? TOK_FUNCTION : TOK_IDENT;
        case 0x85ee37bfu: return strcmp(lexeme, "return") == 0 ? TOK_RETURN : TOK_IDENT;
        case 0xc9648178u: return strcmp(lexeme, "break") == 0 ? TOK_BREAK : TOK_IDENT;
        case 0xd99ba82au: return strcmp(lexeme, "repeat") == 0 ? TOK_REPEAT : TOK_IDENT;
        case 0x150c5d8bu: return strcmp(lexeme, "portal") == 0 ? TOK_PORTAL : TOK_IDENT;
        case 0xb1727e44u: return strcmp(lexeme, "continue") == 0 ? TOK_CONTINUE : TOK_IDENT;
        case 0x112a90d4u: return strcmp(lexeme, "import") == 0 ? TOK_IMPORT : TOK_IDENT;
        case 0xac1db00eu: return strcmp(lexeme, "try") == 0 ? TOK_TRY : TOK_IDENT;
        case 0x4288e94cu: return strcmp(lexeme, "catch") == 0 ? TOK_CATCH : TOK_IDENT;
        case 0x95e97e5eu: return strcmp(lexeme, "int") == 0 ? TOK_INT : TOK_IDENT;
        case 0xcb9836a1u: return strcmp(lexeme, "uint") == 0 ? TOK_UINT : TOK_IDENT;
        case 0xa6c45d85u: return strcmp(lexeme, "float") == 0 ? TOK_FLOAT : TOK_IDENT;
        case 0xc894953du: return strcmp(lexeme, "bool") == 0 ? TOK_BOOL : TOK_IDENT;
        case 0x17c16538u: return strcmp(lexeme, "string") == 0 ? TOK_STRING : TOK_IDENT;
        case 0x0cfb5881u: return strcmp(lexeme, "list") == 0 ? TOK_LIST : TOK_IDENT;
        case 0xdfa2efb1u: return strcmp(lexeme, "map") == 0 ? TOK_MAP : TOK_IDENT;
        case 0x6a9f8552u: return strcmp(lexeme, "define") == 0 ? TOK_DEFINE : TOK_IDENT;
        case 0x9c436708u: return strcmp(lexeme, "local") == 0 ? TOK_LOCAL : TOK_IDENT;
        case 0x1dff06aeu: return strcmp(lexeme, "global") == 0 ? TOK_GLOBAL : TOK_IDENT;
        case 0x0f29c2a6u: return strcmp(lexeme, "and") == 0 ? TOK_AND : TOK_IDENT;
        case 0x5d342984u: return strcmp(lexeme, "or") == 0 ? TOK_OR : TOK_IDENT;
        case 0x41387a9eu: return strcmp(lexeme, "in") == 0 ? TOK_IN : TOK_IDENT;
        case 0x29b19c8au: return strcmp(lexeme, "not") == 0 ? TOK_NOT : TOK_IDENT;
        case 0x9c677a2cu: return strcmp(lexeme, "flags") == 0 ? TOK_FLAG : TOK_IDENT;
        case 0x17db1627u: return strcmp(lexeme, "line") == 0 ? TOK_LINE : TOK_IDENT;
        case 0xa01e3d98u: return strcmp(lexeme, "execute") == 0 ? TOK_EXECUTE : TOK_IDENT;
        default: return TOK_IDENT;
    }
}
