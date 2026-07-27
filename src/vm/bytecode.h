/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License.
 * Código fuente de Infernal: vm/bytecode.h
*/

#ifndef VM_BYTECODE_H
#define VM_BYTECODE_H

#include "core/types.h"
#include "core/value.h"

typedef enum {
    OP_NOP,
    OP_PUSH_INT, OP_PUSH_FLOAT, OP_PUSH_STRING, OP_PUSH_BOOL, OP_PUSH_NULL,
    OP_LOAD_VAR,
    OP_STORE_VAR,
    OP_LOAD_GLOBAL,
    OP_STORE_GLOBAL,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_NEG,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_AND, OP_OR, OP_NOT,
    OP_CALL_BUILTIN,
    OP_CALL_USER,
    OP_RETURN,
    OP_JUMP_IF_FALSE, OP_JUMP,
    OP_DUP, OP_POP,
    OP_NEW_LIST, OP_LIST_APPEND,
    OP_INDEX,
    OP_INDEX_ASSIGN,
    OP_EMBEDDED_CMD,
    OP_SHELL_CMD,
    OP_FLAGS,
    OP_CMD_ASSIGN,
    OP_INTERPRET_NODE,
    OP_REPEAT_LINE
} OpCode;

typedef struct {
    OpCode op;
    int operand;
    int operand2;
} Instruction;

typedef struct Chunk {
    Value *constants;
    int const_count, const_cap;

    char **local_names;
    int *local_types;   // tipo de cada local (TOK_INT, TOK_LIST, etc.) 0 = sin tipo fijo
    int local_count;

    Instruction *code;
    int code_count, code_cap;

    int param_count;
} Chunk;

#endif
