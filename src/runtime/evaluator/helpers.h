/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/helpers.h
*/

#ifndef RUNTIME_EVALUATOR_HELPERS_H
#define RUNTIME_EVALUATOR_HELPERS_H

#include "core/types.h"

bool val_is_truthy(Value v);
const char *type_name(int tok_type);
int get_node_line(ASTNode *node);
Value resolve_reference(Value v, int line);
int extract_integer_index(ASTNode *node, int line);
bool try_convert_value(Value *val, int target_tok_type);

#endif
