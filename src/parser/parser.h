/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License, Lynds Corp., Aros Legendarios, David Baña Szymaniak.
 * Código fuente de Infernal: parser/parser.h
*/

#ifndef PARSER_PARSER_H
#define PARSER_PARSER_H

#include "core/types.h"

NodeList parse_block(const char *terminator);
ASTNode *parse_if_statement(void);

/* --- Función para construir comando desde tokens ------------ */
char *build_command_from_tokens(int start_pos, int end_pos);

#endif
