/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: parser/expression.h
*/

#ifndef PARSER_EXPRESSION_H
#define PARSER_EXPRESSION_H

#include "core/types.h"

ASTNode *parse_expression(int dummy);
ASTNode *parse_primary(void);
ASTNode *parse_slice_content(int line);

/* Detecta el patrón "string literal pegado a IDENT/STRING/NUMBER" (típico
 * de comillas dobles sin escapar) y emite un mensaje específico. Si el
 * patrón no aplica, retorna sin hacer nada. Si aplica, llama a error_at()
 * y no retorna. */
void check_unescaped_quote(int prev_pos, const char *context);

#endif
