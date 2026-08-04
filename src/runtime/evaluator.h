/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License.
 * Código fuente de Infernal: runtime/evaluator.h
*/

#ifndef RUNTIME_EVALUATOR_H
#define RUNTIME_EVALUATOR_H

#include "core/types.h"

Value eval_expr(ASTNode *expr);
void exec_block(NodeList *block);
void exec_block_from(NodeList *block, int start_index);
void exec_flag_spec(FlagSpec *spec);

/* ─── Conversión de tipos (pública) ─── */
bool try_convert_value(Value *val, int target_tok_type);

#endif
