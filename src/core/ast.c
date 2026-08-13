/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: core/ast.c
*/

#include <stdlib.h>
#include "ast.h"
#include "memory.h"

ASTNode *node_create(int kind, int line) {
    ASTNode *n = infernal_calloc(1, sizeof(ASTNode));
    n->kind = kind;
    n->line = line;
    return n;
}

void nodelist_add(NodeList *list, ASTNode *node) {
    if (list->count >= list->cap) {
        list->cap = list->cap == 0 ? 8 : list->cap * 2;
        list->stmts = infernal_realloc(list->stmts, list->cap * sizeof(ASTNode*));
    }
    list->stmts[list->count++] = node;
}
