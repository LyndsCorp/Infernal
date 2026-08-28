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
#include <stdint.h>
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
#include "core/memory.h"
#include "runtime/evaluator/helpers.h"
#include "developer/debug.h"

/* --- Estado de limpieza de parser ante longjmp --- */
#define MAX_ACTIVE_PARSE_BLOCKS 256
static NodeList *active_parse_blocks[MAX_ACTIVE_PARSE_BLOCKS];
static size_t active_parse_block_count = 0;

void parser_cleanup_on_error(void) {
    while (active_parse_block_count > 0) {
        NodeList *block = active_parse_blocks[--active_parse_block_count];
        nodelist_free(block);
    }
    ast_free_all();
}

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
    if (!path || !*path || strchr(path, '\n') || strchr(path, '\r'))
        return false;

    const char *p = path;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        const char *start = p;
        while (*p && *p != '/' && *p != '\\') p++;
        size_t len = (size_t)(p - start);
        if (len == 2 && start[0] == '.' && start[1] == '.')
            return false;
    }
    return true;
}

static void command_append(char **cmd, size_t *len, size_t *cap, const char *text) {
    if (!text) return;
    size_t add = strlen(text);
    if (add > SIZE_MAX - *len - 1)
        error(0, "Comando demasiado largo");
    size_t needed = *len + add + 1;
    if (needed > *cap) {
        size_t new_cap = *cap ? *cap : 64;
        while (new_cap < needed) {
            if (new_cap > SIZE_MAX / 2) {
                new_cap = needed;
                break;
            }
            new_cap *= 2;
        }
        *cmd = infernal_realloc(*cmd, new_cap);
        *cap = new_cap;
    }
    memcpy(*cmd + *len, text, add);
    *len += add;
    (*cmd)[*len] = '\0';
}

static void command_append_token(char **cmd, size_t *len, size_t *cap, Token tok, bool add_space) {
    if (add_space && *len > 0) command_append(cmd, len, cap, " ");
    if (tok.type == TOK_STRING_LITERAL) command_append(cmd, len, cap, "\"");
    command_append(cmd, len, cap, tok.lexeme);
    if (tok.type == TOK_STRING_LITERAL) command_append(cmd, len, cap, "\"");
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


static int find_switch_boundary(int start_pos) {
    int depth = 0;
    for (int i = start_pos; i < ts.count; i++) {
        TokenType type = ts.tokens[i].type;
        if (type == TOK_IF || type == TOK_WHILE || type == TOK_FOR ||
            type == TOK_FUNCTION || type == TOK_TRY || type == TOK_SWITCH) {
            depth++;
            continue;
        }
        if (type == TOK_FI) {
            if (depth == 0) return i;
            depth--;
            continue;
        }
        if (depth == 0 && (type == TOK_CASE || type == TOK_DEFAULT)) return i;
    }
    return -1;
}

static bool validate_switch_body(const NodeList *body) {
    if (!body || body->count == 0) return true;
    return body->stmts[body->count - 1]->kind == NODE_BREAK;
}

static ASTNode *parse_switch_statement(void) {
    Token t = ts_peek();
    int line = t.line;
    ts_advance();

    ASTNode *stmt = node_create(NODE_SWITCH, line);
    stmt->data.switch_stmt.expr = parse_expression(0);
    stmt->data.switch_stmt.cases = NULL;
    stmt->data.switch_stmt.case_count = 0;
    stmt->data.switch_stmt.default_block = (NodeList){NULL, 0, 0};
    stmt->data.switch_stmt.has_default = false;

    ts_skip_newlines();

    while (1) {
        Token head = ts_peek();
        if (head.type == TOK_FI) {
            ts_advance();
            break;
        }
        if (head.type == TOK_EOF) {
            error(line, "Se esperaba 'fi' para cerrar el switch");
        }
        if (head.type == TOK_CASE) {
            ts_advance();
            ASTNode *value = parse_expression(0);
            if (!value) error(head.line, "Se esperaba un valor después de 'case'");
            ts_skip_newlines();

            int boundary = find_switch_boundary(ts.pos);
            if (boundary < 0) error(head.line, "Se esperaba otro 'case', 'default' o 'fi' en el switch");

            TokenType saved_type = ts.tokens[boundary].type;
            ts.tokens[boundary].type = TOK_EOF;
            NodeList body = parse_block(NULL);
            ts.tokens[boundary].type = saved_type;
            ts.pos = boundary;

            /* Validate before attaching the partially parsed case to the
             * switch AST.  On parser errors error() longjmps to the outer
             * cleanup path, so keeping ownership unambiguous here avoids
             * double-freeing partially constructed switch nodes. */
            if (!validate_switch_body(&body)) {
                /* `body` is not owned by the switch yet.  Free its NodeList
                 * storage before jumping out through the parser error path. */
                nodelist_free(&body);
                error(head.line, "El case debe terminar con 'break'");
            }

            int n = stmt->data.switch_stmt.case_count;
            SwitchCase *tmp = infernal_realloc(stmt->data.switch_stmt.cases, (size_t)(n + 1) * sizeof(*tmp));
            stmt->data.switch_stmt.cases = tmp;
            stmt->data.switch_stmt.cases[n].value = value;
            stmt->data.switch_stmt.cases[n].body = body;
            stmt->data.switch_stmt.case_count = n + 1;
            ts_skip_newlines();
            continue;
        }
        if (head.type == TOK_DEFAULT) {
            if (stmt->data.switch_stmt.has_default)
                error(head.line, "No puede haber más de un 'default' en un switch");
            ts_advance();
            ts_skip_newlines();

            int boundary = find_switch_boundary(ts.pos);
            if (boundary < 0) error(head.line, "Se esperaba 'fi' al cerrar el switch");
            if (boundary != ts.pos && ts.tokens[boundary].type == TOK_CASE)
                error(head.line, "'default' debe ser el último bloque del switch");

            TokenType saved_type = ts.tokens[boundary].type;
            ts.tokens[boundary].type = TOK_EOF;
            NodeList body = parse_block(NULL);
            ts.tokens[boundary].type = saved_type;
            ts.pos = boundary;

            /* Same ownership rule as cases: validate before attaching the
             * block so a syntax error cannot leave duplicate ownership. */
            if (!validate_switch_body(&body)) {
                nodelist_free(&body);
                error(head.line, "El default debe terminar con 'break'");
            }
            stmt->data.switch_stmt.default_block = body;
            stmt->data.switch_stmt.has_default = true;
            ts_skip_newlines();
            continue;
        }
        error(head.line, "Se esperaba 'case', 'default' o 'fi' dentro del switch");
    }

    return stmt;
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
        var_node->data.var.clone = false;
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
    if (active_parse_block_count < MAX_ACTIVE_PARSE_BLOCKS)
        active_parse_blocks[active_parse_block_count++] = &block;
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
            char *cmd = infernal_strdup("");
            size_t cmd_len = 0, cmd_cap = 1;
            Token prev_token = {TOK_EOF, "", 0, 0, 0};
            while (ts_peek().type != TOK_BANG && ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                Token ct = ts_advance();
                bool hay_espacio = prev_token.type != TOK_EOF && ct.start_col > prev_token.end_col;
                command_append_token(&cmd, &cmd_len, &cmd_cap, ct, hay_espacio);
                prev_token = ct;
            }
            if (ts_peek().type == TOK_BANG) ts_advance();
            stmt = node_create(NODE_CMD_STMT, t.line);
            stmt->data.cmd_stmt.cmd = cmd;

            while (ts_match(TOK_OR)) {
                ASTNode *next_stmt = NULL;
                Token next = ts_peek();
                if (next.type == TOK_BANG) {
                    ts_advance();
                    cmd = infernal_strdup("");
                    cmd_len = 0; cmd_cap = 1;
                    prev_token = (Token){TOK_EOF, "", 0, 0, 0};
                    while (ts_peek().type != TOK_BANG && ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                        Token ct2 = ts_advance();
                        bool hay_espacio2 = prev_token.type != TOK_EOF && ct2.start_col > prev_token.end_col;
                        command_append_token(&cmd, &cmd_len, &cmd_cap, ct2, hay_espacio2);
                        prev_token = ct2;
                    }
                    if (ts_peek().type == TOK_BANG) ts_advance();
                    next_stmt = node_create(NODE_CMD_STMT, next.line);
                    next_stmt->data.cmd_stmt.cmd = cmd;
                } else {
                    cmd = infernal_strdup("");
                    cmd_len = 0; cmd_cap = 1;
                    Token prev2 = (Token){TOK_EOF, "", 0, 0, 0};
                    while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF && ts_peek().type != TOK_OR) {
                        Token ct2 = ts_advance();
                        bool hay_espacio2 = prev2.type != TOK_EOF && ct2.start_col > prev2.end_col;
                        command_append_token(&cmd, &cmd_len, &cmd_cap, ct2, hay_espacio2);
                        prev2 = ct2;
                    }
                    next_stmt = node_create(NODE_SHELL_CMD, next.line);
                    next_stmt->data.shell_cmd.cmd = cmd;
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
            char *cmd = infernal_strdup("");
            size_t cmd_len = 0, cmd_cap = 1;
            Token first = ts_advance();
            command_append_token(&cmd, &cmd_len, &cmd_cap, first, false);
            Token prev_token = first;
            while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                Token ct = ts_advance();
                bool hay_espacio = prev_token.type != TOK_EOF && ct.start_col > prev_token.end_col;
                command_append_token(&cmd, &cmd_len, &cmd_cap, ct, hay_espacio);
                prev_token = ct;
            }
            stmt = node_create(NODE_SHELL_CMD, t.line);
            stmt->data.shell_cmd.cmd = cmd;
            while (ts_match(TOK_OR)) {
                ASTNode *next_stmt = NULL;
                Token next = ts_peek();
                cmd = infernal_strdup("");
                cmd_len = 0; cmd_cap = 1;
                Token prev2 = (Token){TOK_EOF, "", 0, 0, 0};
                if (next.type == TOK_BANG) {
                    ts_advance();
                    while (ts_peek().type != TOK_BANG && ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                        Token ct2 = ts_advance();
                        bool hay_espacio2 = prev2.type != TOK_EOF && ct2.start_col > prev2.end_col;
                        command_append_token(&cmd, &cmd_len, &cmd_cap, ct2, hay_espacio2);
                        prev2 = ct2;
                    }
                    if (ts_peek().type == TOK_BANG) ts_advance();
                    next_stmt = node_create(NODE_CMD_STMT, next.line);
                    next_stmt->data.cmd_stmt.cmd = cmd;
                } else {
                    while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF && ts_peek().type != TOK_OR) {
                        Token ct2 = ts_advance();
                        bool hay_espacio2 = prev2.type != TOK_EOF && ct2.start_col > prev2.end_col;
                        command_append_token(&cmd, &cmd_len, &cmd_cap, ct2, hay_espacio2);
                        prev2 = ct2;
                    }
                    next_stmt = node_create(NODE_SHELL_CMD, next.line);
                    next_stmt->data.shell_cmd.cmd = cmd;
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

        /* --- SWITCH --- */
        if (t.type == TOK_SWITCH) {
            stmt = parse_switch_statement();
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_SWITCH en línea %d", stmt->line);
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
            if (next_tok.type == TOK_IDENT && next_tok.lexeme[0] == '$')
                error(t.line, "En el incremento de un for debes usar 'i++' o 'i--', sin '$'");
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
            stmt->data.for_stmt.var = strdup(varname);
            if (!stmt->data.for_stmt.var) {
                ast_free(stmt);
                error(t.line, "No hay memoria para el nombre de variable del for");
            }
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
            stmt = node_create(NODE_FUNC_DEF, t.line);
            if (ts_peek().type != TOK_IDENT) {
                ast_free(stmt);
                stmt = NULL;
                error(t.line, "Se esperaba nombre de función");
            }
            stmt->data.func.name = strdup(ts_advance().lexeme);
            stmt->data.func.params = NULL;
            stmt->data.func.ptypes = NULL;
            stmt->data.func.param_count = 0;
            stmt->data.func.body = (NodeList){NULL, 0, 0};
            nodelist_add(&block, stmt);

            if (func_lookup(stmt->data.func.name) != NULL)
                error(t.line, "'%s' es una función interna y no puede ser redefinida.", stmt->data.func.name);

            if (!ts_match(TOK_LPAREN)) error(t.line, "Se esperaba '('");
            if (!ts_match(TOK_RPAREN)) {
                do {
                    int ptype = 0;
                    TokenType tt = ts_peek().type;
                    if (tt == TOK_INT || tt == TOK_FLOAT || tt == TOK_BOOL || tt == TOK_STRING || tt == TOK_LIST || tt == TOK_MAP)
                        ptype = ts_advance().type;
                    if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba nombre de parámetro");
                    char *pname = clean_var_name(ts_advance().lexeme);
                    validate_var_name(pname, t.line);
                    int pcount = stmt->data.func.param_count;
                    stmt->data.func.params = realloc(stmt->data.func.params, (pcount + 1) * sizeof(char*));
                    stmt->data.func.ptypes = realloc(stmt->data.func.ptypes, (pcount + 1) * sizeof(int));
                    stmt->data.func.params[pcount] = pname;
                    stmt->data.func.ptypes[pcount] = ptype;
                    stmt->data.func.param_count = pcount + 1;
                } while (ts_match(TOK_COMMA));
                if (!ts_match(TOK_RPAREN)) error(t.line, "Se esperaba ')'");
            }

            stmt->data.func.body = parse_block("fi");
            if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi'");

            func_register(stmt->data.func.name, stmt);
            if (current_import_prefix) {
                char prefixed[512];
                snprintf(prefixed, sizeof(prefixed), "%s.%s", current_import_prefix, stmt->data.func.name);
                func_register(prefixed, stmt);
            }

            DEBUG_INFO("parse_block: añadido NODE_FUNC_DEF en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- RETURN --- */
        if (t.type == TOK_RETURN) {
            ts_advance();
            ASTNode *expr = NULL;
            int rtype = 0;

            TokenType tt = ts_peek().type;
            if (tt == TOK_INT || tt == TOK_FLOAT || tt == TOK_BOOL ||
                tt == TOK_STRING || tt == TOK_LIST || tt == TOK_MAP) {
                rtype = ts_advance().type;
                if (ts_peek().type == TOK_NEWLINE || ts_peek().type == TOK_FI || ts_peek().type == TOK_EOF)
                    error(t.line, "Se esperaba un valor después del tipo de retorno '%s'", type_name(rtype));
            }

            if (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_FI && ts_peek().type != TOK_EOF)
                expr = parse_expression(0);

            if (rtype != 0 && !expr)
                error(t.line, "Se esperaba un valor después de 'return %s'", type_name(rtype));

            stmt = node_create(NODE_RETURN, t.line);
            stmt->data.ret.expr = expr;
            stmt->data.ret.rtype = rtype;
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_RETURN en línea %d (rtype=%d)", stmt->line, rtype);
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
            char *module_alias = NULL;
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

            /* import <módulo/ruta> as <alias> */
            if (ts_peek().type == TOK_IDENT && strcmp(ts_peek().lexeme, "as") == 0) {
                ts_advance();
                if (ts_peek().type != TOK_IDENT)
                    error(t.line, "Se esperaba un alias después de 'as'");
                module_alias = strdup(ts_advance().lexeme);
                if (!valid_module_name(module_alias))
                    error(t.line, "Alias de módulo inválido: %s", module_alias);
            }

            TokenStream old_ts = ts;
            char **old_source_lines = source_lines;
            int old_source_line_count = source_line_count;
            ts_init();
            source_lines = NULL;
            source_line_count = 0;

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
            current_import_prefix = module_alias ? module_alias : prefix_base;
            NodeList module_block = parse_block(NULL);
            current_import_prefix = old_prefix;

            /* El stream del módulo ya no se necesita después del parseo.
             * Liberarlo evita que cada import embebido pierda sus tokens. */
            for (int i = 0; i < ts.count; i++) free(ts.tokens[i].lexeme);
            free(ts.tokens);
            ts = old_ts;

            /* tokenize_file() mantiene sus propias source_lines. Restauramos
             * las del script principal para que los comandos posteriores y
             * los mensajes de error sigan apuntando al archivo correcto. */
            for (int i = 0; i < source_line_count; i++) free(source_lines[i]);
            free(source_lines);
            source_lines = old_source_lines;
            source_line_count = old_source_line_count;

            free(module_name);
            stmt = node_create(NODE_IMPORT, t.line);
            stmt->data.import.path = NULL;
            stmt->data.import.alias = module_alias;
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

        /* --- LOCAL / GLOBAL / TIPO: declaraciones de variables --- */
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
            if (ts_peek().type == TOK_INT || ts_peek().type == TOK_FLOAT ||
                ts_peek().type == TOK_BOOL || ts_peek().type == TOK_STRING ||
                ts_peek().type == TOK_LIST || ts_peek().type == TOK_MAP) {
                vtype = ts_advance().type;
            } else if (ts_peek().type != TOK_IDENT) {
                error(t.line, "Se esperaba un tipo (int, float, bool, string, list, map) o un nombre de variable");
            }

            /* Una sola línea puede declarar varias variables, pero todas
             * comparten exactamente el mismo scope y tipo definidos arriba. */
            while (1) {
                if (ts_peek().type != TOK_IDENT)
                    error(t.line, "Se esperaba nombre de variable después del tipo o de ','");

                char *vname = clean_var_name(ts_advance().lexeme);
                validate_var_name(vname, t.line);

                if (ts_peek().type == TOK_LOCAL || ts_peek().type == TOK_GLOBAL)
                    error(t.line, "No se puede cambiar el scope dentro de una declaración múltiple; todas las variables deben compartir scope y tipo");

                if (ts_peek().type == TOK_LBRACKET)
                    error(t.line, "No se puede usar indexación al declarar una variable");

                ASTNode *value = NULL;
                bool is_cmd = false;
                char *cmd_str = NULL;

                if (ts_match(TOK_EQ)) {
                    Token next_token = ts_peek();
                    if (next_token.type == TOK_IDENT && next_token.lexeme[0] != '$' && next_token.lexeme[0] != '?') {
                        int next_pos = ts.pos + 1;
                        TokenType rhs_follow = (next_pos < ts.count) ? ts.tokens[next_pos].type : TOK_EOF;

                        /*
                         * Un comando simple sigue siendo válido (p. ej. `host = hostname`).
                         * En cambio, si después del identificador viene un operador, debemos
                         * tratarlo como expresión para que referencias de variables como
                         * `contador_test = contador_test + 1` no terminen ejecutándose como
                         * comandos del shell.
                         */
                        bool simple_command =
                            rhs_follow == TOK_NEWLINE || rhs_follow == TOK_EOF ||
                            rhs_follow == TOK_COMMA;

                        if (rhs_follow == TOK_LPAREN || rhs_follow == TOK_LBRACKET || !simple_command) {
                            value = parse_expression(0);
                        } else {
                            is_cmd = true;
                            cmd_str = extract_literal_command(t.line);
                            while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) ts_advance();
                        }
                    } else {
                        value = parse_expression(0);
                    }
                }

                stmt = node_create(NODE_ASSIGN, t.line);
                stmt->data.assign.name = vname;
                stmt->data.assign.is_cmd = is_cmd;
                stmt->data.assign.cmd_str = cmd_str;
                stmt->data.assign.value = value; /* NULL = valor por defecto del tipo */
                stmt->data.assign.vtype = vtype;
                stmt->data.assign.is_local = is_local;
                stmt->data.assign.is_global = is_global;
                stmt->data.assign.lhs_index = NULL;
                nodelist_add(&block, stmt);

                if (is_cmd) {
                    /* extract_literal_command consume el resto de la línea. */
                    break;
                }

                if (!ts_match(TOK_COMMA))
                    break;

                /* El tipado automático (vtype == 0) solo puede declarar una
                 * variable por línea. Las declaraciones múltiples requieren
                 * un tipo explícito común para todas. */
                if (vtype == 0)
                    error(t.line, "Las declaraciones múltiples requieren un tipo explícito");

                /* Después de ',' debe venir solamente otro identificador.
                 * Por tanto 'global a, local b' queda rechazado. */
                if (ts_peek().type != TOK_IDENT)
                    error(t.line, "Después de ',' se esperaba otra variable; no se puede cambiar el scope o tipo dentro de la misma línea");
            }

            ts_skip_newlines();
            continue;
        }

        /* --- TIPO + IDENT (int x = 5, int a, b, c) --- */
        if (t.type == TOK_INT || t.type == TOK_FLOAT || t.type == TOK_BOOL ||
            t.type == TOK_STRING || t.type == TOK_LIST || t.type == TOK_MAP) {
            int vtype = ts_advance().type;

            while (1) {
                if (ts_peek().type != TOK_IDENT)
                    error(t.line, "Se esperaba nombre de variable después del tipo o de ','");

                char *vname = clean_var_name(ts_advance().lexeme);
                validate_var_name(vname, t.line);

                if (ts_peek().type == TOK_LOCAL || ts_peek().type == TOK_GLOBAL)
                    error(t.line, "No se puede cambiar el scope dentro de una declaración múltiple; todas las variables deben compartir scope y tipo");

                if (ts_peek().type == TOK_LBRACKET)
                    error(t.line, "No se puede usar indexación al declarar una variable");

                ASTNode *value = NULL;
                bool is_cmd = false;
                char *cmd_str = NULL;

                if (ts_match(TOK_EQ)) {
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
                            }
                        } else {
                            is_cmd = true;
                            cmd_str = extract_literal_command(t.line);
                            while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) ts_advance();
                        }
                    } else {
                        value = parse_expression(0);
                    }
                }

                stmt = node_create(NODE_ASSIGN, t.line);
                stmt->data.assign.name = vname;
                stmt->data.assign.is_cmd = is_cmd;
                stmt->data.assign.cmd_str = cmd_str;
                stmt->data.assign.value = value; /* NULL = valor por defecto del tipo */
                stmt->data.assign.vtype = vtype;
                stmt->data.assign.is_local = false;
                stmt->data.assign.is_global = false;
                stmt->data.assign.lhs_index = NULL;
                nodelist_add(&block, stmt);
                DEBUG_INFO("parse_block: añadida declaración tipada de '%s' en línea %d", vname, stmt->line);

                if (is_cmd)
                    break;
                if (!ts_match(TOK_COMMA))
                    break;
                if (ts_peek().type != TOK_IDENT)
                    error(t.line, "Después de ',' se esperaba otra variable; no se puede cambiar el scope o tipo dentro de la misma línea");
            }

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
                        if ((expr->kind == NODE_POST_INC || expr->kind == NODE_POST_DEC) &&
                            expr->data.post_op.var && expr->data.post_op.var->kind == NODE_VAR &&
                            expr->data.post_op.var->data.var.clone) {
                            expr->data.post_op.statement_context = true;
                            }
                            stmt = node_create(NODE_EXPR_STMT, saved_t.line);
                        stmt->data.expr_stmt.expr = expr;
                        nodelist_add(&block, stmt);
                        DEBUG_INFO("parse_block: post-inc/dec para '%s' en línea %d", saved_t.lexeme, stmt->line);
                        ts_skip_newlines();
                        continue;
                    }

                    /* COMANDO SHELL (cualquier otra cosa) — NUEVO: preserva comillas */
                    {
                        ts.pos--;
                        char *cmd = infernal_strdup("");
                        size_t cmd_len = 0, cmd_cap = 1;
                        Token prev_token = {TOK_EOF, "", 0, 0, 0};

                        while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                            Token ct = ts_advance();
                            bool hay_espacio = prev_token.type != TOK_EOF && ct.start_col > prev_token.end_col;
                            command_append_token(&cmd, &cmd_len, &cmd_cap, ct, hay_espacio);
                            prev_token = ct;
                        }

                        stmt = node_create(NODE_SHELL_CMD, saved_t.line);
                        stmt->data.shell_cmd.cmd = cmd;
                        nodelist_add(&block, stmt);
                        DEBUG_INFO("parse_block: comando shell '%s' en línea %d", cmd, stmt->line);
                        ts_skip_newlines();
                        continue;
                    }
            }

    DEBUG_INFO("=== parse_block: bloque finalizado, %d sentencias ===", block.count);
    if (active_parse_block_count > 0 && active_parse_blocks[active_parse_block_count - 1] == &block)
        active_parse_block_count--;
    return block;
}
}
