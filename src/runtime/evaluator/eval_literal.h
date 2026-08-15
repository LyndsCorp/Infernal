/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/eval_literal.h
*/

#ifndef RUNTIME_EVALUATOR_EVAL_LITERAL_H
#define RUNTIME_EVALUATOR_EVAL_LITERAL_H

#include "core/types.h"

Value eval_literal(ASTNode *expr);
Value eval_var(ASTNode *expr);
Value eval_list(ASTNode *expr);
Value eval_map(ASTNode *expr);

#endif
