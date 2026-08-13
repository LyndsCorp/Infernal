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
ASTNode *parse_index_or_slice(int line);  // <-- NUEVO

#endif
