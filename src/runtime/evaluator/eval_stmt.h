/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/eval_stmt.h
*/

#ifndef RUNTIME_EVALUATOR_EVAL_STMT_H
#define RUNTIME_EVALUATOR_EVAL_STMT_H

#include "core/types.h"

void exec_block_impl(NodeList *block);
void exec_block_from_impl(NodeList *block, int start_index);
void exec_stmt(ASTNode *stmt);

#endif
