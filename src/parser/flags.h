/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: parser/flags.h
*/

#ifndef PARSER_FLAGS_H
#define PARSER_FLAGS_H

#include "core/types.h"

ASTNode *parse_flags(void);
void parse_flag_body_tokens(Token **body_tokens, int *body_count, int already_consumed_brace);

#endif
