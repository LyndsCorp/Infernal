/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License, Lynds Corp., Aros Legendarios, David Baña Szymaniak.
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
#include "stdlib/embedded.h"
#include "vm/compiler.h"

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

NodeList parse_block(const char *terminator) {
    NodeList block = {NULL, 0, 0};
    while (1) {
        ts_skip_newlines();
        Token t = ts_peek();
        if (t.type == TOK_EOF) break;
        if (terminator && lookup_keyword(terminator) == t.type) break;
        if (terminator && strcmp(terminator, "fi") == 0 &&
            (t.type == TOK_ELSE || t.type == TOK_ELSEIF)) break;
        if (terminator && strcmp(terminator, "}") == 0 && t.type == TOK_RBRACE) break;

        ASTNode *stmt = NULL;

        /***** COMANDOS CON ! *****/
        if (t.type == TOK_BANG) {
            ts_advance();
            char cmd[4096] = {0};
            bool last_was_no_space = false;
            bool last_was_slash = false;
            while (ts_peek().type != TOK_BANG && ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                Token ct = ts_advance();
                bool prepend_space = true;
                if (last_was_no_space || last_was_slash) prepend_space = false;
                if (ct.type == TOK_SLASH || strcmp(ct.lexeme, "/") == 0) prepend_space = false;
                if (ct.type == TOK_IDENT && ct.lexeme[0] == '/') prepend_space = false;
                if (ct.type == TOK_IDENT && ct.lexeme[0] == '$' && cmd[0] != '\0' && cmd[strlen(cmd)-1] == '/')
                    prepend_space = false;
                if (prepend_space && cmd[0] != '\0') strcat(cmd, " ");
                if (ct.type == TOK_STRING_LITERAL) {
                    strcat(cmd, "\"");
                    strcat(cmd, ct.lexeme);
                    strcat(cmd, "\"");
                } else {
                    strcat(cmd, ct.lexeme);
                }
                last_was_no_space = (ct.type == TOK_MINUS || ct.type == TOK_PLUS);
                last_was_slash = (ct.type == TOK_SLASH || strcmp(ct.lexeme, "/") == 0 ||
                (ct.type == TOK_IDENT && ct.lexeme[0] == '/'));
            }
            if (ts_peek().type == TOK_BANG) ts_advance();
            stmt = node_create(NODE_CMD_STMT, t.line);
            stmt->data.cmd_stmt.cmd = strdup(cmd);
            // Soporte para `or` después de comando embebido
            while (ts_match(TOK_OR)) {
                ASTNode *next_stmt = NULL;
                Token next = ts_peek();
                if (next.type == TOK_BANG) {
                    ts_advance();
                    char cmd2[4096] = {0};
                    bool last_was_no_space2 = false;
                    bool last_was_slash2 = false;
                    while (ts_peek().type != TOK_BANG && ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                        Token ct2 = ts_advance();
                        bool prepend_space = true;
                        if (last_was_no_space2 || last_was_slash2) prepend_space = false;
                        if (ct2.type == TOK_SLASH || strcmp(ct2.lexeme, "/") == 0) prepend_space = false;
                        if (ct2.type == TOK_IDENT && ct2.lexeme[0] == '/') prepend_space = false;
                        if (ct2.type == TOK_IDENT && ct2.lexeme[0] == '$' && cmd2[0] != '\0' && cmd2[strlen(cmd2)-1] == '/')
                            prepend_space = false;
                        if (prepend_space && cmd2[0] != '\0') strcat(cmd2, " ");
                        if (ct2.type == TOK_STRING_LITERAL) {
                            strcat(cmd2, "\"");
                            strcat(cmd2, ct2.lexeme);
                            strcat(cmd2, "\"");
                        } else {
                            strcat(cmd2, ct2.lexeme);
                        }
                        last_was_no_space2 = (ct2.type == TOK_MINUS || ct2.type == TOK_PLUS);
                        last_was_slash2 = (ct2.type == TOK_SLASH || strcmp(ct2.lexeme, "/") == 0 ||
                        (ct2.type == TOK_IDENT && ct2.lexeme[0] == '/'));
                    }
                    if (ts_peek().type == TOK_BANG) ts_advance();
                    next_stmt = node_create(NODE_CMD_STMT, next.line);
                    next_stmt->data.cmd_stmt.cmd = strdup(cmd2);
                } else {
                    char cmd2[4096] = {0};
                    Token first2 = ts_advance();
                    strcpy(cmd2, first2.lexeme);
                    bool last_was_no_space2 = false;
                    bool last_was_lbrace2 = (first2.type == TOK_LBRACE);
                    bool last_was_slash2 = false;
                    while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF && ts_peek().type != TOK_OR) {
                        Token ct2 = ts_advance();
                        bool prepend_space = true;
                        if (last_was_lbrace2 || ct2.type == TOK_RBRACE || last_was_no_space2 || last_was_slash2)
                            prepend_space = false;
                        if (ct2.type == TOK_SLASH || strcmp(ct2.lexeme, "/") == 0) prepend_space = false;
                        if (ct2.type == TOK_IDENT && ct2.lexeme[0] == '/') prepend_space = false;
                        if (ct2.type == TOK_IDENT && ct2.lexeme[0] == '$' && cmd2[0] != '\0' && cmd2[strlen(cmd2)-1] == '/')
                            prepend_space = false;
                        if (prepend_space) strcat(cmd2, " ");
                        if (ct2.type == TOK_STRING_LITERAL) {
                            strcat(cmd2, "\"");
                            strcat(cmd2, ct2.lexeme);
                            strcat(cmd2, "\"");
                        } else {
                            strcat(cmd2, ct2.lexeme);
                        }
                        last_was_lbrace2 = (ct2.type == TOK_LBRACE);
                        last_was_no_space2 = (ct2.type == TOK_MINUS || ct2.type == TOK_PLUS);
                        last_was_slash2 = (ct2.type == TOK_SLASH || strcmp(ct2.lexeme, "/") == 0 ||
                        (ct2.type == TOK_IDENT && ct2.lexeme[0] == '/'));
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
            ts_skip_newlines();
            continue;
        }

        /***** COMANDOS QUE EMPIEZAN CON CADENA LITERAL *****/
        if (t.type == TOK_STRING_LITERAL) {
            char cmd[4096] = {0};
            Token first = ts_advance();
            strcat(cmd, "\"");
            strcat(cmd, first.lexeme);
            strcat(cmd, "\"");
            bool last_was_no_space = false;
            bool last_was_slash = false;
            while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                Token ct = ts_advance();
                bool prepend_space = true;
                if (last_was_no_space || last_was_slash) prepend_space = false;
                if (ct.type == TOK_SLASH || strcmp(ct.lexeme, "/") == 0) prepend_space = false;
                if (ct.type == TOK_IDENT && ct.lexeme[0] == '/') prepend_space = false;
                if (ct.type == TOK_IDENT && ct.lexeme[0] == '$' && cmd[0] != '\0' && cmd[strlen(cmd)-1] == '/')
                    prepend_space = false;
                if (prepend_space) strcat(cmd, " ");
                if (ct.type == TOK_STRING_LITERAL) {
                    strcat(cmd, "\"");
                    strcat(cmd, ct.lexeme);
                    strcat(cmd, "\"");
                } else {
                    strcat(cmd, ct.lexeme);
                }
                last_was_no_space = (ct.type == TOK_MINUS || ct.type == TOK_PLUS);
                last_was_slash = (ct.type == TOK_SLASH || strcmp(ct.lexeme, "/") == 0 ||
                (ct.type == TOK_IDENT && ct.lexeme[0] == '/'));
            }
            stmt = node_create(NODE_SHELL_CMD, t.line);
            stmt->data.shell_cmd.cmd = strdup(cmd);
            // Soporte para `or` después de este comando
            while (ts_match(TOK_OR)) {
                ASTNode *next_stmt = NULL;
                Token next = ts_peek();
                if (next.type == TOK_BANG) {
                    ts_advance();
                    char cmd2[4096] = {0};
                    bool last_was_no_space2 = false;
                    bool last_was_slash2 = false;
                    while (ts_peek().type != TOK_BANG && ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                        Token ct2 = ts_advance();
                        bool prepend_space = true;
                        if (last_was_no_space2 || last_was_slash2) prepend_space = false;
                        if (ct2.type == TOK_SLASH || strcmp(ct2.lexeme, "/") == 0) prepend_space = false;
                        if (ct2.type == TOK_IDENT && ct2.lexeme[0] == '/') prepend_space = false;
                        if (ct2.type == TOK_IDENT && ct2.lexeme[0] == '$' && cmd2[0] != '\0' && cmd2[strlen(cmd2)-1] == '/')
                            prepend_space = false;
                        if (prepend_space && cmd2[0] != '\0') strcat(cmd2, " ");
                        if (ct2.type == TOK_STRING_LITERAL) {
                            strcat(cmd2, "\"");
                            strcat(cmd2, ct2.lexeme);
                            strcat(cmd2, "\"");
                        } else {
                            strcat(cmd2, ct2.lexeme);
                        }
                        last_was_no_space2 = (ct2.type == TOK_MINUS || ct2.type == TOK_PLUS);
                        last_was_slash2 = (ct2.type == TOK_SLASH || strcmp(ct2.lexeme, "/") == 0 ||
                        (ct2.type == TOK_IDENT && ct2.lexeme[0] == '/'));
                    }
                    if (ts_peek().type == TOK_BANG) ts_advance();
                    next_stmt = node_create(NODE_CMD_STMT, next.line);
                    next_stmt->data.cmd_stmt.cmd = strdup(cmd2);
                } else {
                    char cmd2[4096] = {0};
                    Token first2 = ts_advance();
                    strcpy(cmd2, first2.lexeme);
                    bool last_was_no_space2 = false;
                    bool last_was_lbrace2 = (first2.type == TOK_LBRACE);
                    bool last_was_slash2 = false;
                    while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF && ts_peek().type != TOK_OR) {
                        Token ct2 = ts_advance();
                        bool prepend_space = true;
                        if (last_was_lbrace2 || ct2.type == TOK_RBRACE || last_was_no_space2 || last_was_slash2)
                            prepend_space = false;
                        if (ct2.type == TOK_SLASH || strcmp(ct2.lexeme, "/") == 0) prepend_space = false;
                        if (ct2.type == TOK_IDENT && ct2.lexeme[0] == '/') prepend_space = false;
                        if (ct2.type == TOK_IDENT && ct2.lexeme[0] == '$' && cmd2[0] != '\0' && cmd2[strlen(cmd2)-1] == '/')
                            prepend_space = false;
                        if (prepend_space) strcat(cmd2, " ");
                        if (ct2.type == TOK_STRING_LITERAL) {
                            strcat(cmd2, "\"");
                            strcat(cmd2, ct2.lexeme);
                            strcat(cmd2, "\"");
                        } else {
                            strcat(cmd2, ct2.lexeme);
                        }
                        last_was_lbrace2 = (ct2.type == TOK_LBRACE);
                        last_was_no_space2 = (ct2.type == TOK_MINUS || ct2.type == TOK_PLUS);
                        last_was_slash2 = (ct2.type == TOK_SLASH || strcmp(ct2.lexeme, "/") == 0 ||
                        (ct2.type == TOK_IDENT && ct2.lexeme[0] == '/'));
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
            goto stmt_done;
        }

        /***** PORTALES *****/
        if (t.type == TOK_AT) {
            ts_advance();
            if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba nombre de portal después de '@'");
            char *portal_name = strdup(ts_advance().lexeme);
            stmt = node_create(NODE_PORTAL, t.line);
            stmt->data.portal.name = portal_name;
            stmt->data.portal.is_local = false;
            nodelist_add(&block, stmt);
            ts_skip_newlines();
            continue;
        }

        /***** FLAGS *****/
        if (t.type == TOK_FLAG) {
            ts_advance();
            stmt = parse_flags();
            nodelist_add(&block, stmt);
            ts_skip_newlines();
            continue;
        }

        /***** ESTRUCTURAS DE CONTROL *****/
        if (t.type == TOK_IF) {
            stmt = parse_if_statement();
        } else if (t.type == TOK_WHILE) {
            ts_advance();
            ASTNode *cond = parse_expression(0);
            if (!ts_match(TOK_THEN)) error_missing_then(t.line, "while");
            NodeList body = parse_block("fi");
            if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi'");
            stmt = node_create(NODE_WHILE, t.line);
            stmt->data.while_stmt.cond = cond;
            stmt->data.while_stmt.body = body;
        } else if (t.type == TOK_FOR) {
            ts_advance();
            if (ts_peek().type == TOK_IDENT && ts.pos+1 < ts.count && ts.tokens[ts.pos+1].type == TOK_IN) {
                char *varname = strdup(ts_advance().lexeme);
                validate_var_name(varname, t.line);
                ts_advance();
                ASTNode *list_expr = parse_expression(0);
                if (!ts_match(TOK_THEN)) error_missing_then(t.line, "for-in");
                NodeList body = parse_block("fi");
                if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi'");
                stmt = node_create(NODE_FOR_IN, t.line);
                stmt->data.for_in.var = varname;
                stmt->data.for_in.list_expr = list_expr;
                stmt->data.for_in.body = body;
            } else {
                int vtype = 0;
                if (ts_peek().type == TOK_INT || ts_peek().type == TOK_FLOAT ||
                    ts_peek().type == TOK_BOOL || ts_peek().type == TOK_STRING || ts_peek().type == TOK_LIST)
                    vtype = ts_advance().type;
                if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba variable en for");
                char *varname = strdup(ts_advance().lexeme);
                validate_var_name(varname, t.line);
                if (!ts_match(TOK_EQ)) error(t.line, "Se esperaba '=' en for");
                ASTNode *init = parse_expression(0);
                if (!ts_match(TOK_SEMI)) error(t.line, "Se esperaba ';' después de inicialización");
                ASTNode *cond = parse_expression(0);
                if (!ts_match(TOK_SEMI)) error(t.line, "Se esperaba ';' después de condición");
                ASTNode *incr = parse_expression(0);
                if (!ts_match(TOK_THEN)) error_missing_then(t.line, "for");
                NodeList body = parse_block("fi");
                if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi'");
                stmt = node_create(NODE_FOR, t.line);
                stmt->data.for_stmt.var = varname;
                stmt->data.for_stmt.vtype = vtype;
                stmt->data.for_stmt.init = init;
                stmt->data.for_stmt.cond = cond;
                stmt->data.for_stmt.incr = incr;
                stmt->data.for_stmt.body = body;
            }
        } else if (t.type == TOK_FUNCTION) {
            ts_advance();
            if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba nombre de función");
            char *fname = strdup(ts_advance().lexeme);
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
        } else if (t.type == TOK_RETURN) {
            ts_advance();
            ASTNode *expr = NULL;
            if (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_FI && ts_peek().type != TOK_EOF)
                expr = parse_expression(0);
            stmt = node_create(NODE_RETURN, t.line);
            stmt->data.ret.expr = expr;
        } else if (t.type == TOK_BREAK) {
            ts_advance(); stmt = node_create(NODE_BREAK, t.line);
        } else if (t.type == TOK_CONTINUE) {
            ts_advance(); stmt = node_create(NODE_CONTINUE, t.line);
        } else if (t.type == TOK_REPEAT) {
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
        } else if (t.type == TOK_IMPORT) {
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
                            error(t.line, "No se pudo abrir módulo '%s' (buscado en ~/.infernal/fire/ y /usr/share/infernal/fire/)", module_name);
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
        } else if (t.type == TOK_TRY) {
            ts_advance();
            NodeList try_block = parse_block("catch");
            if (!ts_match(TOK_CATCH)) error(t.line, "Se esperaba 'catch'");
            NodeList catch_block = parse_block("fi");
            if (!ts_match(TOK_FI)) error(t.line, "Se esperaba 'fi'");
            stmt = node_create(NODE_TRY, t.line);
            stmt->data.try_stmt.try_block = try_block;
            stmt->data.try_stmt.catch_block = catch_block;
        } else if (t.type == TOK_LOCAL || t.type == TOK_GLOBAL) {
            bool is_local = ts_match(TOK_LOCAL);
            bool is_global = ts_match(TOK_GLOBAL);

            if (ts_peek().type == TOK_AT) {
                ts_advance();
                if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba nombre de portal después de '@'");
                char *portal_name = strdup(ts_advance().lexeme);
                stmt = node_create(NODE_PORTAL, t.line);
                stmt->data.portal.name = portal_name;
                stmt->data.portal.is_local = is_local;
                nodelist_add(&block, stmt);
                ts_skip_newlines();
                continue;
            }

            int vtype = 0;
            if (!is_local && !is_global) {
                vtype = ts_advance().type;
            } else {
                if (ts_peek().type == TOK_INT || ts_peek().type == TOK_FLOAT ||
                    ts_peek().type == TOK_BOOL || ts_peek().type == TOK_STRING || ts_peek().type == TOK_LIST)
                    vtype = ts_advance().type;
            }
            if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba nombre de variable");
            char *vname = strdup(ts_advance().lexeme);
            validate_var_name(vname, t.line);

            ASTNode *lhs_index = NULL;
            if (ts_match(TOK_LBRACKET)) {
                lhs_index = parse_expression(0);
                if (!ts_match(TOK_RBRACKET))
                    error(t.line, "Se esperaba ']' tras índice");
            }

            if (!ts_match(TOK_EQ)) error(t.line, "Se esperaba '='");

            ASTNode *value = NULL;
            bool is_cmd = false;
            char *cmd_str = NULL;

            // ----- NUEVA LÓGICA PARA RHS -----
            if (ts_peek().type == TOK_IDENT && ts_peek().lexeme[0] == '$') {
                value = parse_expression(0);
            } else if (ts_peek().type == TOK_IDENT) {
                // Verificar si el siguiente token es '(' para detectar llamada a función
                int next_pos = ts.pos + 1;
                if (next_pos < ts.count && ts.tokens[next_pos].type == TOK_LPAREN) {
                    value = parse_expression(0);
                } else {
                    is_cmd = true;
                    cmd_str = extract_command_string(t.line);
                    while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) ts_advance();
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

        } else if (t.type == TOK_INT || t.type == TOK_FLOAT || t.type == TOK_BOOL ||
            t.type == TOK_STRING || t.type == TOK_LIST) {
            int vtype = ts_advance().type;
        if (ts_peek().type != TOK_IDENT) error(t.line, "Se esperaba nombre de variable");
        char *vname = strdup(ts_advance().lexeme);
            validate_var_name(vname, t.line);

            ASTNode *lhs_index = NULL;
            if (ts_match(TOK_LBRACKET)) {
                lhs_index = parse_expression(0);
                if (!ts_match(TOK_RBRACKET))
                    error(t.line, "Se esperaba ']' tras índice");
            }

            if (!ts_match(TOK_EQ)) error(t.line, "Se esperaba '='");

            ASTNode *value = NULL;
            bool is_cmd = false;
            char *cmd_str = NULL;

            // ----- NUEVA LÓGICA PARA RHS -----
            if (ts_peek().type == TOK_IDENT && ts_peek().lexeme[0] == '$') {
                value = parse_expression(0);
            } else if (ts_peek().type == TOK_IDENT) {
                // Verificar si el siguiente token es '(' para detectar llamada a función
                int next_pos = ts.pos + 1;
                if (next_pos < ts.count && ts.tokens[next_pos].type == TOK_LPAREN) {
                    value = parse_expression(0);
                } else {
                    is_cmd = true;
                    cmd_str = extract_command_string(t.line);
                    while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) ts_advance();
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

            } else if (t.type == TOK_IDENT) {
                Token saved_t = t;
                ts_advance();

                if (ts_match(TOK_EQ) || ts_match(TOK_LBRACKET)) {
                    char *vname = strdup(saved_t.lexeme);
                    validate_var_name(vname, saved_t.line);
                    ASTNode *lhs_index = NULL;
                    if (ts.tokens[ts.pos-1].type == TOK_LBRACKET) {
                        lhs_index = parse_expression(0);
                        if (!ts_match(TOK_RBRACKET))
                            error(t.line, "Se esperaba ']' tras índice");
                        if (!ts_match(TOK_EQ)) error(t.line, "Se esperaba '='");
                    }

                    ASTNode *value = NULL;
                    bool is_cmd = false;
                    char *cmd_str = NULL;

                    // ----- NUEVA LÓGICA PARA RHS -----
                    if (ts_peek().type == TOK_IDENT && ts_peek().lexeme[0] == '$') {
                        value = parse_expression(0);
                    } else if (ts_peek().type == TOK_IDENT) {
                        // Verificar si el siguiente token es '(' para detectar llamada a función
                        int next_pos = ts.pos + 1;
                        if (next_pos < ts.count && ts.tokens[next_pos].type == TOK_LPAREN) {
                            value = parse_expression(0);
                        } else {
                            is_cmd = true;
                            cmd_str = extract_command_string(saved_t.line);
                            while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) ts_advance();
                        }
                    } else {
                        value = parse_expression(0);
                    }

                    stmt = node_create(NODE_ASSIGN, saved_t.line);
                    stmt->data.assign.name = vname;
                    stmt->data.assign.is_cmd = is_cmd;
                    stmt->data.assign.cmd_str = cmd_str;
                    stmt->data.assign.value = value;
                    stmt->data.assign.vtype = 0;
                    stmt->data.assign.is_local = false;
                    stmt->data.assign.is_global = false;
                    stmt->data.assign.lhs_index = lhs_index;
                } else {
                    ts.pos--;

                    int known = (scope_find_script(current_scope, saved_t.lexeme) != NULL ||
                    func_lookup(saved_t.lexeme) != NULL);
                    int next_is_paren_or_bracket = 0;
                    if (ts.pos + 1 < ts.count) {
                        TokenType next = ts.tokens[ts.pos + 1].type;
                        if (next == TOK_LPAREN || next == TOK_LBRACKET)
                            next_is_paren_or_bracket = 1;
                    }

                    if (known || next_is_paren_or_bracket) {
                        int saved = ts.pos;
                        jmp_buf saved_env; memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
                        int saved_raised = exception_raised;
                        exception_raised = 0;
                        if (!setjmp(exception_env)) {
                            ASTNode *expr = parse_expression(0);
                            if (ts_peek().type == TOK_NEWLINE || ts_peek().type == TOK_EOF ||
                                ts_peek().type == TOK_FI || ts_peek().type == TOK_RBRACE) {
                                stmt = node_create(NODE_EXPR_STMT, t.line);
                            stmt->data.expr_stmt.expr = expr;
                            memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
                            exception_raised = saved_raised;
                            goto stmt_done;
                                }
                        }
                        ts.pos = saved;
                        exception_raised = 0;
                    }

                    /***** COMANDO SHELL NORMAL (sin ! ni comillas) *****/
                    char cmd[4096] = {0};
                    Token first = ts_advance();
                    strcpy(cmd, first.lexeme);
                    bool last_was_no_space = false;
                    bool last_was_lbrace = (first.type == TOK_LBRACE);
                    bool last_was_slash = false;
                    while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                        Token ct = ts_advance();
                        bool prepend_space = true;
                        if (last_was_lbrace || ct.type == TOK_RBRACE || last_was_no_space || last_was_slash)
                            prepend_space = false;
                        if (ct.type == TOK_SLASH || strcmp(ct.lexeme, "/") == 0) prepend_space = false;
                        if (ct.type == TOK_IDENT && ct.lexeme[0] == '/') prepend_space = false;
                        if (ct.type == TOK_IDENT && ct.lexeme[0] == '$' && cmd[0] != '\0' && cmd[strlen(cmd)-1] == '/')
                            prepend_space = false;
                        if (prepend_space) strcat(cmd, " ");
                        if (ct.type == TOK_STRING_LITERAL) {
                            strcat(cmd, "\"");
                            strcat(cmd, ct.lexeme);
                            strcat(cmd, "\"");
                        } else {
                            strcat(cmd, ct.lexeme);
                        }
                        last_was_lbrace = (ct.type == TOK_LBRACE);
                        last_was_no_space = (ct.type == TOK_MINUS || ct.type == TOK_PLUS);
                        last_was_slash = (ct.type == TOK_SLASH || strcmp(ct.lexeme, "/") == 0 ||
                        (ct.type == TOK_IDENT && ct.lexeme[0] == '/'));
                    }
                    stmt = node_create(NODE_SHELL_CMD, t.line);
                    stmt->data.shell_cmd.cmd = strdup(cmd);
                    // Soporte para `or` después de comando shell
                    while (ts_match(TOK_OR)) {
                        ASTNode *next_stmt = NULL;
                        Token next = ts_peek();
                        if (next.type == TOK_BANG) {
                            ts_advance();
                            char cmd2[4096] = {0};
                            bool last_was_no_space2 = false;
                            bool last_was_slash2 = false;
                            while (ts_peek().type != TOK_BANG && ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF) {
                                Token ct2 = ts_advance();
                                bool prepend_space = true;
                                if (last_was_no_space2 || last_was_slash2) prepend_space = false;
                                if (ct2.type == TOK_SLASH || strcmp(ct2.lexeme, "/") == 0) prepend_space = false;
                                if (ct2.type == TOK_IDENT && ct2.lexeme[0] == '/') prepend_space = false;
                                if (ct2.type == TOK_IDENT && ct2.lexeme[0] == '$' && cmd2[0] != '\0' && cmd2[strlen(cmd2)-1] == '/')
                                    prepend_space = false;
                                if (prepend_space && cmd2[0] != '\0') strcat(cmd2, " ");
                                if (ct2.type == TOK_STRING_LITERAL) {
                                    strcat(cmd2, "\"");
                                    strcat(cmd2, ct2.lexeme);
                                    strcat(cmd2, "\"");
                                } else {
                                    strcat(cmd2, ct2.lexeme);
                                }
                                last_was_no_space2 = (ct2.type == TOK_MINUS || ct2.type == TOK_PLUS);
                                last_was_slash2 = (ct2.type == TOK_SLASH || strcmp(ct2.lexeme, "/") == 0 ||
                                (ct2.type == TOK_IDENT && ct2.lexeme[0] == '/'));
                            }
                            if (ts_peek().type == TOK_BANG) ts_advance();
                            next_stmt = node_create(NODE_CMD_STMT, next.line);
                            next_stmt->data.cmd_stmt.cmd = strdup(cmd2);
                        } else {
                            char cmd2[4096] = {0};
                            Token first2 = ts_advance();
                            strcpy(cmd2, first2.lexeme);
                            bool last_was_no_space2 = false;
                            bool last_was_lbrace2 = (first2.type == TOK_LBRACE);
                            bool last_was_slash2 = false;
                            while (ts_peek().type != TOK_NEWLINE && ts_peek().type != TOK_EOF && ts_peek().type != TOK_OR) {
                                Token ct2 = ts_advance();
                                bool prepend_space = true;
                                if (last_was_lbrace2 || ct2.type == TOK_RBRACE || last_was_no_space2 || last_was_slash2)
                                    prepend_space = false;
                                if (ct2.type == TOK_SLASH || strcmp(ct2.lexeme, "/") == 0) prepend_space = false;
                                if (ct2.type == TOK_IDENT && ct2.lexeme[0] == '/') prepend_space = false;
                                if (ct2.type == TOK_IDENT && ct2.lexeme[0] == '$' && cmd2[0] != '\0' && cmd2[strlen(cmd2)-1] == '/')
                                    prepend_space = false;
                                if (prepend_space) strcat(cmd2, " ");
                                if (ct2.type == TOK_STRING_LITERAL) {
                                    strcat(cmd2, "\"");
                                    strcat(cmd2, ct2.lexeme);
                                    strcat(cmd2, "\"");
                                } else {
                                    strcat(cmd2, ct2.lexeme);
                                }
                                last_was_lbrace2 = (ct2.type == TOK_LBRACE);
                                last_was_no_space2 = (ct2.type == TOK_MINUS || ct2.type == TOK_PLUS);
                                last_was_slash2 = (ct2.type == TOK_SLASH || strcmp(ct2.lexeme, "/") == 0 ||
                                (ct2.type == TOK_IDENT && ct2.lexeme[0] == '/'));
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
                    goto stmt_done;
                }
            } else {
                error(t.line, "Sentencia no reconocida '%s'", t.lexeme);
            }

            stmt_done:
            if (stmt) nodelist_add(&block, stmt);
            ts_skip_newlines();
        if (terminator && ts_peek().type == lookup_keyword(terminator)) break;
        if (terminator && strcmp(terminator, "}") == 0 && ts_peek().type == TOK_RBRACE) break;
    }
    return block;
}
