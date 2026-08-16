/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/evaluator.h
*/

#ifndef RUNTIME_EVALUATOR_EVALUATOR_H
#define RUNTIME_EVALUATOR_EVALUATOR_H

#include "core/types.h"

/* Funciones públicas (orquestador) */
Value eval_expr(ASTNode *expr);
void exec_block(NodeList *block);
void exec_block_from(NodeList *block, int start_index);
void exec_flag_spec(FlagSpec *spec);

/* Auxiliar pública */
bool try_convert_value(Value *val, int target_tok_type);

#endif
