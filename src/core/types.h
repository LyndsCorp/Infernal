/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License, Lynds Corp., Aros Legendarios, David Baña Szymaniak.
 * Código fuente de Infernal: core/types.h
*/

#ifndef CORE_TYPES_H
#define CORE_TYPES_H

#include <stdbool.h>
#include <setjmp.h>

/* ─── Forward declarations ────────────────────────────────── */
typedef struct Chunk Chunk;

/* ─── Token types ────────────────────────────────────────── */
typedef enum {
    TOK_EOF, TOK_NEWLINE, TOK_IDENT, TOK_NUMBER, TOK_STRING_LITERAL,
    TOK_EQ, TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_EEQ, TOK_NEQ, TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACKET, TOK_RBRACKET,
    TOK_LBRACE, TOK_RBRACE,
    TOK_SEMI, TOK_COMMA, TOK_PIPE, TOK_GT_OP, TOK_LT_OP, TOK_GGT,
    TOK_IF, TOK_THEN, TOK_FI, TOK_ELSE, TOK_ELSEIF,
    TOK_WHILE, TOK_FOR,
    TOK_FUNCTION, TOK_RETURN, TOK_BREAK, TOK_REPEAT, TOK_CONTINUE,
    TOK_IMPORT, TOK_TRY, TOK_CATCH,
    TOK_INT, TOK_FLOAT, TOK_BOOL, TOK_STRING, TOK_LIST,
    TOK_TRUE, TOK_FALSE,
    TOK_LOCAL, TOK_GLOBAL,
    TOK_AND, TOK_OR,
    TOK_IN,
    TOK_FLAG,
    TOK_BANG,
    TOK_AT,
    TOK_LINE,
    TOK_COLON          /* <-- NUEVO: para slices */
} TokenType;

/* ─── Token ──────────────────────────────────────────────── */
typedef struct {
    TokenType type;
    char *lexeme;
    int line;
    int start_col;
    int end_col;
} Token;

/* ─── AST nodes ──────────────────────────────────────────── */
typedef struct ASTNode ASTNode;
typedef struct {
    ASTNode **stmts;
    int count, cap;
} NodeList;

typedef struct {
    char **names;
    int name_count;
    int vtype;
    char *var_name;
    Token *body_tokens;
    int body_count;
    bool catch_all;
    bool is_empty;
    bool is_global;      /* <-- NUEVO: si la variable se define en ámbito superglobal */
} FlagSpec;

/* ─── Value types ────────────────────────────────────────── */
#define VAL_NULL      0
#define VAL_INT       1
#define VAL_FLOAT     2
#define VAL_BOOL      3
#define VAL_STRING    4
#define VAL_LIST      5
#define VAL_REFERENCE 6
#define VAL_PTR       7

typedef struct Value Value;
struct Value {
    int type;
    union {
        int ival;
        double fval;
        bool bval;
        char *sval;
        struct {
            Value *items;
            int count, cap;
        } list;
        struct {
            char *list_name;
            int index;
        } ref;
        void *ptr;
    } data;
};

typedef Value (*BuiltinFunc)(int argc, Value *args);

/* ─── Función objeto (con código compilado) ───────────────── */
typedef struct FuncObject {
    enum { FUNC_USER, FUNC_BUILTIN } kind;
    union {
        ASTNode *def;
        BuiltinFunc builtin;
    };
    Chunk *code;
} FuncObject;

/* ─── AST node structure ──────────────────────────────────── */
struct ASTNode {
    int line;
    enum {
        NODE_PROGRAM, NODE_EXPR_STMT, NODE_CMD_STMT, NODE_SHELL_CMD, NODE_ASSIGN,
        NODE_IF, NODE_WHILE, NODE_FOR, NODE_FUNC_DEF, NODE_RETURN,
        NODE_BREAK, NODE_CONTINUE, NODE_REPEAT, NODE_IMPORT, NODE_TRY,
        NODE_VAR, NODE_LITERAL, NODE_BINOP, NODE_CALL, NODE_INDEX,
        NODE_FLAGS, NODE_LIST, NODE_FOR_IN, NODE_PORTAL,
        NODE_SLICE
    } kind;
    union {
        struct { NodeList stmts; } prog;
        struct { ASTNode *expr; } expr_stmt;
        struct { char *cmd; } cmd_stmt;
        struct { char *cmd; } shell_cmd;
        struct { char *name; ASTNode *value; bool is_cmd; char *cmd_str; int vtype;
            bool is_local; bool is_global; ASTNode *lhs_index; } assign;
            struct { ASTNode *cond; NodeList then_block, else_block; } if_stmt;
            struct { ASTNode *cond; NodeList body; } while_stmt;
            struct { char *var; int vtype; ASTNode *init, *cond, *incr; NodeList body; } for_stmt;
            struct { char *name; char **params; int *ptypes; int param_count; NodeList body; } func;
            struct { ASTNode *expr; } ret;
            struct { char *path; NodeList module_block; } import;
            struct { NodeList try_block, catch_block; } try_stmt;
            struct { char *name; } var;
            struct { int type; int ival; double fval; int bval; char *sval; } lit;
            struct { int op; ASTNode *left, *right; } binop;
            struct { char *name; ASTNode **args; int argc; } call;
            struct { ASTNode *list, *index; } idx;
            struct { int mode; FlagSpec *specs; int spec_count; } flags;
            struct { ASTNode **items; int count; } list_lit;
            struct { char *var; ASTNode *list_expr; NodeList body; } for_in;
            struct {
                ASTNode *line_expr;
                char *portal_name;
            } repeat;
            struct { char *name; bool is_local; } portal;
            struct {
                ASTNode *list;
                int mode;
                int start;
                int end;
            } slice;
    } data;
};

#endif
