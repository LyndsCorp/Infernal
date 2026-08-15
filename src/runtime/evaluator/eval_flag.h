/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/eval_flag.h
*/

#ifndef RUNTIME_EVALUATOR_EVAL_FLAG_H
#define RUNTIME_EVALUATOR_EVAL_FLAG_H

#include "core/types.h"

/* Ejecuta un spec individual (usado internamente) */
void exec_flag_spec_impl(FlagSpec *spec);

/* Ejecuta un nodo completo de flags (orquestador) */
void exec_flags(ASTNode *node);

#endif
