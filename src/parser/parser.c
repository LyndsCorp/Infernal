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
#include <unistd.h>
#include <limits.h>
#include "parser.h"
#include "core/ast.h"
#include "lexer/lexer.h"
#include "lexer/keywords.h"
#include "expression.h"
#include "flags.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "runtime/lava.h"
#include "embedded/embedded.h"
#include "vm/compiler.h"
#include "developer/debug.h"
#include "core/memory.h"
#include "runtime/evaluator/helpers.h"

/* --- Estado de limpieza de parser ante longjmp --- */
void parser_cleanup_on_error(void) {
    ast_free_all();
}

/* --- Contador de bloques abiertos que esperan un 'fi' ---
 *
 * Se incrementa al entrar en un bloque que se cierra con 'fi'
 * (if / while / for / for-in / function / try / switch) y se
 * decrementa al consumir su 'fi'.
 *
 * Sirve para que, cuando un bloque intenta consumir un 'fi', podamos
 * comprobar si ese 'fi' realmente le pertenece: si en el resto del
 * archivo quedan menos 'fi' que bloques abiertos, es que este 'fi'
 * pertenece a un bloque externo y al nuestro le falta el suyo. */
static int fi_block_nesting = 0;

static int count_remaining_fi(void) {
    int count = 0;
    for (int i = ts.pos; i < ts.count; i++) {
        if (ts.tokens[i].type == TOK_FI) count++;
    }
    return count;
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

static bool is_block_closer(TokenType type) {
    return type == TOK_FI || type == TOK_ELSE || type == TOK_ELSEIF ||
    type == TOK_CASE || type == TOK_DEFAULT || type == TOK_CATCH ||
    type == TOK_EOF;
}

static bool is_expression_start(TokenType type) {
    return type == TOK_NUMBER || type == TOK_STRING_LITERAL || type == TOK_TRUE ||
    type == TOK_FALSE || type == TOK_IDENT || type == TOK_LBRACKET ||
    type == TOK_LBRACE || type == TOK_LPAREN || type == TOK_MINUS ||
    type == TOK_PLUS ||
    type == TOK_NOT;
}

static void require_statement_end(const char *what) {
    Token next = ts_peek();
    if (next.type == TOK_NEWLINE || next.type == TOK_EOF || next.type == TOK_THEN || is_block_closer(next.type))
        return;
    error_at(next.line, next.start_col > 0 ? next.start_col : 1,
             "Sobra '%s' después de %s; quizá falta un operador o un salto de línea",
             next.lexeme, what);
}

static ASTNode *parse_typed_empty_collection(int vtype, int line) {
    if (ts_peek().type != TOK_LBRACKET ||
        ts.pos + 1 >= ts.count || ts.tokens[ts.pos + 1].type != TOK_RBRACKET) {
        return NULL;
        }

        if (vtype != TOK_LIST && vtype != TOK_MAP) return NULL;

        ts_advance();
    ts_advance();

    ASTNode *node = node_create(vtype == TOK_LIST ? NODE_LIST : NODE_MAP, line);
    if (node->kind == NODE_LIST) {
        node->data.list_lit.items = NULL;
        node->data.list_lit.count = 0;
    } else {
        node->data.map.pairs = NULL;
        node->data.map.pair_count = 0;
    }
    return node;
}

/*
 * Un nombre de módulo puede contener subdirectorios separados por '/'
 * (p. ej. "build/lava/ejemplo.lava"). Se prohíben rutas absolutas y
 * segmentos ".." para evitar escapes del directorio de módulos.
 */
static bool valid_module_name(const char *name) {
    if (!name || !*name) return false;
    if (name[0] == '/') return false;

    const char *p = name;
    while (*p) {
        const char *seg_start = p;
        while (*p && *p != '/') p++;
        size_t seg_len = (size_t)(p - seg_start);

        /* Segmentos vacíos: "//" o "/" al final. */
        if (seg_len == 0) return false;
        /* "." y ".." prohibidos. */
        if (seg_len == 1 && seg_start[0] == '.') return false;
        if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.') return false;

        for (size_t i = 0; i < seg_len; i++) {
            unsigned char c = (unsigned char)seg_start[i];
            if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
        }
        if (*p == '/') p++;
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
    fi_block_nesting++;

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
            int remaining = count_remaining_fi();
            if (remaining < fi_block_nesting) {
                error(line,
                      "Falta el 'fi' que cierra este 'switch' (abierto en la línea %d).\n"
                      "    Quedan %d 'fi' en el archivo pero hay %d bloque(s) abierto(s) esperando el suyo.",
                      line, remaining, fi_block_nesting);
            }
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

    fi_block_nesting--;
    return stmt;
}

ASTNode *parse_if_statement() {
    Token t = ts_peek();
    int line = t.line;
    ts_advance();
    fi_block_nesting++;
    if (!is_expression_start(ts_peek().type)) {
        error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                 "Se esperaba una condición después de 'if'");
    }
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
        if (!is_expression_start(ts_peek().type)) {
            error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                     "Se esperaba una condición después de 'elseif'");
        }
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
    fi_block_nesting--;
    return first_if;
}

/*
 * Determina si el token actual es el inicio de una expresión de
 * post-incremento/decremento (x++ o x--) seguida solo de un salto
 * de línea o EOF. En tal caso NO es un comando shell, aunque empiece
 * por un identificador y esté seguido por un token que no sea '(' ni '['.
 * La posición del parser no se modifica.
 *
 * Misma regla que en parse_block: en cuanto hay algo después de ++/--,
 * se considera un comando (g++ --help, g++ archivo, etc.).
 */
static bool rhs_is_post_op(void) {
    Token t = ts_peek();
    if (t.type != TOK_IDENT) return false;
    if (ts.pos + 1 >= ts.count) return false;
    TokenType incdec = ts.tokens[ts.pos + 1].type;
    if (incdec != TOK_INC && incdec != TOK_DEC) return false;
    int after = ts.pos + 2;
    if (after >= ts.count) return true;
    TokenType at = ts.tokens[after].type;
    return at == TOK_NEWLINE || at == TOK_EOF;
}

ASTNode *parse_assignment_expr(int line) {
    DEBUG_INFO("parse_assignment_expr: ¡LLAMADA! línea %d", line);
    if (ts_peek().type != TOK_IDENT) {
        error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                 "Se esperaba una variable en la asignación");
    }
    Token var_tok = ts_advance();
    char *varname = clean_var_name(var_tok.lexeme);
    validate_var_name(varname, line);

    /* Crear el nodo antes de parsear el valor. Si parse_expression() lanza un
     * error, el registro del AST también se encargará de liberar varname. */
    ASTNode *assign = node_create(NODE_ASSIGN, line);
    assign->data.assign.name = varname;
    assign->data.assign.value = NULL;
    assign->data.assign.vtype = 0;
    assign->data.assign.is_local = false;
    assign->data.assign.is_global = false;
    assign->data.assign.is_cmd = false;
    assign->data.assign.cmd_str = NULL;
    assign->data.assign.lhs_index = NULL;

    int op = ts_peek().type;
    if (op != TOK_EQ && op != TOK_PLUS_EQ && op != TOK_MINUS_EQ &&
        op != TOK_STAR_EQ && op != TOK_SLASH_EQ && op != TOK_PERCENT_EQ) {
        error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                 "Se esperaba '=', '+=', '-=', '*=', '/=' o '%=' después de '%s'",
                 varname);
        }
        Token op_tok = ts_advance();

    bool is_cmd = false;
    char *cmd_str = NULL;
    ASTNode *value = NULL;

    /* Solo para '=' simple, detectar si es un comando. */
    if (op == TOK_EQ) {
        Token next_token = ts_peek();
        DEBUG_INFO("parse_assignment_expr: valor siguiente '%s' (tipo %d)", next_token.lexeme, next_token.type);
        if (next_token.type == TOK_IDENT && next_token.lexeme[0] != '$' && next_token.lexeme[0] != '?') {
            int save_pos = ts.pos;
            ts_advance();                 /* consumir el identificador */
            Token next_next = ts_peek();
            ts.pos = save_pos;

            /* Un identificador desnudo en el lado derecho de '=' es SIEMPRE
             * un comando. Se exceptúan:
             *   - llamadas a función: f(...)
             *   - indexación:         l[...]
             *   - post-incremento/decremento sueltos: x++  o  x--
             * Para usar el valor de una variable hay que escribir $var. */
            bool post_op = rhs_is_post_op();
            if (!post_op && next_next.type != TOK_LPAREN && next_next.type != TOK_LBRACKET) {
                is_cmd = true;
                cmd_str = extract_literal_command(line);
                DEBUG_INFO("parse_assignment_expr: comando detectado: '%s'", cmd_str);
                while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                    ts_advance();
                }
            }
        }
    }

    if (!is_cmd) {
        DEBUG_INFO("parse_assignment_expr: parseando expresión normal");
        Token rhs_start = ts_peek();
        if (!is_expression_start(rhs_start.type)) {
            error_at(rhs_start.line, rhs_start.start_col > 0 ? rhs_start.start_col : 1,
                     "Se esperaba un valor después del operador de asignación '%s'", op_tok.lexeme);
        }
        value = parse_expression(0);
        if (!value) {
            value = node_create(NODE_LITERAL, line);
            value->data.lit.type = TOK_INT;
            value->data.lit.ival = 0;
        }
        require_statement_end("la asignación");
    }

    if (op == TOK_EQ) {
        assign->data.assign.value = value;
        assign->data.assign.is_cmd = is_cmd;
        assign->data.assign.cmd_str = cmd_str;
        DEBUG_INFO("parse_assignment_expr: creado NODE_ASSIGN para '%s', is_cmd=%d", varname, is_cmd);
        return assign;
    }

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
        case TOK_PLUS_EQ:    binop->data.binop.op = TOK_PLUS;    break;
        case TOK_MINUS_EQ:   binop->data.binop.op = TOK_MINUS;   break;
        case TOK_STAR_EQ:    binop->data.binop.op = TOK_STAR;    break;
        case TOK_SLASH_EQ:   binop->data.binop.op = TOK_SLASH;   break;
        case TOK_PERCENT_EQ: binop->data.binop.op = TOK_PERCENT; break;
        default: break;
    }
    assign->data.assign.value = binop;
    return assign;
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

        if (t.type == TOK_IDENT && strcmp(t.lexeme, "funuction") == 0) {
            error(t.line, "Error de sintaxis: quizás querías decir \"function\"?");
        }

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

        /* --- PORTAL --- */
        if (t.type == TOK_PORTAL) {
            ts_advance();
            if (ts_peek().type != TOK_IDENT)
                error(t.line, "Se esperaba nombre de portal después de 'portal'");
            char *portal_name = strdup(ts_advance().lexeme);
            stmt = node_create(NODE_PORTAL, t.line);
            stmt->data.portal.name = portal_name;
            stmt->data.portal.is_local = false;
            require_statement_end("'portal'");
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
            if (!is_expression_start(ts_peek().type)) {
                error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                         "Se esperaba una condición después de 'while'");
            }
            ASTNode *cond = parse_expression(0);
            if (!ts_match(TOK_THEN)) error_missing_then(t.line, "while");
            fi_block_nesting++;
            NodeList body = parse_block("fi");
            if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi'");
            fi_block_nesting--;
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

            /* FOR-IN con índice: for i, elemento in lista then
             *
             * Las variables del for-in son SIEMPRE locales al bucle, por lo
             * que aquí no se admite 'local'/'global' ni tipo explícito.
             * El primer identificador actúa como índice (base 1) y el
             * segundo recibe el valor de cada elemento.
             *
             * Solo tratamos la coma como separador de for-in con índice si
             * el patrón es exactamente IDENT ',' IDENT 'in'. Si no, dejamos
             * caer el flujo al parseo de for tradicional, que dará el error
             * correcto ('=' faltante tras el nombre, etc.). Así no
             * confundimos un for tradicional mal escrito con un for-in al
             * que le falta el 'in'. */
            if (ts_peek().type == TOK_COMMA &&
                ts.pos + 2 < ts.count &&
                ts.tokens[ts.pos + 1].type == TOK_IDENT &&
                ts.tokens[ts.pos + 2].type == TOK_IN) {
                ts_advance();  /* consumir ',' */
                if (ts_peek().type != TOK_IDENT) {
                    error(t.line, "Se esperaba nombre de variable después de ',' en for-in");
                }
                char *second = clean_var_name(ts_advance().lexeme);
            validate_var_name(second, t.line);
            ts_advance();  /* consumir 'in' */
            if (!is_expression_start(ts_peek().type)) {
                error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                         "Se esperaba una expresión después de 'in'");
            }
            ASTNode *list_expr = parse_expression(0);
            if (!ts_match(TOK_THEN)) error_missing_then(t.line, "for-in");
            fi_block_nesting++;
                NodeList body = parse_block("fi");
                if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi'");
                fi_block_nesting--;
                stmt = node_create(NODE_FOR_IN, t.line);
                stmt->data.for_in.index_var = varname;   /* primer identificador: índice */
                stmt->data.for_in.var = second;          /* segundo identificador: valor */
                stmt->data.for_in.list_expr = list_expr;
                stmt->data.for_in.body = body;
                nodelist_add(&block, stmt);
                DEBUG_INFO("parse_block: añadido NODE_FOR_IN con índice en línea %d", stmt->line);
                ts_skip_newlines();
                continue;
                }

                // FOR-IN
                if (ts_peek().type == TOK_IN) {
                    ts_advance();
                    if (!is_expression_start(ts_peek().type)) {
                        error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                                 "Se esperaba una expresión después de 'in'");
                    }
                    ASTNode *list_expr = parse_expression(0);
                    if (!ts_match(TOK_THEN)) error_missing_then(t.line, "for-in");
                    fi_block_nesting++;
                    NodeList body = parse_block("fi");
                    if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi'");
                    fi_block_nesting--;
                    stmt = node_create(NODE_FOR_IN, t.line);
                    stmt->data.for_in.index_var = NULL;
                    stmt->data.for_in.var = varname;
                    stmt->data.for_in.list_expr = list_expr;
                    stmt->data.for_in.body = body;
                    nodelist_add(&block, stmt);
                    DEBUG_INFO("parse_block: añadido NODE_FOR_IN en línea %d", stmt->line);
                    ts_skip_newlines();
                    continue;
                }

                // FOR tradicional
                //
                // Formas válidas:
                //   for i = 0, cond, incr then              → init explícito
                //   for local int i, cond, incr then        → init = valor por defecto de int (0)
                //   for float f, cond, incr then            → init = 0.0
                //   for string s, cond, incr then           → init = ""
                //   etc.
                //
                // El valor por defecto solo se aplica si hay un tipo declarado
                // (vtype != 0). Sin tipo, el valor inicial es obligatorio, porque
                // no hay forma de saber qué tipo de valor por defecto usar.
                ASTNode *init_expr = NULL;
                if (ts_match(TOK_EQ)) {
                    if (!is_expression_start(ts_peek().type)) {
                        error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                                 "Se esperaba el valor inicial del for después de '='");
                    }
                    init_expr = parse_expression(0);
                } else if (vtype != 0 && ts_peek().type == TOK_COMMA) {
                    /* El tipo ya determina el valor inicial. init_expr queda NULL
                     * y eval_stmt usará el valor por defecto del tipo. */
                    init_expr = NULL;
                } else if (is_local || is_global) {
                    /* 'for local i, ...' sin tipo y sin '=' es ambiguo:
                     * no se puede decidir el tipo de la variable. */
                    error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                             "La declaración 'for %s %s' es ambigua.\n"
                             "    No se indicó ni un tipo ni un valor inicial, así que no se puede\n"
                             "    determinar el tipo de '%s'.\n"
                             "    Especifica un tipo o asígnale un valor:\n"
                             "        for %s int %s, condición, incremento then      (usa el valor por defecto de int)\n"
                             "        for %s %s = 0, condición, incremento then      (infiere el tipo del valor)",
                             is_local ? "local" : "global", varname,
                             varname,
                             is_local ? "local" : "global", varname,
                             is_local ? "local" : "global", varname);
                } else {
                    error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                             "Se esperaba '=' después de la variable del for.\n"
                             "    La sintaxis del for tradicional es:\n"
                             "        for variable = inicio, condición, incremento then\n"
                             "        for tipo variable, condición, incremento then   (usa el valor por defecto del tipo)\n"
                             "    Por ejemplo:  for i = 0, i <= 3, i++ then\n"
                             "                  for local int i, i <= 3, i++ then\n"
                             "    Para recorrer una lista usa for-in:\n"
                             "        for elemento in lista then\n"
                             "        for i, elemento in lista then");
                }

                ASTNode *init = node_create(NODE_ASSIGN, t.line);
                init->data.assign.name = varname;
                init->data.assign.value = init_expr;
                init->data.assign.vtype = vtype;
                init->data.assign.is_local = is_local;
                init->data.assign.is_global = is_global;
                init->data.assign.is_cmd = false;
                init->data.assign.cmd_str = NULL;
                init->data.assign.lhs_index = NULL;

                if (!ts_match(TOK_COMMA)) {
                    error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                             "Se esperaba ',' después de la inicialización del for");
                }

                if (!is_expression_start(ts_peek().type)) {
                    error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                             "Se esperaba la condición del for después de la primera ','");
                }
                ASTNode *cond = parse_expression(0);

                if (!ts_match(TOK_COMMA)) {
                    error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                             "Se esperaba ',' después de la condición del for");
                }

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
                        next_next.type == TOK_PERCENT_EQ || next_next.type == TOK_EQ) {
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
                fi_block_nesting++;
            NodeList body = parse_block("fi");
            if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi' para cerrar el for");
            fi_block_nesting--;

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

            if (!ts_match(TOK_LPAREN)) {
                error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                         "Se esperaba '(' después del nombre de la función '%s'", stmt->data.func.name);
            }
            if (!ts_match(TOK_RPAREN)) {
                do {
                    int ptype = 0;
                    TokenType tt = ts_peek().type;
                    if (tt == TOK_INT || tt == TOK_FLOAT || tt == TOK_BOOL || tt == TOK_STRING || tt == TOK_LIST || tt == TOK_MAP)
                        ptype = ts_advance().type;
                    if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba nombre de parámetro");
                    char *pname = clean_var_name(ts_advance().lexeme);
                    validate_var_name(pname, t.line);
                    for (int i = 0; i < stmt->data.func.param_count; i++) {
                        if (strcmp(stmt->data.func.params[i], pname) == 0)
                            error(t.line, "El parámetro '%s' está repetido", pname);
                    }
                    int pcount = stmt->data.func.param_count;
                    stmt->data.func.params = realloc(stmt->data.func.params, (pcount + 1) * sizeof(char*));
                    stmt->data.func.ptypes = realloc(stmt->data.func.ptypes, (pcount + 1) * sizeof(int));
                    stmt->data.func.params[pcount] = pname;
                    stmt->data.func.ptypes[pcount] = ptype;
                    stmt->data.func.param_count = pcount + 1;
                    Token sep = ts_peek();
                    if (sep.type != TOK_COMMA && sep.type != TOK_RPAREN)
                        error_at(sep.line, sep.start_col > 0 ? sep.start_col : 1,
                                 "Falta ',' entre parámetros de la función '%s'", stmt->data.func.name);
                } while (ts_match(TOK_COMMA) && ts_peek().type != TOK_RPAREN);
                if (ts_peek().type == TOK_RPAREN && ts.pos > 0 &&
                    ts.tokens[ts.pos - 1].type == TOK_COMMA) {
                    error_at(ts_peek().line, ts_peek().start_col,
                             "No puede haber una coma al final de los parámetros de '%s'", stmt->data.func.name);
                    }
                    if (!ts_match(TOK_RPAREN)) {
                        error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                                 "Se esperaba ')' para cerrar la declaración de '%s'", stmt->data.func.name);
                    }
            }

            fi_block_nesting++;
            stmt->data.func.body = parse_block("fi");
            if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi'");
            fi_block_nesting--;

            func_register(stmt->data.func.name, stmt);
            if (current_import_prefix) {
                char prefixed[512];
                snprintf(prefixed, sizeof(prefixed), "%s.%s", current_import_prefix, stmt->data.func.name);
                func_register(prefixed, stmt);
            }

            /* NO compilar aquí. La compilación se hará en compile_program */
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
            require_statement_end("'break'");
            stmt = node_create(NODE_BREAK, t.line);
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_BREAK en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- CONTINUE --- */
        if (t.type == TOK_CONTINUE) {
            ts_advance();
            require_statement_end("'continue'");
            stmt = node_create(NODE_CONTINUE, t.line);
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_CONTINUE en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- REPEAT --- */
        if (t.type == TOK_REPEAT) {
            ts_advance();
            if (ts_peek().type == TOK_IDENT) {
                char *portal_name = strdup(ts_advance().lexeme);
                stmt = node_create(NODE_REPEAT, t.line);
                stmt->data.repeat.portal_name = portal_name;
                stmt->data.repeat.line_expr = NULL;
            } else if (ts_match(TOK_LINE)) {
                if (!is_expression_start(ts_peek().type)) {
                    error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                             "Se esperaba una expresión después de 'repeat line'");
                }
                ASTNode *line_expr = parse_expression(0);
                stmt = node_create(NODE_REPEAT, t.line);
                stmt->data.repeat.line_expr = line_expr;
                stmt->data.repeat.portal_name = NULL;
            } else {
                error(t.line, "Se esperaba nombre de portal o 'line' después de 'repeat'");
            }
            require_statement_end("'repeat'");
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
            bool is_quoted_path = false;

            if (nt.type == TOK_IDENT) {
                /* Identificador desnudo: es SIEMPRE un nombre de módulo,
                 * aunque contenga '/' o '.' (p. ej. build/lava/ejemplo.lava). */
                module_name = strdup(nt.lexeme);
                ts_advance();
                if (embedded_find(module_name, &emb_data, &emb_size, NULL)) {
                    use_embedded = 1;
                }
            } else if (nt.type == TOK_STRING_LITERAL) {
                /* Entre comillas: es una ruta directa. */
                ts_advance();
                module_name = strdup(nt.lexeme);
                is_quoted_path = true;
            } else {
                error(t.line, "Se esperaba nombre o ruta en import");
            }

            if (!is_quoted_path && !valid_module_name(module_name)) {
                free(module_name);
                error(t.line, "Nombre de módulo inválido: %s", nt.lexeme);
            }

            /* import <módulo/ruta> as <alias> */
            if (ts_peek().type == TOK_IDENT && strcmp(ts_peek().lexeme, "as") == 0) {
                ts_advance();
                if (ts_peek().type != TOK_IDENT) {
                    free(module_name);
                    error(t.line, "Se esperaba un alias después de 'as'");
                }
                module_alias = strdup(ts_advance().lexeme);
                if (!valid_module_name(module_alias)) {
                    free(module_name);
                    free(module_alias);
                    error(t.line, "Alias de módulo inválido: %s", ts_peek().lexeme);
                }
            }

            TokenStream old_ts = ts;
            char **old_source_lines = source_lines;
            int old_source_line_count = source_line_count;

            /* ============================================================
             * CASO A: ruta entre comillas → ruta directa.
             * ============================================================ */
            if (is_quoted_path && !use_embedded) {
                char tried[2048] = "";
                const char *prefix = module_alias ? module_alias : NULL;

                if (lava_try_import_path(module_name, prefix, tried, sizeof(tried))) {
                    stmt = node_create(NODE_IMPORT, t.line);
                    stmt->data.import.path = NULL;
                    stmt->data.import.alias = module_alias;
                    stmt->data.import.module_block = (NodeList){NULL, 0, 0};
                    nodelist_add(&block, stmt);

                    DEBUG_INFO("parse_block: módulo Lava '%s' cargado por ruta en línea %d",
                               module_name, stmt->line);
                    free(module_name);
                    ts_skip_newlines();
                    continue;
                }

                FILE *fp = fopen(module_name, "r");
                if (!fp) {
                    free(module_name);
                    free(module_alias);
                    error(t.line,
                          "No se pudo importar '%s'.\n"
                          "    No existe como librería Lava ni como archivo .fire legible.\n"
                          "    Rutas probadas (Lava):\n%s",
                          nt.lexeme,
                          tried[0] ? tried : "        (ninguna)\n");
                }

                ts_init();
                source_lines = NULL;
                source_line_count = 0;
                tokenize_file(fp);
                fclose(fp);
            }
            /* ============================================================
             * CASO B: nombre embebido.
             * ============================================================ */
            else if (use_embedded) {
                ts_init();
                source_lines = NULL;
                source_line_count = 0;
                tokenize_buffer((const char *)emb_data, emb_size);
            }
            /* ============================================================
             * CASO C: nombre de módulo.
             *   Orden:
             *     1. ~/.infernal/fire/<name>.fire
             *     2. /usr/share/infernal/fire/<name>.fire
             *     3. ~/.infernal/lava/<name>.lava
             *     4. /usr/share/infernal/lava/<name>.lava
             * ============================================================ */
            else {
                FILE *fire_fp = NULL;
                char fire_local[PATH_MAX];
                char fire_global[PATH_MAX];
                const char *home = getenv("HOME");

                fire_local[0]  = '\0';
                fire_global[0] = '\0';

                if (home && *home) {
                    snprintf(fire_local, sizeof(fire_local),
                             "%s/.infernal/fire/%s.fire", home, module_name);
                    fire_fp = fopen(fire_local, "r");
                }
                if (!fire_fp) {
                    snprintf(fire_global, sizeof(fire_global),
                             "/usr/share/infernal/fire/%s.fire", module_name);
                    fire_fp = fopen(fire_global, "r");
                }

                if (fire_fp) {
                    ts_init();
                    source_lines = NULL;
                    source_line_count = 0;
                    tokenize_file(fire_fp);
                    fclose(fire_fp);
                } else {
                    /* No hay .fire: probar Lava. Prefijo por defecto =
                     * basename sin extensión (o el alias, si se dio). */
                    char prefix_buf[PATH_MAX];
                    const char *base = module_name;
                    const char *slash = strrchr(module_name, '/');
                    if (slash) base = slash + 1;
                    snprintf(prefix_buf, sizeof(prefix_buf), "%s", base);
                    char *dot = strrchr(prefix_buf, '.');
                    if (dot) *dot = '\0';

                    const char *prefix = module_alias ? module_alias : prefix_buf;
                    char tried[2048] = "";

                    if (lava_try_import(module_name, prefix, tried, sizeof(tried))) {
                        stmt = node_create(NODE_IMPORT, t.line);
                        stmt->data.import.path = NULL;
                        stmt->data.import.alias = module_alias;
                        stmt->data.import.module_block = (NodeList){NULL, 0, 0};
                        nodelist_add(&block, stmt);

                        DEBUG_INFO("parse_block: módulo Lava '%s' cargado en línea %d",
                                   prefix, stmt->line);
                        free(module_name);
                        ts_skip_newlines();
                        continue;
                    }

                    /* Error completo con TODAS las rutas probadas. */
                    const char *fire_local_msg = (fire_local[0] != '\0')
                    ? fire_local
                    : "(HOME no definido)";
                    const char *fire_global_msg = (fire_global[0] != '\0')
                    ? fire_global
                    : "(ruta global no construida)";

                    free(module_name);
                    free(module_alias);
                    error(t.line,
                          "No se pudo importar '%s'.\n"
                          "    Ni es un módulo Fire ni una librería Lava. Si quieres usar una ruta relativa al script, tienes que poner la ruta entre comillas.\n\n"
                          "    Rutas probadas como Fire:\n"
                          "        %s\n"
                          "        %s\n"
                          "    Rutas probadas como Lava:\n%s",
                          nt.lexeme,
                          fire_local_msg,
                          fire_global_msg,
                          tried[0] ? tried : "        (ninguna)\n");
                }
            }

            /* ============================================================
             * Cuerpo del .fire (común a los tres caminos anteriores).
             * ============================================================ */
            {
                char *prefix_base = module_name;
                char *slash = strrchr(module_name, '/');
                if (slash) prefix_base = slash + 1;
                char *dot = strrchr(prefix_base, '.');
                if (dot) *dot = '\0';

                char *old_prefix = current_import_prefix;
                current_import_prefix = module_alias ? module_alias : prefix_base;
                NodeList module_block = parse_block(NULL);
                current_import_prefix = old_prefix;

                for (int i = 0; i < ts.count; i++) free(ts.tokens[i].lexeme);
                free(ts.tokens);
                ts = old_ts;

                for (int i = 0; i < source_line_count; i++) free(source_lines[i]);
                free(source_lines);
                source_lines = old_source_lines;
                source_line_count = old_source_line_count;

                free(module_name);
                require_statement_end("la instrucción import");
                stmt = node_create(NODE_IMPORT, t.line);
                stmt->data.import.path = NULL;
                stmt->data.import.alias = module_alias;
                stmt->data.import.module_block = module_block;
                nodelist_add(&block, stmt);
                DEBUG_INFO("parse_block: añadido NODE_IMPORT en línea %d", stmt->line);
                ts_skip_newlines();
                continue;
            }
        }

        /* --- TRY --- */
        if (t.type == TOK_TRY) {
            ts_advance();
            fi_block_nesting++;
            NodeList try_block = parse_block("catch");
            if (!ts_match(TOK_CATCH)) error(t.line, "Se esperaba 'catch' para cerrar el bloque 'try'");
            NodeList catch_block = parse_block("fi");
            if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi'");
            fi_block_nesting--;
            stmt = node_create(NODE_TRY, t.line);
            stmt->data.try_stmt.try_block = try_block;
            stmt->data.try_stmt.catch_block = catch_block;
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadido NODE_TRY en línea %d", stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- DEFINE: declaración de constantes --- */
        if (t.type == TOK_DEFINE) {
            ts_advance();

            Token name_tok = ts_peek();
            if (name_tok.type != TOK_IDENT) {
                error_at(name_tok.line, name_tok.start_col > 0 ? name_tok.start_col : 1,
                         "Se esperaba el nombre de la constante después de 'define'");
            }

            ts_advance();
            char *name = clean_var_name(name_tok.lexeme);
            validate_var_name(name, t.line);

            if (!is_expression_start(ts_peek().type)) {
                error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                         "Se esperaba un valor después del nombre de la constante '%s'", name);
            }

            ASTNode *value = parse_expression(0);
            if (!value) {
                error(t.line, "Se esperaba un valor para la constante '%s'", name);
            }
            require_statement_end("la definición de la constante");

            stmt = node_create(NODE_DEFINE, t.line);
            stmt->data.define.name = name;
            stmt->data.define.value = value;
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadida constante '%s' en línea %d", name, stmt->line);
            ts_skip_newlines();
            continue;
        }

        /* --- LOCAL / GLOBAL / TIPO: declaraciones de variables --- */
        if (t.type == TOK_LOCAL || t.type == TOK_GLOBAL) {
            bool is_local = ts_match(TOK_LOCAL);
            bool is_global = ts_match(TOK_GLOBAL);

            DEBUG_INFO("parse_block: declaración %s en línea %d", is_local ? "local" : "global", t.line);

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

                    /* --- Indexación anidada en la declaración ---
                     *
                     *   map pages[web][pagina] = []
                     *   list casas[1] = "algo"
                     *
                     * Si tras el nombre viene '[', construimos una cadena de
                     * NODE_INDEX cuyo nodo raíz es un NODE_VAR con el nombre.
                     * El runtime creará la variable y los contenedores
                     * intermedios que falten.
                     *
                     * Indexación solo tiene sentido para map y list. */
                    ASTNode *lhs_index = NULL;
                    if (ts_peek().type == TOK_LBRACKET) {
                        if (vtype != TOK_MAP && vtype != TOK_LIST) {
                            const char *tipo_str = "ese tipo";
                            if (vtype == TOK_INT)         tipo_str = "int";
                            else if (vtype == TOK_FLOAT)  tipo_str = "float";
                            else if (vtype == TOK_BOOL)   tipo_str = "bool";
                            else if (vtype == TOK_STRING) tipo_str = "string";
                            else if (vtype == 0)          tipo_str = "sin tipo declarado";

                            error(t.line,
                                  "No puedes poner corchetes '[ ... ]' justo después del nombre al declarar una variable de tipo %s.\n"
                                  "    Los corchetes solo sirven para decir \"qué parte del map/list quiero tocar\".\n"
                                  "    Ejemplos válidos:\n"
                                  "        map %s[clave] = []              (crea un submapa dentro del map %s)\n"
                                  "        map %s[clave1][clave2] = []     (crea un submapa dentro de otro submapa)\n"
                                  "        list %s[1] = valor              (guarda 'valor' en la posición 1 de la lista %s)\n"
                                  "    Si solo quieres declarar %s sin tocar sus elementos, escríbelo así:\n"
                                  "        %s %s = ...",
                                  tipo_str,
                                  vname, vname,
                                  vname,
                                  vname, vname,
                                  vname,
                                  tipo_str, vname);
                        }
                        ASTNode *base = node_create(NODE_VAR, t.line);
                        base->data.var.name = strdup(vname);
                        base->data.var.clone = false;
                        ASTNode *prev = base;
                        while (ts_peek().type == TOK_LBRACKET) {
                            Token lb = ts_advance();
                            ASTNode *idx = parse_expression(0);
                            if (!ts_match(TOK_RBRACKET)) error(lb.line, "Se esperaba ']' para cerrar el índice");
                            ASTNode *ni = node_create(NODE_INDEX, lb.line);
                            ni->data.idx.list = prev;
                            ni->data.idx.index = idx;
                            prev = ni;
                        }
                        lhs_index = prev;
                    }

                    ASTNode *value = NULL;
                    bool is_cmd = false;
                    char *cmd_str = NULL;

                    if (ts_match(TOK_EQ)) {
                        Token next_token = ts_peek();
                        bool try_cmd = (lhs_index == NULL) &&
                        next_token.type == TOK_IDENT &&
                        next_token.lexeme[0] != '$' && next_token.lexeme[0] != '?';
                        if (try_cmd) {
                            int save_pos = ts.pos;
                            ts_advance();                 /* consumir el identificador */
                            Token rhs_follow = ts_peek();
                            ts.pos = save_pos;

                            /* Un identificador desnudo a la derecha de '=' es
                             * siempre un comando. Se exceptúan:
                             *   - llamadas a función: f(...)
                             *   - indexación:         l[...]
                             *   - post-incremento/decremento sueltos: x++  o  x--
                             * Para usar el valor de una variable hay que usar $var. */
                            bool post_op = rhs_is_post_op();
                            if (!post_op && rhs_follow.type != TOK_LPAREN &&
                                rhs_follow.type != TOK_LBRACKET) {
                                is_cmd = true;
                            cmd_str = extract_literal_command(t.line);
                            while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) ts_advance();
                                } else {
                                    value = parse_typed_empty_collection(vtype, t.line);
                                    if (!value) value = parse_expression(0);
                                }
                        } else {
                            value = parse_typed_empty_collection(vtype, t.line);
                            if (!value) value = parse_expression(0);
                        }
                    }

                    /* Una declaración sin tipo ni valor inicial es ambigua:
                     * no hay forma de saber qué tipo debe tener la variable.
                     * Especificar el tipo (int, float, ...) o darle un valor
                     * resuelve la ambigüedad. */
                    if (!is_cmd && value == NULL && vtype == 0) {
                        error(t.line,
                              "Declaración '%s %s' sin tipo ni valor inicial.\n"
                              "    La variable '%s' es ambigua: no se puede determinar su tipo.\n"
                              "    Especifica un tipo o asígnale un valor:\n"
                              "        %s int %s           (usa el valor por defecto de int, 0)\n"
                              "        %s %s = 0           (tipado automático del valor dado)",
                              is_local ? "local" : "global", vname,
                              vname,
                              is_local ? "local" : "global", vname,
                              is_local ? "local" : "global", vname);
                    }

                    stmt = node_create(NODE_ASSIGN, t.line);
                    stmt->data.assign.name = vname;
                    stmt->data.assign.is_cmd = is_cmd;
                    stmt->data.assign.cmd_str = cmd_str;
                    stmt->data.assign.value = value; /* NULL = valor por defecto del tipo */
                    stmt->data.assign.vtype = vtype;
                    stmt->data.assign.is_local = is_local;
                    stmt->data.assign.is_global = is_global;
                    stmt->data.assign.lhs_index = lhs_index;
                    nodelist_add(&block, stmt);

                    if (is_cmd) {
                        /* extract_literal_command consume el resto de la línea. */
                        break;
                    }

                    if (lhs_index) {
                        /* Una declaración con índice no admite ',' y más
                         * variables en la misma línea: la ruta afecta solo
                         * a un destino. */
                        require_statement_end("la declaración");
                        break;
                    }

                    if (!ts_match(TOK_COMMA)) {
                        require_statement_end("la declaración");
                        break;
                    }

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

            /* --- Indexación anidada en la declaración tipada ---
             *
             *   map pages[web][pagina] = []
             *   list casas[1] = "algo"
             *
             * Ver el comentario extenso en el bloque local/global de arriba.
             * Aquí va el mismo tratamiento. */
            ASTNode *lhs_index = NULL;
            if (ts_peek().type == TOK_LBRACKET) {
                if (vtype != TOK_MAP && vtype != TOK_LIST) {
                    const char *tipo_str = "ese tipo";
                    if (vtype == TOK_INT)         tipo_str = "int";
                    else if (vtype == TOK_FLOAT)  tipo_str = "float";
                    else if (vtype == TOK_BOOL)   tipo_str = "bool";
                    else if (vtype == TOK_STRING) tipo_str = "string";

                    error(t.line,
                          "No puedes poner corchetes '[ ... ]' justo después del nombre al declarar una variable de tipo %s.\n"
                          "    Los corchetes solo sirven para decir \"qué parte del map/list quiero tocar\".\n"
                          "    Ejemplos válidos:\n"
                          "        map %s[clave] = []              (crea un submapa dentro del map %s)\n"
                          "        map %s[clave1][clave2] = []     (crea un submapa dentro de otro submapa)\n"
                          "        list %s[1] = valor              (guarda 'valor' en la posición 1 de la lista %s)\n"
                          "    Si solo quieres declarar %s sin tocar sus elementos, escríbelo así:\n"
                          "        %s %s = ...",
                          tipo_str,
                          vname, vname,
                          vname,
                          vname, vname,
                          vname,
                          tipo_str, vname);
                }
                ASTNode *base = node_create(NODE_VAR, t.line);
                base->data.var.name = strdup(vname);
                base->data.var.clone = false;
                ASTNode *prev = base;
                while (ts_peek().type == TOK_LBRACKET) {
                    Token lb = ts_advance();
                    ASTNode *idx = parse_expression(0);
                    if (!ts_match(TOK_RBRACKET)) error(lb.line, "Se esperaba ']' para cerrar el índice");
                    ASTNode *ni = node_create(NODE_INDEX, lb.line);
                    ni->data.idx.list = prev;
                    ni->data.idx.index = idx;
                    prev = ni;
                }
                lhs_index = prev;
            }

            ASTNode *value = NULL;
            bool is_cmd = false;
            char *cmd_str = NULL;

            if (ts_match(TOK_EQ)) {
                Token next_token = ts_peek();
                bool try_cmd = (lhs_index == NULL) &&
                next_token.type == TOK_IDENT &&
                next_token.lexeme[0] != '$' && next_token.lexeme[0] != '?';
                if (try_cmd) {
                    int save_pos = ts.pos;
                    ts_advance();                 /* consumir el identificador */
                    Token rhs_follow = ts_peek();
                    ts.pos = save_pos;

                    /* Un identificador desnudo a la derecha de '=' es
                     * siempre un comando (excepto llamada o indexación).
                     * Para usar el valor de una variable hay que usar $var. */
                    if (rhs_follow.type != TOK_LPAREN && rhs_follow.type != TOK_LBRACKET) {
                        is_cmd = true;
                        cmd_str = extract_literal_command(t.line);
                        while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) ts_advance();
                    } else {
                        value = parse_typed_empty_collection(vtype, t.line);
                        if (!value) value = parse_expression(0);
                    }
                } else {
                    value = parse_typed_empty_collection(vtype, t.line);
                    if (!value) value = parse_expression(0);
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
            stmt->data.assign.lhs_index = lhs_index;
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadida declaración tipada de '%s' en línea %d", vname, stmt->line);

            if (is_cmd)
                break;
            if (lhs_index) {
                /* Con índice anidado no permitimos más variables en la misma
                 * línea: la ruta apunta a un único destino dentro del contenedor. */
                require_statement_end("la declaración tipada");
                break;
            }
            if (!ts_match(TOK_COMMA)) {
                require_statement_end("la declaración tipada");
                break;
            }
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
                    next_tok.type == TOK_SLASH_EQ || next_tok.type == TOK_PERCENT_EQ) {
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
                        require_statement_end("la llamada a función");
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
                            if (idx_expr->kind == NODE_INDEX) {
                                ASTNode *base = idx_expr;
                                while (base->kind == NODE_INDEX) base = base->data.idx.list;
                                if (base->kind == NODE_VAR) {
                                    vname = strdup(base->data.var.name);
                                }
                            }
                            if (!vname) error(saved_t.line, "Lado izquierdo inválido para asignación con índice");
                            if (!is_expression_start(ts_peek().type)) {
                                error_at(ts_peek().line, ts_peek().start_col > 0 ? ts_peek().start_col : 1,
                                         "Se esperaba un valor después de '=' en la asignación con índice");
                            }
                            ASTNode *value = parse_expression(0);
                            require_statement_end("la asignación con índice");
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
                            require_statement_end("la indexación");
                            stmt = node_create(NODE_EXPR_STMT, saved_t.line);
                            stmt->data.expr_stmt.expr = idx_expr;
                            nodelist_add(&block, stmt);
                            DEBUG_INFO("parse_block: indexación en línea %d", stmt->line);
                            ts_skip_newlines();
                            continue;
                        }
                    }

                    /* POST-INCREMENTO/DECREMENTO (g++ o g--)
                     *
                     * Regla inequívoca para comandos que contienen `++`/`--`:
                     *
                     *   g++          -> variable `g`, post-incremento
                     *   g--          -> variable `g`, post-decremento
                     *   g++ --help   -> comando shell completo
                     *   g++ archivo  -> comando shell completo
                     *
                     * Es decir, solo reconocemos el operador como post-op cuando
                     * NO queda ningún token después de ++/-- en esa línea. En cuanto
                     * hay un argumento, `++`/`--` forma parte del comando. */
                    if (next_tok.type == TOK_INC || next_tok.type == TOK_DEC) {
                        Token after_op = (ts.pos + 1 < ts.count)
                        ? ts.tokens[ts.pos + 1]
                        : (Token){TOK_EOF, "", 0, 0, 0};
                        bool post_op_only = (after_op.type == TOK_NEWLINE ||
                        after_op.type == TOK_EOF);
                        if (post_op_only) {
                            ts.pos--;
                            ASTNode *expr = parse_expression(0);
                            /* Marcar como sentencia suelta: SOLO en este
                             * contexto el runtime puede interpretar `g++`
                             * como un comando si la variable no existe.
                             * Un `j++` dentro de un for o de una expresión
                             * NO es candidato a comando: es una variable
                             * que debe existir. */
                            if (expr->kind == NODE_POST_INC || expr->kind == NODE_POST_DEC) {
                                expr->data.post_op.statement_context = true;
                            }
                            stmt = node_create(NODE_EXPR_STMT, saved_t.line);
                            stmt->data.expr_stmt.expr = expr;
                            nodelist_add(&block, stmt);
                            DEBUG_INFO("parse_block: post-inc/dec para '%s' en línea %d", saved_t.lexeme, stmt->line);
                            ts_skip_newlines();
                            continue;
                        }
                        /* Quedan tokens tras ++/--; toda la línea es un comando shell. */
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

            /* --- Diagnósticos de sintaxis de alto nivel --- */
            if (t.type == TOK_FI)
                error_at(t.line, t.start_col, "'fi' no tiene un bloque abierto que cerrar aquí");
        if (t.type == TOK_ELSE || t.type == TOK_ELSEIF)
            error_at(t.line, t.start_col, "'%s' solo puede aparecer dentro de un 'if'", t.lexeme);
        if (t.type == TOK_CASE || t.type == TOK_DEFAULT)
            error_at(t.line, t.start_col, "'%s' solo puede aparecer dentro de un 'switch'", t.lexeme);
        if (t.type == TOK_CATCH)
            error_at(t.line, t.start_col, "'catch' solo puede aparecer después de un 'try'");
        if (t.type == TOK_THEN)
            error_at(t.line, t.start_col, "'then' necesita una condición anterior y debe cerrar una estructura como 'if', 'while' o 'for'");
        if (t.type == TOK_RPAREN)
            error_at(t.line, t.start_col, "')' no tiene un '(' abierto que cerrar aquí");
        if (t.type == TOK_RBRACKET)
            error_at(t.line, t.start_col, "']' no tiene un '[' abierto que cerrar aquí");
        if (t.type == TOK_RBRACE)
            error_at(t.line, t.start_col, "'}' no tiene un '{' abierto que cerrar aquí");
        if (t.type == TOK_COMMA)
            error_at(t.line, t.start_col, "Hay una ',' fuera de una lista, mapa, llamada o declaración múltiple");
        if (t.type == TOK_SEMI)
            error_at(t.line, t.start_col, "';' no separa instrucciones en Infernal; usa un salto de línea");

        error_at(t.line, t.start_col > 0 ? t.start_col : 1,
                 "Sentencia no reconocida '%s'", t.lexeme);

        if (stmt) {
            nodelist_add(&block, stmt);
            DEBUG_INFO("parse_block: añadida sentencia tipo %d en línea %d", stmt->kind, stmt->line);
        }
        ts_skip_newlines();
        if (terminator && ts_peek().type == lookup_keyword(terminator)) break;
        if (terminator && strcmp(terminator, "}") == 0 && ts_peek().type == TOK_RBRACE) break;
    }

    DEBUG_INFO("=== parse_block: bloque finalizado, %d sentencias ===", block.count);

    /* Garantizar que el NodeList siempre sea válido incluso si no hay sentencias */
    if (block.count == 0 && block.stmts == NULL) {
        // Ya está vacío y válido
    } else if (block.count > 0 && block.stmts == NULL) {
        // Inconsistencia: corregir
        block.stmts = NULL;
        block.count = 0;
        block.cap = 0;
        DEBUG_WARN("parse_block: inconsistencia detectada (count>0 pero stmts=NULL), reiniciando");
    }

    return block;
}
