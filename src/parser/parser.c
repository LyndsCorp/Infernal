/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: parser/parser.c
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "parser.h"
#include "core/ast.h"
#include "lexer/lexer.h"
#include "lexer/keywords.h"
#include "expression.h"
#include "flags.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "embedded/embedded.h"
#include "vm/compiler.h"
#include "developer/debug.h"

/* --- Funciones auxiliares --- */
static char *strip_quotes(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    if (len >= 2 && (s[0] == '"' || s[0] == '\'') && s[0] == s[len - 1]) {
        char *result = malloc(len - 1);
        if (!result) return strdup(s);
        memcpy(result, s + 1, len - 2);
        result[len - 2] = '\0';
        return result;
    }
    return strdup(s);
}

static char *extract_literal_command(int line) {
    char *raw = extract_command_string(line);
    if (!raw) return strdup("");
    char *p = raw;
    while ((p = strstr(p, "??")) != NULL) {
        p[0] = '$';
        p[1] = '$';
        p += 2;
    }
    return raw;
}

static char *clean_var_name(const char *raw) {
    if (!raw) return NULL;
    if (raw[0] == '$' || raw[0] == '?') {
        return strdup(raw + 1);
    }
    return strdup(raw);
}

static void validate_var_name(const char *name, int line) {
    const char invalid[] = "@[](){}";
    for (const char *p = name; *p; p++)
        if (strchr(invalid, *p)) error(line, "Carácter inválido '%c' en nombre de variable", *p);
}

static bool valid_module_name(const char *name) {
    if (!name || !*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (const char *p = name; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return false;
    }
    return true;
}

static bool safe_module_path(const char *path) {
    return path && *path && !strstr(path, "..") && !strchr(path, '\n') && !strchr(path, '\r');
}

char *build_command_from_tokens(int start_pos, int end_pos) {
    if (start_pos >= end_pos) return strdup("");
    char *cmd = malloc(1);
    cmd[0] = '\0';
    size_t len = 0;

    for (int i = start_pos; i < end_pos; i++) {
        Token *t = &ts.tokens[i];
        if (t->type == TOK_NEWLINE || t->type == TOK_EOF) break;

        if (i > start_pos) {
            int prev_end = ts.tokens[i-1].end_col;
            if (t->start_col > prev_end) {
                cmd = realloc(cmd, len + 2);
                cmd[len++] = ' ';
                cmd[len] = '\0';
            }
        }

        size_t tlen = strlen(t->lexeme);
        cmd = realloc(cmd, len + tlen + 1);
        memcpy(cmd + len, t->lexeme, tlen);
        len += tlen;
        cmd[len] = '\0';
    }
    return cmd;
}

ASTNode *parse_if_statement() {
    Token t = ts_peek();
    int line = t.line;
    ts_advance();
    ASTNode *cond = parse_expression(0);
    if (!ts_match(TOK_THEN)) error_missing_then(line, "if");
    NodeList then_block = parse_block("fi");
    ASTNode *first_if = node_create(NODE_IF, line);
    first_if->data.if_stmt.cond = cond;
    first_if->data.if_stmt.then_block = then_block;
    first_if->data.if_stmt.else_block = (NodeList){NULL,0,0};
    ASTNode *current_if = first_if;
    while (ts_peek().type == TOK_ELSEIF) {
        ts_advance();
        ASTNode *elseif_cond = parse_expression(0);
        if (!ts_match(TOK_THEN)) error_missing_then(line, "elseif");
        NodeList elseif_then = parse_block("fi");
        ASTNode *elseif_node = node_create(NODE_IF, line);
        elseif_node->data.if_stmt.cond = elseif_cond;
        elseif_node->data.if_stmt.then_block = elseif_then;
        elseif_node->data.if_stmt.else_block = (NodeList){NULL,0,0};
        NodeList wrapper = {NULL, 0, 0};
        nodelist_add(&wrapper, elseif_node);
        current_if->data.if_stmt.else_block = wrapper;
        current_if = elseif_node;
    }
    if (ts_match(TOK_ELSE)) {
        NodeList else_block = parse_block("fi");
        current_if->data.if_stmt.else_block = else_block;
    }
    if (!ts_match(TOK_FI)) error(line, "Se esperaba 'fi' al final del bloque if");
    return first_if;
}

ASTNode *parse_assignment_expr(int line) {
    DEBUG_INFO("parse_assignment_expr: ¡LLAMADA! línea %d", line);
    if (ts_peek().type != TOK_IDENT) error(line, "Se esperaba variable en la asignación");
    Token var_tok = ts_advance();
    char *varname = clean_var_name(var_tok.lexeme);
    validate_var_name(varname, line);

    int op = ts_peek().type;
    if (op != TOK_EQ && op != TOK_PLUS_EQ && op != TOK_MINUS_EQ &&
        op != TOK_STAR_EQ && op != TOK_SLASH_EQ) {
        error(line, "Se esperaba operador de asignación (=, +=, -=, *=, /=)");
        }
        ts_advance(); // consumir el operador

        bool is_cmd = false;
    char *cmd_str = NULL;
    ASTNode *value = NULL;

    // Solo para '=' simple, detectar si es un comando
    if (op == TOK_EQ) {
        Token next_token = ts_peek();
        DEBUG_INFO("parse_assignment_expr: valor siguiente '%s' (tipo %d)", next_token.lexeme, next_token.type);
        if (next_token.type == TOK_IDENT && next_token.lexeme[0] != '$' && next_token.lexeme[0] != '?') {
            int save_pos = ts.pos;
            ts_advance();
            Token next_next = ts_peek();
            ts.pos = save_pos;
            if (next_next.type != TOK_LPAREN && next_next.type != TOK_LBRACKET) {
                is_cmd = true;
                cmd_str = extract_literal_command(line);
                DEBUG_INFO("parse_assignment_expr: comando detectado: '%s'", cmd_str);
                while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                    ts_advance();
                }
                value = NULL;
            }
        }
    }

    if (!is_cmd) {
        DEBUG_INFO("parse_assignment_expr: parseando expresión normal");
        value = parse_expression(0);
        if (value) {
            DEBUG_INFO("parse_assignment_expr: expresión parseada, tipo nodo %d", value->kind);
        } else {
            DEBUG_INFO("parse_assignment_expr: parse_expression devolvió NULL, creando literal 0");
            value = node_create(NODE_LITERAL, line);
            value->data.lit.type = TOK_INT;
            value->data.lit.ival = 0;
        }
    }

    if (op == TOK_EQ) {
        ASTNode *assign = node_create(NODE_ASSIGN, line);
        assign->data.assign.name = varname;
        assign->data.assign.value = value;
        assign->data.assign.vtype = 0;
        assign->data.assign.is_local = false;
        assign->data.assign.is_global = false;
        assign->data.assign.is_cmd = is_cmd;
        assign->data.assign.cmd_str = cmd_str;
        assign->data.assign.lhs_index = NULL;
        DEBUG_INFO("parse_assignment_expr: creado NODE_ASSIGN para '%s', is_cmd=%d", varname, is_cmd);
        return assign;
    } else {
        // asignación compuesta
        if (is_cmd) {
            error(line, "No se puede usar un comando en una asignación compuesta");
        }
        ASTNode *var_node = node_create(NODE_VAR, line);
        var_node->data.var.name = strdup(varname);
        ASTNode *binop = node_create(NODE_BINOP, line);
        binop->data.binop.left = var_node;
        binop->data.binop.right = value;
        switch (op) {
            case TOK_PLUS_EQ:  binop->data.binop.op = TOK_PLUS;  break;
            case TOK_MINUS_EQ: binop->data.binop.op = TOK_MINUS; break;
            case TOK_STAR_EQ:  binop->data.binop.op = TOK_STAR;  break;
            case TOK_SLASH_EQ: binop->data.binop.op = TOK_SLASH; break;
            default: break;
        }
        ASTNode *assign = node_create(NODE_ASSIGN, line);
        assign->data.assign.name = varname;
        assign->data.assign.value = binop;
        assign->data.assign.vtype = 0;
        assign->data.assign.is_local = false;
        assign->data.assign.is_global = false;
        assign->data.assign.is_cmd = false;
        assign->data.assign.cmd_str = NULL;
        assign->data.assign.lhs_index = NULL;
        return assign;
    }
}

NodeList parse_block(const char *terminator) {
    NodeList block = {NULL, 0, 0};
    DEBUG_INFO("=== parse_block: iniciando bloque, terminator='%s' ===", terminator ? terminator : "NULL");

    while (1) {
        ts_skip_newlines();
        Token t = ts_peek();
        if (t.type == TOK_EOF) break;
        if (terminator && lookup_keyword(terminator) == t.type) break;
        if (terminator && strcmp(terminator, "fi") == 0 &&
            (t.type == TOK_ELSE || t.type == TOK_ELSEIF)) break;
        if (terminator && strcmp(terminator, "}") == 0 && t.type == TOK_RBRACE) break;

        DEBUG_INFO("parse_block: token actual '%s' (tipo %d) en línea %d", t.lexeme, t.type, t.line);

        ASTNode *stmt = NULL;

        /* --- Comando embebido con ! --- */
        if (t.type == TOK_BANG) {
            ts_advance();
            char cmd[4096] = {0};
            Token prev_token = {TOK_EOF, "", 0, 0, 0};
            while (ts_peek().type != TOK_BANG && ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                Token ct = ts_advance();
                bool hay_espacio = false;
                if (prev_token.type != TOK_EOF) {
                    if (ct.start_col > prev_token.end_col) {
                        hay_espacio = true;
                    }
                }
                if (hay_espacio && cmd[0] != '\0') strcat(cmd, " ");
                if (ct.type == TOK_STRING_LITERAL) {
                    strcat(cmd, "\"");
                    strcat(cmd, ct.lexeme);
                    strcat(cmd, "\"");
                } else {
                    strcat(cmd, ct.lexeme);
                }
                prev_token = ct;
            }
            if (ts_peek().type == TOK_BANG) ts_advance();
            stmt = node_create(NODE_CMD_STMT, t.line);
            stmt->data.cmd_stmt.cmd = strdup(cmd);
            while (ts_match(TOK_OR)) {
                ASTNode *next_stmt = NULL;
                Token next = ts_peek();
                if (next.type == TOK_BANG) {
                    ts_advance();
                    char cmd2[4096] = {0};
                    Token prev2 = {TOK_EOF, "", 0, 0, 0};
                    while (ts_peek().type != TOK_BANG && ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                        Token ct2 = ts_advance();
                        bool hay_espacio2 = false;
                        if (prev2.type != TOK_EOF) {
                            if (ct2.start_col > prev2.end_col) hay_espacio2 = true;
                        }
                        if (hay_espacio2 && cmd2[0] != '\0') strcat(cmd2, " ");
                        if (ct2.type == TOK_STRING_LITERAL) {
                            strcat(cmd2, "\"");
                            strcat(cmd2, ct2.lexeme);
                            strcat(cmd2, "\"");
                        } else {
                            strcat(cmd2, ct2.lexeme);
                        }
                        prev2 = ct2;
                    }
                    if (ts_peek().type == TOK_BANG) ts_advance();
                    next_stmt = node_create(NODE_CMD_STMT, next.line);
                    next_stmt->data.cmd_stmt.cmd = strdup(cmd2);
                } else {
                    char cmd2[4096] = {0};
                    Token first2 = ts_advance();
                    strcpy(cmd2, first2.lexeme);
                    Token prev2 = first2;
                    while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF && ts_peek().type != TOK_OR) {
                        Token ct2 = ts_advance();
                        bool hay_espacio2 = false;
                        if (prev2.type != TOK_EOF) {
                            if (ct2.start_col > prev2.end_col) hay_espacio2 = true;
                        }
                        if (hay_espacio2) strcat(cmd2, " ");
                        if (ct2.type == TOK_STRING_LITERAL) {
                            strcat(cmd2, "\"");
                            strcat(cmd2, ct2.lexeme);
                            strcat(cmd2, "\"");
                        } else {
                            strcat(cmd2, ct2.lexeme);
                        }
                        prev2 = ct2;
                    }
                    next_stmt = node_create(NODE_SHELL_CMD, next.line);
                    next_stmt->data.shell_cmd.cmd = strdup(cmd2);
                }
                ASTNode *try_node = node_create(NODE_TRY, t.line);
                try_node->data.try_stmt.try_block = (NodeList){NULL, 0, 0};
                nodelist_add(&try_node->data.try_stmt.try_block, stmt);
                try_node->data.try_stmt.catch_block = (NodeList){NULL, 0, 0};
                nodelist_add(&try_node->data.try_stmt.catch_block, next_stmt);
                stmt = try_node;
            }
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_CMD_STMT en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- Comando shell con cadena literal --- */
        if (t.type == TOK_STRING_LITERAL) {
            char cmd[4096] = {0};
            Token first = ts_advance();
            strcat(cmd, "\"");
            strcat(cmd, first.lexeme);
            strcat(cmd, "\"");
            Token prev_token = first;
            while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                Token ct = ts_advance();
                bool hay_espacio = false;
                if (prev_token.type != TOK_EOF) {
                    if (ct.start_col > prev_token.end_col) hay_espacio = true;
                }
                if (hay_espacio) strcat(cmd, " ");
                if (ct.type == TOK_STRING_LITERAL) {
                    strcat(cmd, "\"");
                    strcat(cmd, ct.lexeme);
                    strcat(cmd, "\"");
                } else {
                    strcat(cmd, ct.lexeme);
                }
                prev_token = ct;
            }
            stmt = node_create(NODE_SHELL_CMD, t.line);
            stmt->data.shell_cmd.cmd = strdup(cmd);
            while (ts_match(TOK_OR)) {
                ASTNode *next_stmt = NULL;
                Token next = ts_peek();
                if (next.type == TOK_BANG) {
                    ts_advance();
                    char cmd2[4096] = {0};
                    Token prev2 = {TOK_EOF, "", 0, 0, 0};
                    while (ts_peek().type != TOK_BANG && ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                        Token ct2 = ts_advance();
                        bool hay_espacio2 = false;
                        if (prev2.type != TOK_EOF) {
                            if (ct2.start_col > prev2.end_col) hay_espacio2 = true;
                        }
                        if (hay_espacio2 && cmd2[0] != '\0') strcat(cmd2, " ");
                        if (ct2.type == TOK_STRING_LITERAL) {
                            strcat(cmd2, "\"");
                            strcat(cmd2, ct2.lexeme);
                            strcat(cmd2, "\"");
                        } else {
                            strcat(cmd2, ct2.lexeme);
                        }
                        prev2 = ct2;
                    }
                    if (ts_peek().type == TOK_BANG) ts_advance();
                    next_stmt = node_create(NODE_CMD_STMT, next.line);
                    next_stmt->data.cmd_stmt.cmd = strdup(cmd2);
                } else {
                    char cmd2[4096] = {0};
                    Token first2 = ts_advance();
                    strcpy(cmd2, first2.lexeme);
                    Token prev2 = first2;
                    while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF && ts_peek().type != TOK_OR) {
                        Token ct2 = ts_advance();
                        bool hay_espacio2 = false;
                        if (prev2.type != TOK_EOF) {
                            if (ct2.start_col > prev2.end_col) hay_espacio2 = true;
                        }
                        if (hay_espacio2) strcat(cmd2, " ");
                        if (ct2.type == TOK_STRING_LITERAL) {
                            strcat(cmd2, "\"");
                            strcat(cmd2, ct2.lexeme);
                            strcat(cmd2, "\"");
                        } else {
                            strcat(cmd2, ct2.lexeme);
                        }
                        prev2 = ct2;
                    }
                    next_stmt = node_create(NODE_SHELL_CMD, next.line);
                    next_stmt->data.shell_cmd.cmd = strdup(cmd2);
                }
                ASTNode *try_node = node_create(NODE_TRY, t.line);
                try_node->data.try_stmt.try_block = (NodeList){NULL, 0, 0};
                nodelist_add(&try_node->data.try_stmt.try_block, stmt);
                try_node->data.try_stmt.catch_block = (NodeList){NULL, 0, 0};
                nodelist_add(&try_node->data.try_stmt.catch_block, next_stmt);
                stmt = try_node;
            }
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_SHELL_CMD en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- Portal @ --- */
        if (t.type == TOK_AT) {
            ts_advance();
            if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba nombre de portal después de '@'");
            char *portal_name = strdup(ts_advance().lexeme);
            stmt = node_create(NODE_PORTAL, t.line);
            stmt->data.portal.name = portal_name;
            stmt->data.portal.is_local = false;
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_PORTAL en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- Flags --- */
        if (t.type == TOK_FLAG) {
            ts_advance();
            stmt = parse_flags();
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_FLAGS en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- Execute --- */
        if (t.type == TOK_EXECUTE) {
            ts_advance();
            ASTNode *path_expr = parse_expression(0);
            if (!path_expr) error(t.line, "Se esperaba una ruta de script después de 'execute'");
            int argc = 0;
            char **args = NULL;
            while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                Token tok = ts_advance();
                char *arg = (tok.type == TOK_STRING_LITERAL) ? strip_quotes(tok.lexeme) : strdup(tok.lexeme);
                args = realloc(args, (argc + 1) * sizeof(char*));
                args[argc++] = arg;
            }
            stmt = node_create(NODE_EXECUTE, t.line);
            stmt->data.execute.path_expr = path_expr;
            stmt->data.execute.args = args;
            stmt->data.execute.argc = argc;
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_EXECUTE en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- IF --- */
        if (t.type == TOK_IF) {
            stmt = parse_if_statement();
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_IF en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- WHILE --- */
        if (t.type == TOK_WHILE) {
            ts_advance();
            ASTNode *cond = parse_expression(0);
            if (!ts_match(TOK_THEN)) error_missing_then(t.line, "while");
            NodeList body = parse_block("fi");
            if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi'");
            stmt = node_create(NODE_WHILE, t.line);
            stmt->data.while_stmt.cond = cond;
            stmt->data.while_stmt.body = body;
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_WHILE en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- FOR --- */
        if (t.type == TOK_FOR) {
            ts_advance();

            bool is_local = false, is_global = false;
            int vtype = 0;

            if (ts_peek().type == TOK_LOCAL) { is_local = true; ts_advance(); }
            else if (ts_peek().type == TOK_GLOBAL) { is_global = true; ts_advance(); }

            TokenType tt = ts_peek().type;
            if (tt == TOK_INT || tt == TOK_FLOAT || tt == TOK_BOOL ||
                tt == TOK_STRING || tt == TOK_LIST) {
                vtype = ts_advance().type;
                }

                if (ts_peek().type != TOK_IDENT)
                    error(t.line, "Se esperaba nombre de variable en for");
            char *varname = clean_var_name(ts_advance().lexeme);
            validate_var_name(varname, t.line);

            // FOR-IN
            if (ts_peek().type == TOK_IN) {
                ts_advance();
                ASTNode *list_expr = parse_expression(0);
                if (!ts_match(TOK_THEN)) error_missing_then(t.line, "for-in");
                NodeList body = parse_block("fi");
                if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi'");
                stmt = node_create(NODE_FOR_IN, t.line);
                stmt->data.for_in.var = varname;
                stmt->data.for_in.list_expr = list_expr;
                stmt->data.for_in.body = body;
                nodelist_add(&block, stmt);
                DEBUG_INFO("parse_block: añadido NODE_FOR_IN en línea %d", stmt->line);
                ts_skip_newlines();
                continue;
            }

            // FOR tradicional
            if (!ts_match(TOK_EQ))
                error(t.line, "Se esperaba '=' en la inicialización del for");

            ASTNode *init_expr = parse_expression(0);
            ASTNode *init = node_create(NODE_ASSIGN, t.line);
            init->data.assign.name = varname;
            init->data.assign.value = init_expr;
            init->data.assign.vtype = vtype;
            init->data.assign.is_local = is_local;
            init->data.assign.is_global = is_global;
            init->data.assign.is_cmd = false;
            init->data.assign.cmd_str = NULL;
            init->data.assign.lhs_index = NULL;

            if (!ts_match(TOK_COMMA))
                error(t.line, "Se esperaba ',' después de la inicialización");

            ASTNode *cond = parse_expression(0);

            if (!ts_match(TOK_COMMA))
                error(t.line, "Se esperaba ',' después de la condición");

            ASTNode *incr = NULL;
            Token next_tok = ts_peek();
            if (next_tok.type == TOK_IDENT) {
                int pos = ts.pos + 1;
                if (pos < ts.count) {
                    Token next_next = ts.tokens[pos];
                    if (next_next.type == TOK_PLUS_EQ || next_next.type == TOK_MINUS_EQ ||
                        next_next.type == TOK_STAR_EQ || next_next.type == TOK_SLASH_EQ ||
                        next_next.type == TOK_EQ) {
                        incr = parse_assignment_expr(t.line);
                        }
                }
            }
            if (!incr) incr = parse_expression(0);
            if (!incr) error(t.line, "No se pudo parsear el incremento del bucle for");

            if (incr->kind != NODE_POST_INC && incr->kind != NODE_POST_DEC &&
                incr->kind != NODE_ASSIGN) {
                ASTNode *incr_stmt = node_create(NODE_EXPR_STMT, incr->line);
            incr_stmt->data.expr_stmt.expr = incr;
            incr = incr_stmt;
                }

                if (!ts_match(TOK_THEN)) error_missing_then(t.line, "for");
                NodeList body = parse_block("fi");
            if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi'");

            stmt = node_create(NODE_FOR, t.line);
            stmt->data.for_stmt.var = varname;
            stmt->data.for_stmt.vtype = vtype;
            stmt->data.for_stmt.is_local = is_local;
            stmt->data.for_stmt.is_global = is_global;
            stmt->data.for_stmt.init = init;
            stmt->data.for_stmt.cond = cond;
            stmt->data.for_stmt.incr = incr;
            stmt->data.for_stmt.body = body;
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_FOR tradicional en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- FUNCTION --- */
        if (t.type == TOK_FUNCTION) {
            ts_advance();
            if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba nombre de función");
            char *fname = strdup(ts_advance().lexeme);

            if (func_lookup(fname) != NULL) {
                fprintf(stderr, "Error al declarar función, línea: %d: '%s' es una función interna y no puede ser redefinida.\n",
                        t.line, fname);
                exit(1);
            }

            if (!ts_match(TOK_LPAREN)) error(t.line, "Se esperaba '('");
            char **params = NULL; int *ptypes = NULL; int pcount = 0;
            if (!ts_match(TOK_RPAREN)) {
                do {
                    int ptype = 0;
                    TokenType tt = ts_peek().type;
                    if (tt == TOK_INT || tt == TOK_FLOAT || tt == TOK_BOOL || tt == TOK_STRING || tt == TOK_LIST)
                        ptype = ts_advance().type;
                    if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba nombre de parámetro");
                    char *pname = strdup(ts_advance().lexeme);
                    validate_var_name(pname, t.line);
                    params = realloc(params, (pcount+1)*sizeof(char*));
                    ptypes = realloc(ptypes, (pcount+1)*sizeof(int));
                    params[pcount] = pname;
                    ptypes[pcount] = ptype;
                    pcount++;
                } while (ts_match(TOK_COMMA));
                if (!ts_match(TOK_RPAREN)) error(t.line, "Se esperaba ')'");
            }
            NodeList body = parse_block("fi");
            if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi'");
            stmt = node_create(NODE_FUNC_DEF, t.line);
            stmt->data.func.name = fname;
            stmt->data.func.params = params;
            stmt->data.func.ptypes = ptypes;
            stmt->data.func.param_count = pcount;
            stmt->data.func.body = body;
            func_register(fname, stmt);
            if (current_import_prefix) {
                char prefixed[512];
                snprintf(prefixed, sizeof(prefixed), "%s.%s", current_import_prefix, fname);
                func_register(strdup(prefixed), stmt);
            }

            if (stmt) {
                Chunk *func_chunk = compile_function(stmt);
                if (func_chunk) {
                    FuncObject *fobj = func_lookup(stmt->data.func.name);
                    if (fobj && fobj->kind == FUNC_USER) {
                        fobj->code = func_chunk;
                    }
                }
            }
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_FUNC_DEF en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- RETURN --- */
        if (t.type == TOK_RETURN) {
            ts_advance();
            ASTNode *expr = NULL;
            if (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_FI && ts_peek().type != TOK_EOF)
                expr = parse_expression(0);
            stmt = node_create(NODE_RETURN, t.line);
            stmt->data.ret.expr = expr;
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_RETURN en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- BREAK --- */
        if (t.type == TOK_BREAK) {
            ts_advance();
            stmt = node_create(NODE_BREAK, t.line);
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_BREAK en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- CONTINUE --- */
        if (t.type == TOK_CONTINUE) {
            ts_advance();
            stmt = node_create(NODE_CONTINUE, t.line);
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_CONTINUE en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- REPEAT --- */
        if (t.type == TOK_REPEAT) {
            ts_advance();
            if (ts_peek().type == TOK_AT) {
                ts_advance();
                if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba nombre de portal después de '@'");
                char *portal_name = strdup(ts_advance().lexeme);
                stmt = node_create(NODE_REPEAT, t.line);
                stmt->data.repeat.portal_name = portal_name;
                stmt->data.repeat.line_expr = NULL;
            } else if (ts_match(TOK_LINE)) {
                ASTNode *line_expr = parse_expression(0);
                stmt = node_create(NODE_REPEAT, t.line);
                stmt->data.repeat.line_expr = line_expr;
                stmt->data.repeat.portal_name = NULL;
            } else {
                error(t.line, "Se esperaba 'line' o '@' después de 'repeat'");
            }
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_REPEAT en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- IMPORT --- */
        if (t.type == TOK_IMPORT) {
            ts_advance();
            Token nt = ts_peek();
            char *module_name = NULL;
            int use_embedded = 0;
            const unsigned char *emb_data = NULL;
            size_t emb_size = 0;

            if (nt.type == TOK_IDENT) {
                module_name = strdup(nt.lexeme);
                ts_advance();
                if (embedded_find(module_name, &emb_data, &emb_size, NULL)) {
                    use_embedded = 1;
                }
            } else if (nt.type == TOK_STRING_LITERAL) {
                ts_advance();
                module_name = strdup(nt.lexeme);
            } else {
                error(t.line, "Se esperaba nombre o ruta en import");
            }

            if (nt.type == TOK_IDENT && !valid_module_name(module_name))
                error(t.line, "Nombre de módulo inválido: %s", module_name);

            TokenStream old_ts = ts;
            ts_init();

            if (use_embedded) {
                tokenize_buffer((const char*)emb_data, emb_size);
            } else {
                char *path = NULL;
                if (nt.type == TOK_IDENT) {
                    int found = 0;
                    const char *home = getenv("HOME");
                    if (home) {
                        if (asprintf(&path, "%s/.infernal/fire/%s.fire", home, module_name) < 0)
                            error(t.line, "Memoria insuficiente al construir la ruta local");
                        FILE *fp = fopen(path, "r");
                        if (fp) {
                            tokenize_file(fp);
                            fclose(fp);
                            found = 1;
                        }
                        free(path);
                        path = NULL;
                    }
                    if (!found) {
                        if (asprintf(&path, "/usr/share/infernal/fire/%s.fire", module_name) < 0)
                            error(t.line, "Memoria insuficiente al construir la ruta global");
                        FILE *fp = fopen(path, "r");
                        if (fp) {
                            tokenize_file(fp);
                            fclose(fp);
                            found = 1;
                        } else {
                            error(t.line, "No se pudo abrir módulo '%s'", module_name);
                        }
                        free(path);
                    }
                } else {
                    if (!safe_module_path(nt.lexeme))
                        error(t.line, "Ruta de módulo inválida o insegura: %s", nt.lexeme);
                    path = strdup(nt.lexeme);
                    FILE *fp = fopen(path, "r");
                    if (!fp) error(t.line, "No se pudo abrir módulo: %s", path);
                    tokenize_file(fp);
                    fclose(fp);
                    free(path);
                }
            }

            char *prefix_base = module_name;
            char *slash = strrchr(module_name, '/');
            if (slash) prefix_base = slash + 1;
            char *dot = strrchr(prefix_base, '.');
            if (dot) *dot = '\0';

            char *old_prefix = current_import_prefix;
            current_import_prefix = prefix_base;
            NodeList module_block = parse_block(NULL);
            current_import_prefix = old_prefix;
            ts = old_ts;

            free(module_name);
            stmt = node_create(NODE_IMPORT, t.line);
            stmt->data.import.path = NULL;
            stmt->data.import.module_block = module_block;
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_IMPORT en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- TRY --- */
        if (t.type == TOK_TRY) {
            ts_advance();
            NodeList try_block = parse_block("catch");
            if (!ts_match(TOK_CATCH)) error(t.line, "Se esperaba 'catch'");
            NodeList catch_block = parse_block("fi");
            if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi'");
            stmt = node_create(NODE_TRY, t.line);
            stmt->data.try_stmt.try_block = try_block;
            stmt->data.try_stmt.catch_block = catch_block;
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_TRY en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- LOCAL / GLOBAL --- */
        if (t.type == TOK_LOCAL || t.type == TOK_GLOBAL) {
            bool is_local = ts_match(TOK_LOCAL);
            bool is_global = ts_match(TOK_GLOBAL);

            DEBUG_INFO("parse_block: declaración %s en línea %d", is_local ? "local" : "global", t.line);

            if (ts_peek().type == TOK_AT) {
                ts_advance();
                if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba nombre de portal después de '@'");
                char *portal_name = strdup(ts_advance().lexeme);
                stmt = node_create(NODE_PORTAL, t.line);
                stmt->data.portal.name = portal_name;
                stmt->data.portal.is_local = is_local;
                nodelist_add(&block, stmt);
                DEBUG_INFO("parse_block: añadido NODE_PORTAL en línea %d", stmt->line);
                ts_skip_newlines();
                continue;
            }

            int vtype = 0;
            if (!is_local && !is_global) {
                if (ts_peek().type == TOK_INT || ts_peek().type == TOK_FLOAT ||
                    ts_peek().type == TOK_BOOL || ts_peek().type == TOK_STRING || ts_peek().type == TOK_LIST) {
                    vtype = ts_advance().type;
                    } else {
                        error(t.line, "Se esperaba un tipo (int, float, bool, string, list)");
                    }
            } else {
                if (ts_peek().type == TOK_INT || ts_peek().type == TOK_FLOAT ||
                    ts_peek().type == TOK_BOOL || ts_peek().type == TOK_STRING || ts_peek().type == TOK_LIST) {
                    vtype = ts_advance().type;
                    }
            }

            if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba nombre de variable");
            char *vname = clean_var_name(ts_advance().lexeme);
            validate_var_name(vname, t.line);
            DEBUG_INFO("parse_block: variable '%s', vtype=%d", vname, vtype);

            ASTNode *lhs_index = NULL;
            if (ts_peek().type == TOK_LBRACKET) {
                Token lb = ts_advance();
                lhs_index = parse_index_or_slice(lb.line);
                if (lhs_index->kind == NODE_SLICE) error(lb.line, "No se puede usar slice en el lado izquierdo de una asignación");
                lhs_index->data.idx.list = node_create(NODE_VAR, t.line);
                lhs_index->data.idx.list->data.var.name = strdup(vname);
            }

            if (!ts_match(TOK_EQ)) error(t.line, "Se esperaba '='");

            ASTNode *value = NULL;
            bool is_cmd = false;
            char *cmd_str = NULL;

            Token next_token = ts_peek();
            if (next_token.type == TOK_IDENT && next_token.lexeme[0] != '$' && next_token.lexeme[0] != '?') {
                int next_pos = ts.pos + 1;
                if (next_pos < ts.count) {
                    Token next_next = ts.tokens[next_pos];
                    if (next_next.type == TOK_LPAREN || next_next.type == TOK_LBRACKET) {
                        value = parse_expression(0);
                    } else {
                        is_cmd = true;
                        cmd_str = extract_literal_command(t.line);
                        while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) ts_advance();
                        value = NULL;
                    }
                } else {
                    is_cmd = true;
                    cmd_str = extract_literal_command(t.line);
                    while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) ts_advance();
                    value = NULL;
                }
            } else {
                value = parse_expression(0);
            }

            stmt = node_create(NODE_ASSIGN, t.line);
            stmt->data.assign.name = vname;
            stmt->data.assign.is_cmd = is_cmd;
            stmt->data.assign.cmd_str = cmd_str;
            stmt->data.assign.value = value;
            stmt->data.assign.vtype = vtype;
            stmt->data.assign.is_local = is_local;
            stmt->data.assign.is_global = is_global;
            stmt->data.assign.lhs_index = lhs_index;
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadida asignación de '%s' en línea %d", vname, stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- TIPO + IDENT (int x = 5) --- */
        if (t.type == TOK_INT || t.type == TOK_FLOAT || t.type == TOK_BOOL ||
            t.type == TOK_STRING || t.type == TOK_LIST) {
            int vtype = ts_advance().type;
        if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba nombre de variable");
        char *vname = clean_var_name(ts_advance().lexeme);
            validate_var_name(vname, t.line);

            ASTNode *lhs_index = NULL;
            if (ts_peek().type == TOK_LBRACKET) {
                Token lb = ts_advance();
                lhs_index = parse_index_or_slice(lb.line);
                if (lhs_index->kind == NODE_SLICE) error(lb.line, "No se puede usar slice en el lado izquierdo de una asignación");
                lhs_index->data.idx.list = node_create(NODE_VAR, t.line);
                lhs_index->data.idx.list->data.var.name = strdup(vname);
            }

            if (!ts_match(TOK_EQ)) error(t.line, "Se esperaba '='");

            ASTNode *value = NULL;
            bool is_cmd = false;
            char *cmd_str = NULL;

            Token next_token = ts_peek();
            if (next_token.type == TOK_IDENT && next_token.lexeme[0] != '$' && next_token.lexeme[0] != '?') {
                int next_pos = ts.pos + 1;
                if (next_pos < ts.count) {
                    Token next_next = ts.tokens[next_pos];
                    if (next_next.type == TOK_LPAREN || next_next.type == TOK_LBRACKET) {
                        value = parse_expression(0);
                    } else {
                        is_cmd = true;
                        cmd_str = extract_literal_command(t.line);
                        while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) ts_advance();
                        value = NULL;
                    }
                } else {
                    is_cmd = true;
                    cmd_str = extract_literal_command(t.line);
                    while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) ts_advance();
                    value = NULL;
                }
            } else {
                value = parse_expression(0);
            }

            stmt = node_create(NODE_ASSIGN, t.line);
            stmt->data.assign.name = vname;
            stmt->data.assign.is_cmd = is_cmd;
            stmt->data.assign.cmd_str = cmd_str;
            stmt->data.assign.value = value;
            stmt->data.assign.vtype = vtype;
            stmt->data.assign.is_local = false;
            stmt->data.assign.is_global = false;
            stmt->data.assign.lhs_index = lhs_index;
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadida asignación sin local/global de '%s' en línea %d", vname, stmt->line);
            ts_skip_newlines();
            continue;
            }

            /* --- IDENT (x = 0, print(...), etc.) --- */
            if (t.type == TOK_IDENT) {
                Token saved_t = t;
                ts_advance();
                Token next_tok = ts_peek();
                DEBUG_INFO("parse_block: VI UN IDENT: '%s' (línea %d) siguiente token '%s' (tipo %d)",
                           saved_t.lexeme, saved_t.line, next_tok.lexeme, next_tok.type);

                /* ASIGNACIÓN (x = 0, x += 2, etc.) */
                if (next_tok.type == TOK_EQ || next_tok.type == TOK_PLUS_EQ ||
                    next_tok.type == TOK_MINUS_EQ || next_tok.type == TOK_STAR_EQ ||
                    next_tok.type == TOK_SLASH_EQ) {
                    DEBUG_INFO("parse_block: DETECTADA ASIGNACIÓN para '%s'", saved_t.lexeme);
                ts.pos--;
                stmt = parse_assignment_expr(saved_t.line);
                nodelist_add(&block, stmt);
                DEBUG_INFO("parse_block: asignación parseada para '%s' en línea %d", saved_t.lexeme, stmt->line);
                ts_skip_newlines();
                continue;
                    }

                    /* LLAMADA A FUNCIÓN (print(...)) */
                    if (next_tok.type == TOK_LPAREN) {
                        ts.pos--;
                        ASTNode *expr = parse_expression(0);
                        stmt = node_create(NODE_EXPR_STMT, saved_t.line);
                        stmt->data.expr_stmt.expr = expr;
                        nodelist_add(&block, stmt);
                        DEBUG_INFO("parse_block: llamada a función '%s' en línea %d", saved_t.lexeme, stmt->line);
                        ts_skip_newlines();
                        continue;
                    }

                    /* INDEXACIÓN (lista[0] o asignación con índice) */
                    if (next_tok.type == TOK_LBRACKET) {
                        ts.pos--;
                        ASTNode *idx_expr = parse_expression(0);
                        if (ts_peek().type == TOK_EQ) {
                            // stats["vida"] = 1000
                            ts_advance(); // consumir '='
                            char *vname = NULL;
                            if (idx_expr->kind == NODE_INDEX && idx_expr->data.idx.list->kind == NODE_VAR) {
                                vname = strdup(idx_expr->data.idx.list->data.var.name);
                            }
                            if (!vname) error(saved_t.line, "Lado izquierdo inválido para asignación con índice");
                            ASTNode *value = parse_expression(0);
                            stmt = node_create(NODE_ASSIGN, saved_t.line);
                            stmt->data.assign.name = vname;
                            stmt->data.assign.value = value;
                            stmt->data.assign.vtype = 0;
                            stmt->data.assign.is_local = false;
                            stmt->data.assign.is_global = false;
                            stmt->data.assign.is_cmd = false;
                            stmt->data.assign.cmd_str = NULL;
                            stmt->data.assign.lhs_index = idx_expr;
                            nodelist_add(&block, stmt);
                            DEBUG_INFO("parse_block: asignación con índice de '%s' en línea %d", vname, stmt->line);
                            ts_skip_newlines();
                            continue;
                        } else {
                            stmt = node_create(NODE_EXPR_STMT, saved_t.line);
                            stmt->data.expr_stmt.expr = idx_expr;
                            nodelist_add(&block, stmt);
                            DEBUG_INFO("parse_block: indexación en línea %d", stmt->line);
                            ts_skip_newlines();
                            continue;
                        }
                    }

                    /* POST-INCREMENTO/DECREMENTO (i++ o i--) */
                    if (next_tok.type == TOK_INC || next_tok.type == TOK_DEC) {
                        ts.pos--;
                        ASTNode *expr = parse_expression(0);
                        stmt = node_create(NODE_EXPR_STMT, saved_t.line);
                        stmt->data.expr_stmt.expr = expr;
                        nodelist_add(&block, stmt);
                        DEBUG_INFO("parse_block: post-inc/dec para '%s' en línea %d", saved_t.lexeme, stmt->line);
                        ts_skip_newlines();
                        continue;
                    }

                    /* COMANDO SHELL (cualquier otra cosa) */
                    {
                        ts.pos--;
                        int start_pos = ts.pos;
                        while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                            ts_advance();
                        }
                        char *cmd_str = build_command_from_tokens(start_pos, ts.pos);
                        stmt = node_create(NODE_SHELL_CMD, saved_t.line);
                        stmt->data.shell_cmd.cmd = cmd_str;
                        nodelist_add(&block, stmt);
                        DEBUG_INFO("parse_block: comando shell '%s' en línea %d", cmd_str, stmt->line);
                        ts_skip_newlines();
                        continue;
                    }
            }

            /* --- Token no reconocido --- */
            error(t.line, "Sentencia no reconocida '%s'", t.lexeme);

            if (stmt) {
                nodelist_add(&block, stmt);
                DEBUG_INFO("parse_block: añadida sentencia tipo %d en línea %d", stmt->kind, stmt->line);
            }
            ts_skip_newlines();
            if (terminator && ts_peek().type == lookup_keyword(terminator)) break;
            if (terminator && strcmp(terminator, "}") == 0 && ts_peek().type == TOK_RBRACE) break;
    }

    DEBUG_INFO("=== parse_block: bloque finalizado, %d sentencias ===", block.count);
    return block;
}
