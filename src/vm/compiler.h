/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: vm/compiler.h
*/

#ifndef VM_COMPILER_H
#define VM_COMPILER_H

#include "bytecode.h"
#include "core/ast.h"

Chunk *compile_program(NodeList *program);
Chunk *compile_function(ASTNode *func_node);

#endif
