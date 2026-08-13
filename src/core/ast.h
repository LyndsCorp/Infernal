/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: core/ast.h
*/

#ifndef CORE_AST_H
#define CORE_AST_H

#include "types.h"

ASTNode *node_create(int kind, int line);
void     nodelist_add(NodeList *list, ASTNode *node);

#endif
