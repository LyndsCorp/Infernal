/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/eval_call.h
*/

#ifndef RUNTIME_EVALUATOR_EVAL_CALL_H
#define RUNTIME_EVALUATOR_EVAL_CALL_H

#include "core/types.h"

Value eval_call(ASTNode *expr);

#endif
