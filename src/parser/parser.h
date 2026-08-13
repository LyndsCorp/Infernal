/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
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
