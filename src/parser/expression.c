/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: parser/expression.c
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "expression.h"
#include "core/ast.h"
#include "lexer/lexer.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "parser.h"
#include "developer/debug.h"

/* --- LIMPIEZA DE NOMBRE DE VARIABLE (elimina $ o ?) --- */
static char *clean_var_name(const char *raw) {
    if (!raw) return NULL;
    if (raw[0] == '$' || raw[0] == '?') {
        return strdup(raw + 1);
    }
    return strdup(raw);
}

/* --- Prototipos de funciones estáticas internas --- */
static ASTNode *parse_map_literal(int line);
static ASTNode *parse_postfix(void);
static ASTNode *parse_power(void);
static ASTNode *parse_unary(void);
static ASTNode *parse_term(void);
static ASTNode *parse_expr(void);
static ASTNode *parse_comparison(void);
static ASTNode *parse_logic_and(void);
static ASTNode *parse_logic_or(void);

/* --- parse_slice_content (no static, declarada en .h) --- */
ASTNode *parse_slice_content(int line) {
    Token t = ts_peek();
    int mode = -1;
    int start = 0, end = 0;

    DEBUG_INFO("parse_slice_content: token actual '%s' (tipo %d)", t.lexeme, t.type);

    if (t.type == TOK_STAR) {
        ts_advance();
        if (ts_peek().type == TOK_RBRACKET) {
            mode = 5; start = 0; end = 0;
        } else if (ts_peek().type == TOK_NUMBER) {
            Token num = ts_advance();
            if (strchr(num.lexeme, '.') != NULL) {
                error(line, "Índice inválido: no se permiten números decimales");
            }
            int val = atoi(num.lexeme);
            if (val < 1) error(line, "Índice inválido: debe ser positivo");
            if (ts_peek().type == TOK_STAR) {
                ts_advance();
                mode = 4;
                start = val;
                end = val;
            } else {
                mode = 3;
                start = -1;
                end = val;
            }
        } else {
            error(line, "Se esperaba número o ']' después de '*'");
        }
    } else if (t.type == TOK_NUMBER) {
        Token num = ts_advance();
        if (strchr(num.lexeme, '.') != NULL) {
            error(line, "Índice inválido: no se permiten números decimales");
        }
        int val = atoi(num.lexeme);
        if (val < 1) error(line, "Índice inválido: debe ser positivo");

        Token next = ts_peek();
        if (next.type == TOK_RBRACKET) {
            mode = 0;
            start = val;
            end = val;
        } else if (next.type == TOK_COLON) {
            ts_advance();
            Token next2 = ts_peek();
            if (next2.type == TOK_NUMBER) {
                Token num2 = ts_advance();
                if (strchr(num2.lexeme, '.') != NULL) {
                    error(line, "Índice inválido: no se permiten números decimales");
                }
                int val2 = atoi(num2.lexeme);
                if (val2 < 1) error(line, "Índice inválido: debe ser positivo");
                mode = 1;
                start = val;
                end = val2;
                if (start > end) {
                    error(line, "Rango invertido: [%d:%d] – quizás te referías a [%d:%d]?",
                          start, end, end, start);
                }
            } else {
                error(line, "Se esperaba un número después de ':'");
            }
        } else if (next.type == TOK_STAR) {
            ts_advance();
            mode = 2;
            start = val;
            end = -1;
        } else {
            error(line, "Token inesperado '%s' en slice", next.lexeme);
        }
    } else {
        error(line, "Se esperaba '*' o un número en el slice");
    }

    if (!ts_match(TOK_RBRACKET)) {
        error(line, "Se esperaba ']' al final del slice");
    }

    ASTNode *node = node_create(NODE_SLICE, line);
    node->data.slice.mode = mode;
    node->data.slice.start = start;
    node->data.slice.end = end;
    node->data.slice.list = NULL;
    DEBUG_INFO("parse_slice_content: creado NODE_SLICE modo %d, start=%d, end=%d", mode, start, end);
    return node;
}

/* --- parse_index_or_slice (declarada en .h) --- */
ASTNode *parse_index_or_slice(int line) {
    Token t = ts_peek();
    ASTNode *node = NULL;

    if (t.type == TOK_STAR || (t.type == TOK_NUMBER && (ts.tokens[ts.pos+1].type == TOK_COLON || ts.tokens[ts.pos+1].type == TOK_STAR))) {
        node = parse_slice_content(line);
    } else {
        ASTNode *idx = parse_expression(0);
        if (!ts_match(TOK_RBRACKET)) error(line, "Se esperaba ']'");
        node = node_create(NODE_INDEX, line);
        node->data.idx.index = idx;
    }
    return node;
}

/* --- parse_map_literal (interna) --- */
static ASTNode *parse_map_literal(int line) {
    ASTNode *node = node_create(NODE_MAP, line);
    node->data.map.pairs = NULL;
    node->data.map.pair_count = 0;

    ts_skip_newlines();
    if (ts_peek().type == TOK_RBRACKET) {
        ts_advance();
        return node;
    }

    do {
        ts_skip_newlines();
        ASTNode *key = parse_expression(0);
        // --- NUEVA COMPROBACIÓN: si el siguiente token es ':' ---
        if (ts_peek().type == TOK_COLON) {
            error(line, "En los mapas, separa clave y valor con `=`, no con `:`.");
        }
        if (!ts_match(TOK_EQ)) {
            error(line, "Se esperaba '=' en par clave-valor de mapa");
        }
        ASTNode *value = parse_expression(0);
        node->data.map.pairs = realloc(node->data.map.pairs,
                                       (node->data.map.pair_count + 1) * sizeof(*node->data.map.pairs));
        node->data.map.pairs[node->data.map.pair_count].key = key;
        node->data.map.pairs[node->data.map.pair_count].value = value;
        node->data.map.pair_count++;
        ts_skip_newlines();
    } while (ts_match(TOK_COMMA));

    if (!ts_match(TOK_RBRACKET)) {
        error(line, "Se esperaba ']' al final del mapa");
    }
    return node;
}

/* --- parse_primary (declarada en .h) --- */
ASTNode *parse_primary() {
    Token t = ts_peek();
    DEBUG_INFO("parse_primary: token '%s' (tipo %d)", t.lexeme, t.type);

    if (t.type == TOK_NUMBER) {
        ts_advance();
        ASTNode *n = node_create(NODE_LITERAL, t.line);
        if (strchr(t.lexeme, '.') || strchr(t.lexeme, 'e') || strchr(t.lexeme, 'E')) {
            n->data.lit.type = TOK_FLOAT;
            n->data.lit.fval = atof(t.lexeme);
        } else {
            n->data.lit.type = TOK_INT;
            n->data.lit.ival = atoi(t.lexeme);
        }
        return n;
    }
    if (t.type == TOK_STRING_LITERAL) {
        ts_advance();
        ASTNode *n = node_create(NODE_LITERAL, t.line);
        n->data.lit.type = TOK_STRING;
        n->data.lit.sval = strdup(t.lexeme);
        while (ts_peek().type == TOK_LBRACKET) {
            Token lb = ts_advance();
            Token next = ts_peek();
            if (next.type == TOK_STAR || (next.type == TOK_NUMBER && (ts.tokens[ts.pos+1].type == TOK_COLON || ts.tokens[ts.pos+1].type == TOK_STAR))) {
                ASTNode *slice = parse_slice_content(lb.line);
                slice->data.slice.list = n;
                n = slice;
            } else {
                ASTNode *idx = parse_expression(0);
                if (!ts_match(TOK_RBRACKET)) error(lb.line, "Se esperaba ']'");
                ASTNode *ni = node_create(NODE_INDEX, lb.line);
                ni->data.idx.list = n;
                ni->data.idx.index = idx;
                n = ni;
            }
        }
        return n;
    }
    if (t.type == TOK_TRUE || t.type == TOK_FALSE) {
        ts_advance();
        ASTNode *n = node_create(NODE_LITERAL, t.line);
        n->data.lit.type = TOK_BOOL;
        n->data.lit.bval = (t.type == TOK_TRUE) ? 1 : 0;
        return n;
    }
    if (t.type == TOK_IDENT) {
        bool is_call = (ts.pos + 1 < ts.count && ts.tokens[ts.pos + 1].type == TOK_LPAREN);
        if (is_call) {
            ts_advance();
            char *func_name = strdup(t.lexeme);

            if (strcmp(func_name, "exited") == 0) {
                ts_advance();
                int start_pos = ts.pos;
                int depth = 1;
                while (depth > 0 && ts.pos < ts.count) {
                    Token tok = ts_advance();
                    if (tok.type == TOK_LPAREN) depth++;
                    else if (tok.type == TOK_RPAREN) depth--;
                }
                int end_pos = ts.pos - 1;
                char *cmd = build_command_from_tokens(start_pos, end_pos);
                ASTNode *n = node_create(NODE_CALL, t.line);
                n->data.call.name = func_name;
                n->data.call.argc = 1;
                n->data.call.args = malloc(sizeof(ASTNode*));
                ASTNode *lit = node_create(NODE_LITERAL, t.line);
                lit->data.lit.type = TOK_STRING;
                lit->data.lit.sval = cmd;
                n->data.call.args[0] = lit;
                return n;
            }

            ASTNode *n = node_create(NODE_CALL, t.line);
            n->data.call.name = func_name;
            n->data.call.argc = 0;
            n->data.call.args = NULL;
            ts_advance();
            if (!ts_match(TOK_RPAREN)) {
                do {
                    n->data.call.args = realloc(n->data.call.args,
                                                (n->data.call.argc + 1) * sizeof(ASTNode*));
                    n->data.call.args[n->data.call.argc++] = parse_expression(0);
                } while (ts_match(TOK_COMMA));
                if (!ts_match(TOK_RPAREN))
                    error(t.line, "Se esperaba ')' en la llamada a función '%s'", func_name);
            }
            return n;
        } else {
            ts_advance();
            ASTNode *n = node_create(NODE_VAR, t.line);
            /* $ marca una copia temporal (la información sintáctica se conserva
             * para que $x++ pueda comportarse distinto de x++). */
            n->data.var.clone = (t.lexeme[0] == '$');
            char *cleaned = clean_var_name(t.lexeme);
            n->data.var.name = cleaned;

            // --- NUEVA COMPROBACIÓN: si el siguiente token es '{' ---
            if (ts_peek().type == TOK_LBRACE) {
                error(t.line, "Para acceder a elementos de una lista o mapa, usa corchetes `[]`, no llaves `{}`.");
            }

            while (ts_peek().type == TOK_LBRACKET) {
                Token lb = ts_advance();
                Token next = ts_peek();
                if (next.type == TOK_STAR || (next.type == TOK_NUMBER && (ts.tokens[ts.pos+1].type == TOK_COLON || ts.tokens[ts.pos+1].type == TOK_STAR))) {
                    ASTNode *slice = parse_slice_content(lb.line);
                    slice->data.slice.list = n;
                    n = slice;
                } else {
                    ASTNode *idx = parse_expression(0);
                    if (!ts_match(TOK_RBRACKET)) error(lb.line, "Se esperaba ']'");
                    ASTNode *ni = node_create(NODE_INDEX, lb.line);
                    ni->data.idx.list = n;
                    ni->data.idx.index = idx;
                    n = ni;
                }
            }
            return n;
        }
    }
    if (t.type == TOK_LBRACKET) {
        ts_advance();
        ts_skip_newlines();

        int saved_pos = ts.pos;
        int is_map = 0;

        Token first = ts_peek();
        if (first.type == TOK_IDENT || first.type == TOK_STRING_LITERAL) {
            ts_advance();
            if (ts_peek().type == TOK_EQ) {
                is_map = 1;
            }
        }
        ts.pos = saved_pos;

        if (is_map) {
            return parse_map_literal(t.line);
        } else {
            ASTNode *n = node_create(NODE_LIST, t.line);
            n->data.list_lit.items = NULL;
            n->data.list_lit.count = 0;
            if (!ts_match(TOK_RBRACKET)) {
                do {
                    ts_skip_newlines();
                    n->data.list_lit.items = realloc(n->data.list_lit.items,
                                                     (n->data.list_lit.count + 1) * sizeof(ASTNode*));
                    n->data.list_lit.items[n->data.list_lit.count++] = parse_expression(0);
                    ts_skip_newlines();
                } while (ts_match(TOK_COMMA));
                ts_skip_newlines();
                if (!ts_match(TOK_RBRACKET)) error(t.line, "Se esperaba ']'");
            }
            return n;
        }
    }
    if (t.type == TOK_LBRACE) {
        ts_advance();
        if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba nombre de variable tras '{'");
        char *name = strdup(ts_advance().lexeme);
        if (!ts_match(TOK_RBRACE)) error(t.line, "Se esperaba '}'");
        ASTNode *n = node_create(NODE_VAR, t.line);
        n->data.var.name = name;
        n->data.var.clone = false;
        return n;
    }
    if (t.type == TOK_LPAREN) {
        ts_advance();
        ASTNode *n = parse_expression(0);
        if (!ts_match(TOK_RPAREN)) error(t.line, "Se esperaba ')'");
        return n;
    }
    error(t.line, "Token inesperado '%s'", t.lexeme);
    return NULL;
}

/* --- parse_postfix (interna) --- */
static ASTNode *parse_postfix(void) {
    ASTNode *expr = parse_primary();
    while (1) {
        if (ts_peek().type == TOK_INC || ts_peek().type == TOK_DEC) {
            Token op = ts_advance();
            if (expr->kind != NODE_VAR) {
                error(expr->line, "Solo se puede incrementar/decrementar una variable");
            }
            /* Se permite cualquier nombre de variable, incluyendo $ y ? */
            ASTNode *node = node_create((op.type == TOK_INC) ? NODE_POST_INC : NODE_POST_DEC, expr->line);
            node->data.post_op.var = expr;
            node->data.post_op.statement_context = false;
            expr = node;
        } else {
            break;
        }
    }
    return expr;
}

/* --- parse_power (interna) --- */
static ASTNode *parse_power(void) {
    ASTNode *left = parse_postfix();
    if (ts_match(TOK_POW)) {
        ASTNode *right = parse_unary();  // asociatividad derecha
        ASTNode *node = node_create(NODE_BINOP, left->line);
        node->data.binop.op = TOK_POW;
        node->data.binop.left = left;
        node->data.binop.right = right;
        left = node;
    }
    return left;
}

/* --- parse_unary (interna) --- */
static ASTNode *parse_unary() {
    if (ts_match(TOK_MINUS)) {
        ASTNode *operand = parse_unary();
        ASTNode *node = node_create(NODE_BINOP, operand->line);
        node->data.binop.op = TOK_MINUS;
        node->data.binop.left = node_create(NODE_LITERAL, operand->line);
        node->data.binop.left->data.lit.type = TOK_INT;
        node->data.binop.left->data.lit.ival = 0;
        node->data.binop.right = operand;
        return node;
    }
    if (ts_match(TOK_NOT)) {
        ASTNode *operand = parse_unary();
        ASTNode *node = node_create(NODE_UNARY, operand->line);
        node->data.unary.op = TOK_NOT;
        node->data.unary.operand = operand;
        return node;
    }
    return parse_power();
}

/* --- parse_term (interna) --- */
static ASTNode *parse_term() {
    ASTNode *left = parse_unary();
    while (ts_peek().type == TOK_STAR || ts_peek().type == TOK_SLASH || ts_peek().type == TOK_PERCENT) {
        Token op = ts_advance();
        ASTNode *right = parse_unary();
        ASTNode *n = node_create(NODE_BINOP, op.line);
        n->data.binop.op = op.type;
        n->data.binop.left = left;
        n->data.binop.right = right;
        left = n;
    }
    return left;
}

/* --- parse_expr (interna) --- */
static ASTNode *parse_expr() {
    ASTNode *left = parse_term();
    while (ts_peek().type == TOK_PLUS || ts_peek().type == TOK_MINUS) {
        Token op = ts_advance();
        DEBUG_INFO("parse_expr: operador '%s', siguiente token: '%s' (tipo %d)", op.lexeme, ts_peek().lexeme, ts_peek().type);

        if (op.type == TOK_MINUS && ts_peek().type == TOK_LBRACKET) {
            DEBUG_INFO("parse_expr: detectado '- [' -> slice");
            ts_advance();
            ASTNode *slice = parse_slice_content(op.line);
            slice->data.slice.list = left;
            ASTNode *n = node_create(NODE_BINOP, op.line);
            n->data.binop.op = TOK_MINUS;
            n->data.binop.left = left;
            n->data.binop.right = slice;
            left = n;
            DEBUG_INFO("parse_expr: creado NODE_BINOP con NODE_SLICE");
            break;
        } else {
            ASTNode *right = parse_term();
            ASTNode *n = node_create(NODE_BINOP, op.line);
            n->data.binop.op = op.type;
            n->data.binop.left = left;
            n->data.binop.right = right;
            left = n;
            DEBUG_INFO("parse_expr: creado NODE_BINOP normal con operador %d", op.type);
        }
    }
    return left;
}

/* --- parse_comparison (interna) --- */
static ASTNode *parse_comparison() {
    ASTNode *left = parse_expr();
    TokenType op = ts_peek().type;
    if (op == TOK_EEQ || op == TOK_NEQ || op == TOK_LT_OP || op == TOK_GT_OP ||
        op == TOK_LE || op == TOK_GE) {
        Token t = ts_advance();
    ASTNode *right = parse_expr();
    ASTNode *n = node_create(NODE_BINOP, t.line);
    n->data.binop.op = t.type;
    n->data.binop.left = left;
    n->data.binop.right = right;
    return n;
        }
        return left;
}

/* --- parse_logic_and (interna) --- */
static ASTNode *parse_logic_and() {
    ASTNode *left = parse_comparison();
    while (ts_peek().type == TOK_AND) {
        Token op = ts_advance();
        ASTNode *right = parse_comparison();
        ASTNode *n = node_create(NODE_BINOP, op.line);
        n->data.binop.op = TOK_AND;
        n->data.binop.left = left;
        n->data.binop.right = right;
        left = n;
    }
    return left;
}

/* --- parse_logic_or (interna) --- */
static ASTNode *parse_logic_or() {
    ASTNode *left = parse_logic_and();
    while (ts_peek().type == TOK_OR) {
        Token op = ts_advance();
        ASTNode *right = parse_logic_and();
        ASTNode *n = node_create(NODE_BINOP, op.line);
        n->data.binop.op = TOK_OR;
        n->data.binop.left = left;
        n->data.binop.right = right;
        left = n;
    }
    return left;
}

/* --- parse_expression (punto de entrada, declarada en .h) --- */
ASTNode *parse_expression(int dummy) {
    (void)dummy;
    return parse_logic_or();
}
