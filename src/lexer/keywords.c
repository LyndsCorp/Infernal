/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: lexer/keywords.c
*/

#include <string.h>
#include "keywords.h"

TokenType lookup_keyword(const char *lexeme) {
    // Control de flujo
    if (strcmp(lexeme, "if") == 0) return TOK_IF;
    if (strcmp(lexeme, "then") == 0) return TOK_THEN;
    if (strcmp(lexeme, "fi") == 0) return TOK_FI;
    if (strcmp(lexeme, "else") == 0) return TOK_ELSE;
    if (strcmp(lexeme, "elseif") == 0) return TOK_ELSEIF;
    if (strcmp(lexeme, "while") == 0) return TOK_WHILE;
    if (strcmp(lexeme, "for") == 0) return TOK_FOR;
    if (strcmp(lexeme, "switch") == 0) return TOK_SWITCH;
    if (strcmp(lexeme, "case") == 0) return TOK_CASE;
    if (strcmp(lexeme, "default") == 0) return TOK_DEFAULT;

    // Funciones
    if (strcmp(lexeme, "function") == 0) return TOK_FUNCTION;
    if (strcmp(lexeme, "func") == 0) return TOK_FUNCTION;
    if (strcmp(lexeme, "fn") == 0) return TOK_FUNCTION;
    if (strcmp(lexeme, "def") == 0) return TOK_FUNCTION;

    if (strcmp(lexeme, "return") == 0) return TOK_RETURN;

    // Bucles
    if (strcmp(lexeme, "break") == 0) return TOK_BREAK;
    if (strcmp(lexeme, "repeat") == 0) return TOK_REPEAT;
    if (strcmp(lexeme, "portal") == 0) return TOK_PORTAL;
    if (strcmp(lexeme, "continue") == 0) return TOK_CONTINUE;

    // Módulos Fire
    if (strcmp(lexeme, "import") == 0) return TOK_IMPORT;

    // Robustez
    if (strcmp(lexeme, "try") == 0) return TOK_TRY;
    if (strcmp(lexeme, "catch") == 0) return TOK_CATCH;

    // Tipos de variables
    if (strcmp(lexeme, "int") == 0) return TOK_INT;
    if (strcmp(lexeme, "float") == 0) return TOK_FLOAT;
    if (strcmp(lexeme, "bool") == 0) return TOK_BOOL;
    if (strcmp(lexeme, "string") == 0) return TOK_STRING;
    if (strcmp(lexeme, "list") == 0) return TOK_LIST;
    if (strcmp(lexeme, "map") == 0) return TOK_MAP;

    // Booleanos
    if (strcmp(lexeme, "true") == 0) return TOK_TRUE;
    if (strcmp(lexeme, "false") == 0) return TOK_FALSE;

    // Scopes para variables
    if (strcmp(lexeme, "local") == 0) return TOK_LOCAL;
    if (strcmp(lexeme, "global") == 0) return TOK_GLOBAL;

    // Condiciones
    if (strcmp(lexeme, "and") == 0) return TOK_AND;
    if (strcmp(lexeme, "or") == 0) return TOK_OR;
    if (strcmp(lexeme, "in") == 0) return TOK_IN;
    if (strcmp(lexeme, "not") == 0) return TOK_NOT;

    // Utilidades
    if (strcmp(lexeme, "flags") == 0) return TOK_FLAG;
    if (strcmp(lexeme, "line") == 0) return TOK_LINE;
    if (strcmp(lexeme, "execute") == 0) return TOK_EXECUTE;

    return TOK_IDENT;
}
