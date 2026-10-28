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

/* Libera estructuras parciales del parser cuando un error aborta parse_block(). */
void parser_cleanup_on_error(void);

/* Acceso al contador de bloques abiertos que esperan un 'fi'.
 * Solo lo usa exec_flag_spec_impl() para salvar/restaurar estado. */
int parser_get_fi_nesting(void);
void parser_set_fi_nesting(int v);

#endif
