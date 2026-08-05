/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License.
 * Código fuente de Infernal: runtime/evaluator.c
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include "evaluator.h"
#include "core/value.h"
#include "core/ast.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/command.h"
#include "runtime/error.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "developer/debug.h"

/* ─── Funciones auxiliares ────────────────────────────────────── */
static bool val_is_truthy(Value v) {
    switch (v.type) {
        case VAL_BOOL:   return v.data.bval;
        case VAL_INT:    return v.data.ival != 0;
        case VAL_FLOAT:  return v.data.fval != 0.0;
        case VAL_STRING: return v.data.sval != NULL && strlen(v.data.sval) > 0;
        case VAL_LIST:   return v.data.list.count > 0;
        default:         return false;
    }
}

static const char *type_name(int tok_type) {
    switch (tok_type) {
        case TOK_INT:    return "int";
        case TOK_FLOAT:  return "float";
        case TOK_BOOL:   return "bool";
        case TOK_STRING: return "string";
        case TOK_LIST:   return "list";
        default:         return "desconocido";
    }
}

/* ─── Conversión de tipos (pública) ──────────────────────────── */
bool try_convert_value(Value *val, int target_tok_type) {
    if (val->type == VAL_STRING) {
        const char *s = val->data.sval;
        if (target_tok_type == TOK_INT) {
            char *end;
            long n = strtol(s, &end, 10);
            if (*end == '\0' && end != s) {
                free(val->data.sval);
                val->type = VAL_INT;
                val->data.ival = (int)n;
                return true;
            }
            return false;
        }
        if (target_tok_type == TOK_FLOAT) {
            char *normalized = strdup(s);
            for (char *p = normalized; *p; p++) if (*p == ',') *p = '.';
            char *end;
            double f = strtod(normalized, &end);
            if (*end == '\0' && end != normalized) {
                free(val->data.sval);
                val->type = VAL_FLOAT;
                val->data.fval = f;
                free(normalized);
                return true;
            }
            free(normalized);
            return false;
        }
        if (target_tok_type == TOK_BOOL) {
            if (strcasecmp(s, "true") == 0 || strcmp(s, "1") == 0) {
                free(val->data.sval);
                val->type = VAL_BOOL;
                val->data.bval = true;
                return true;
            }
            if (strcasecmp(s, "false") == 0 || strcmp(s, "0") == 0) {
                free(val->data.sval);
                val->type = VAL_BOOL;
                val->data.bval = false;
                return true;
            }
            return false;
        }
        return false;
    }
    if (val->type == VAL_LIST && target_tok_type == TOK_STRING) {
        size_t total_len = 0;
        for (int i = 0; i < val->data.list.count; i++) {
            Value item = val->data.list.items[i];
            char buf[64];
            const char *str;
            switch (item.type) {
                case VAL_INT:    snprintf(buf, sizeof(buf), "%d", item.data.ival); str = buf; break;
                case VAL_FLOAT:  snprintf(buf, sizeof(buf), "%g", item.data.fval); str = buf; break;
                case VAL_BOOL:   str = item.data.bval ? "true" : "false"; break;
                case VAL_STRING: str = item.data.sval ? item.data.sval : ""; break;
                default:         str = "null";
            }
            total_len += strlen(str);
            if (i > 0) total_len++; // espacio
        }
        char *out = malloc(total_len + 1);
        if (!out) return false;
        char *p = out;
        for (int i = 0; i < val->data.list.count; i++) {
            Value item = val->data.list.items[i];
            char buf[64];
            const char *str;
            switch (item.type) {
                case VAL_INT:    snprintf(buf, sizeof(buf), "%d", item.data.ival); str = buf; break;
                case VAL_FLOAT:  snprintf(buf, sizeof(buf), "%g", item.data.fval); str = buf; break;
                case VAL_BOOL:   str = item.data.bval ? "true" : "false"; break;
                case VAL_STRING: str = item.data.sval ? item.data.sval : ""; break;
                default:         str = "null";
            }
            if (i > 0) *p++ = ' ';
            size_t len = strlen(str);
            memcpy(p, str, len);
            p += len;
        }
        *p = '\0';
        for (int i = 0; i < val->data.list.count; i++) {
            if (val->data.list.items[i].type == VAL_STRING)
                free(val->data.list.items[i].data.sval);
        }
        free(val->data.list.items);
        val->type = VAL_STRING;
        val->data.sval = out;
        return true;
    }
    if (val->type == VAL_INT && target_tok_type == TOK_FLOAT) {
        val->type = VAL_FLOAT;
        val->data.fval = (double)val->data.ival;
        return true;
    }
    if (val->type == VAL_FLOAT && target_tok_type == TOK_INT) {
        val->type = VAL_INT;
        val->data.ival = (int)val->data.fval;
        return true;
    }
    return false;
}

void exec_flag_spec(FlagSpec *spec) {
    if (spec->body_count == 0) return;   /* <-- NUEVO: si no hay cuerpo, no hacer nada */
        TokenStream saved_ts = ts;
    ts.tokens = spec->body_tokens;
    ts.count = spec->body_count;
    ts.pos = 0;
    NodeList flag_body = parse_block(NULL);
    exec_block(&flag_body);
    ts = saved_ts;
}

/* ─── Función auxiliar para obtener la línea de un nodo (recursiva) ─── */
static int get_node_line(ASTNode *node) {
    if (!node) return 0;
    if (node->line != 0) return node->line;

    switch (node->kind) {
        case NODE_INDEX:
            if (node->data.idx.list) {
                int l = get_node_line(node->data.idx.list);
                if (l) return l;
            }
            if (node->data.idx.index) {
                int l = get_node_line(node->data.idx.index);
                if (l) return l;
            }
            break;
        case NODE_SLICE:
            if (node->data.slice.list) {
                int l = get_node_line(node->data.slice.list);
                if (l) return l;
            }
            break;
        case NODE_BINOP:
            if (node->data.binop.left) {
                int l = get_node_line(node->data.binop.left);
                if (l) return l;
            }
            if (node->data.binop.right) {
                int l = get_node_line(node->data.binop.right);
                if (l) return l;
            }
            break;
        case NODE_CALL:
            for (int i = 0; i < node->data.call.argc; i++) {
                int l = get_node_line(node->data.call.args[i]);
                if (l) return l;
            }
            break;
        case NODE_LIST:
            for (int i = 0; i < node->data.list_lit.count; i++) {
                int l = get_node_line(node->data.list_lit.items[i]);
                if (l) return l;
            }
            break;
        default:
            break;
    }
    return 0;
}

/* ────────────────────────────────────────────────────────────── */
/* Slice y eliminación de listas (versión robusta)             */
/* ────────────────────────────────────────────────────────────── */

static Value eval_slice(ASTNode *node) {
    DEBUG_INFO("eval_slice: entrada");
    if (node == NULL) error(0, "eval_slice: nodo es NULL");
    if (node->kind != NODE_SLICE) error(node->line, "eval_slice: nodo no es NODE_SLICE");

    if (node->data.slice.list == NULL) {
        error(current_eval_line, "eval_slice: el campo 'list' del nodo slice es NULL");
    }

    DEBUG_INFO("eval_slice: mode=%d, start=%d, end=%d, list node kind=%d",
               node->data.slice.mode, node->data.slice.start, node->data.slice.end,
               node->data.slice.list->kind);

    Value list = eval_expr(node->data.slice.list);
    DEBUG_INFO("eval_slice: lista evaluada, tipo=%d, count=%d", list.type, list.data.list.count);

    if (list.type != VAL_LIST)
        error(current_eval_line, "El slice solo se puede aplicar a listas (tipo %d)", list.type);

    int len = list.data.list.count;
    if (len == 0) {
        DEBUG_INFO("eval_slice: lista vacía, devolviendo lista vacía");
        return val_list_empty();
    }

    if (list.data.list.items == NULL) {
        error(current_eval_line, "eval_slice: lista corrupta: items es NULL pero count=%d", len);
    }

    int mode = node->data.slice.mode;
    int start = node->data.slice.start;
    int end   = node->data.slice.end;

    Value result = val_list_empty();
    int line = get_node_line(node);
    if (line == 0) line = current_eval_line;

    switch (mode) {
        case 0: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            val_list_append(&result, copy_value_secure(list.data.list.items[start - 1]));
            break;
        }
        case 1: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            int real_end = (end > len) ? len : end;
            if (start > real_end) {
                break;
            }
            for (int i = start - 1; i < real_end; i++) {
                val_list_append(&result, copy_value_secure(list.data.list.items[i]));
            }
            break;
        }
        case 2: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            for (int i = start; i < len; i++) {
                val_list_append(&result, copy_value_secure(list.data.list.items[i]));
            }
            break;
        }
        case 3: {
            if (start != -1) {
                error(line, "Modo *start no implementado correctamente");
            }
            if (end < 1 || end > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            for (int i = 0; i < end - 1; i++) {
                val_list_append(&result, copy_value_secure(list.data.list.items[i]));
            }
            break;
        }
        case 4: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            for (int i = 0; i < len; i++) {
                if (i != start - 1)
                    val_list_append(&result, copy_value_secure(list.data.list.items[i]));
            }
            break;
        }
        case 5: {
            break;
        }
        default:
            error(line, "Modo de slice inválido: %d", mode);
    }

    DEBUG_INFO("eval_slice: resultado lista con %d elementos", result.data.list.count);
    return result;
}

static Value remove_slice(Value list, ASTNode *slice_node) {
    if (slice_node == NULL) error(0, "remove_slice: nodo slice es NULL");
    if (slice_node->kind != NODE_SLICE) error(slice_node->line, "remove_slice: nodo no es NODE_SLICE");

    if (list.type != VAL_LIST)
        error(current_eval_line, "La eliminación solo se puede aplicar a listas");

    int len = list.data.list.count;
    if (len == 0) {
        return val_list_empty();
    }

    if (list.data.list.items == NULL) {
        error(current_eval_line, "remove_slice: lista corrupta: items es NULL pero count=%d", len);
    }

    int mode = slice_node->data.slice.mode;
    int start = slice_node->data.slice.start;
    int end   = slice_node->data.slice.end;
    int line = get_node_line(slice_node);
    if (line == 0) line = current_eval_line;

    Value new_list = val_list_empty();

    switch (mode) {
        case 0: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            for (int i = 0; i < len; i++) {
                if (i == start - 1) continue;
                val_list_append(&new_list, copy_value_secure(list.data.list.items[i]));
            }
            break;
        }
        case 1: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            int real_end = (end > len) ? len : end;
            if (start > real_end) {
                for (int i = 0; i < len; i++)
                    val_list_append(&new_list, copy_value_secure(list.data.list.items[i]));
            } else {
                for (int i = 0; i < len; i++) {
                    if (i >= start - 1 && i <= real_end - 1) continue;
                    val_list_append(&new_list, copy_value_secure(list.data.list.items[i]));
                }
            }
            break;
        }
        case 2: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            for (int i = 0; i < start - 1; i++) {
                val_list_append(&new_list, copy_value_secure(list.data.list.items[i]));
            }
            break;
        }
        case 3: {
            if (start != -1) {
                error(line, "Modo *start no implementado correctamente para eliminación");
            }
            if (end < 1 || end > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            for (int i = end - 1; i < len; i++) {
                val_list_append(&new_list, copy_value_secure(list.data.list.items[i]));
            }
            break;
        }
        case 4: {
            if (start < 1 || start > len)
                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
            val_list_append(&new_list, copy_value_secure(list.data.list.items[start - 1]));
            break;
        }
        case 5: {
            break;
        }
        default:
            error(line, "Modo de slice inválido para eliminación: %d", mode);
    }

    return new_list;
}

static int extract_integer_index(ASTNode *node, int line) {
    if (!node) error(line, "Nodo nulo al extraer índice");

    if (node->kind == NODE_LITERAL && node->data.lit.type == TOK_INT)
        return node->data.lit.ival;

    if (node->kind == NODE_INDEX) {
        Value idx_val = eval_expr(node->data.idx.index);
        if (idx_val.type != VAL_INT)
            error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
        return idx_val.data.ival;
    }

    if (node->kind == NODE_LIST && node->data.list_lit.count == 1) {
        return extract_integer_index(node->data.list_lit.items[0], line);
    }

    Value v = eval_expr(node);
    if (v.type == VAL_INT)
        return v.data.ival;
    if (v.type == VAL_LIST && v.data.list.count == 1) {
        Value item = v.data.list.items[0];
        if (item.type == VAL_INT)
            return item.data.ival;
    }

    error(line, "No se pudo extraer un índice entero para la eliminación");
    return -1;
}

static Value resolve_reference(Value v, int line) {
    if (v.type != VAL_REFERENCE) return v;
    VarEntry *list_var = scope_find(current_scope, v.data.ref.list_name);
    if (!list_var || list_var->value.type != VAL_LIST) {
        error(line, "La referencia apunta a una lista inexistente '%s'", v.data.ref.list_name);
    }
    int idx = v.data.ref.index;
    if (idx < 1 || idx > list_var->value.data.list.count) {
        error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
    }
    return list_var->value.data.list.items[idx - 1];
}

/* ─── Evaluación de expresiones ──────────────────────────────── */
Value eval_expr(ASTNode *expr) {
    DEBUG_INFO("eval_expr: evaluando nodo de tipo %d", expr->kind);

    switch (expr->kind) {
        case NODE_LITERAL: {
            if (expr->data.lit.type == TOK_INT)    return val_int(expr->data.lit.ival);
            if (expr->data.lit.type == TOK_FLOAT)  return val_float(expr->data.lit.fval);
            if (expr->data.lit.type == TOK_BOOL)   return val_bool(expr->data.lit.bval);
            if (expr->data.lit.type == TOK_STRING) return val_string(expr->data.lit.sval);
            return val_make_null();
        }
        case NODE_VAR: {
            const char *name = expr->data.var.name;
            if (name[0] == '$' || name[0] == '?') name++;
            if (*name == '\0')
                error(expr->line, "Nombre de variable vacío");
            VarEntry *e = scope_find(current_scope, name);
            if (!e)
                error(expr->line, "Variable no definida: %s", name);
            return copy_value_secure(e->value);
        }
        case NODE_LIST: {
            Value list = val_list_empty();
            for (int i = 0; i < expr->data.list_lit.count; i++) {
                val_list_append(&list, eval_expr(expr->data.list_lit.items[i]));
            }
            return list;
        }
        case NODE_SLICE: {
            if (expr->data.slice.list == NULL) {
                error(current_eval_line, "NODE_SLICE: campo 'list' es NULL");
            }
            return eval_slice(expr);
        }
        case NODE_INDEX: {
            if (expr->data.idx.index->kind == NODE_SLICE) {
                ASTNode *slice = expr->data.idx.index;
                slice->data.slice.list = expr->data.idx.list;
                return eval_slice(slice);
            }
            Value base = eval_expr(expr->data.idx.list);
            Value idx = eval_expr(expr->data.idx.index);
            Value result = val_make_null();

            int line = get_node_line(expr);
            if (line == 0) line = current_eval_line;
            if (line == 0) line = get_node_line(expr->data.idx.index);
            if (line == 0) line = get_node_line(expr->data.idx.list);

            if (base.type == VAL_LIST) {
                if (idx.type != VAL_INT) {
                    if (idx.type == VAL_FLOAT) {
                        double f = idx.data.fval;
                        int i = (int)f;
                        if ((double)i == f) {
                            if (i < 1 || i > base.data.list.count)
                                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                            if (base.data.list.items == NULL)
                                error(line, "Lista corrupta: items es NULL");
                            result = copy_value_secure(base.data.list.items[i-1]);
                            break;
                        } else {
                            error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                        }
                    } else {
                        error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                    }
                }
                int i = idx.data.ival;
                if (i < 1 || i > base.data.list.count)
                    error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                if (base.data.list.items == NULL)
                    error(line, "Lista corrupta: items es NULL");
                result = copy_value_secure(base.data.list.items[i-1]);
            } else if (base.type == VAL_STRING) {
                if (idx.type != VAL_INT) {
                    if (idx.type == VAL_FLOAT) {
                        double f = idx.data.fval;
                        int i = (int)f;
                        if ((double)i == f) {
                            size_t length = strlen(base.data.sval);
                            if (i < 1 || (size_t)i > length)
                                error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                            char character[2] = {base.data.sval[i-1], '\0'};
                            result = val_string(character);
                            break;
                        } else {
                            error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                        }
                    } else {
                        error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                    }
                }
                int position = idx.data.ival;
                size_t length = strlen(base.data.sval);
                if (position < 1 || (size_t)position > length)
                    error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                char character[2] = {base.data.sval[position - 1], '\0'};
                result = val_string(character);
            } else {
                error(line, "No se puede indexar este tipo de valor");
            }
            return result;
        }
        case NODE_BINOP: {
            const char *op_name = "?";
            switch (expr->data.binop.op) {
                case TOK_PLUS:  op_name = "+"; break;
                case TOK_MINUS: op_name = "-"; break;
                case TOK_STAR:  op_name = "*"; break;
                case TOK_SLASH: op_name = "/"; break;
                case TOK_PERCENT: op_name = "%"; break;
                case TOK_EEQ:   op_name = "=="; break;
                case TOK_NEQ:   op_name = "!="; break;
                case TOK_LT_OP: op_name = "<"; break;
                case TOK_GT_OP: op_name = ">"; break;
                case TOK_LE:    op_name = "<="; break;
                case TOK_GE:    op_name = ">="; break;
                case TOK_AND:   op_name = "&&"; break;
                case TOK_OR:    op_name = "||"; break;
                default:        op_name = "desconocido";
            }
            DEBUG_INFO("NODE_BINOP: operador '%s'", op_name);

            Value left = eval_expr(expr->data.binop.left);
            DEBUG_INFO("Tipo de left (antes de resolver): %d", left.type);
            if (left.type == VAL_REFERENCE) {
                left = resolve_reference(left, expr->line);
                DEBUG_INFO("Resuelta referencia a lista, tipo: %d", left.type);
            }

            if (expr->data.binop.op == TOK_MINUS && left.type == VAL_LIST) {
                DEBUG_OP("=== ELIMINACIÓN DE LISTA DETECTADA ===");
                DEBUG_VAR("lista", left);
                ASTNode *right_node = expr->data.binop.right;
                DEBUG_INFO("Tipo del nodo derecho para eliminación: %d", right_node->kind);

                if (right_node->kind == NODE_SLICE) {
                    Value result = remove_slice(left, right_node);
                    return result;
                }

                int idx = -1;
                if (right_node->kind == NODE_INDEX) {
                    idx = extract_integer_index(right_node, expr->line);
                } else if (right_node->kind == NODE_LIST && right_node->data.list_lit.count == 1) {
                    idx = extract_integer_index(right_node->data.list_lit.items[0], expr->line);
                } else if (right_node->kind == NODE_LITERAL && right_node->data.lit.type == TOK_INT) {
                    idx = right_node->data.lit.ival;
                } else {
                    Value right_val = eval_expr(right_node);
                    if (right_val.type == VAL_INT) idx = right_val.data.ival;
                    else if (right_val.type == VAL_LIST && right_val.data.list.count == 1) {
                        Value item = right_val.data.list.items[0];
                        if (item.type == VAL_INT) idx = item.data.ival;
                    }
                }

                if (idx != -1) {
                    DEBUG_INFO("Índice extraído: %d", idx);
                    Value new_list = val_list_empty();
                    if (left.data.list.items == NULL && left.data.list.count > 0)
                        error(expr->line, "Lista corrupta al eliminar elemento");
                    for (int i = 0; i < left.data.list.count; i++) {
                        if (i == idx - 1) continue;
                        val_list_append(&new_list, copy_value_secure(left.data.list.items[i]));
                    }
                    DEBUG_VAR("lista resultado", new_list);
                    return new_list;
                }

                error(expr->line, "No se puede eliminar de la lista con este tipo de especificación (nodo: %d)", right_node->kind);
            }

            /* ─── INSERCIÓN EN LISTA: lista + elemento[pos] ─── */
            if (left.type == VAL_LIST && expr->data.binop.op == TOK_PLUS &&
                expr->data.binop.right->kind == NODE_INDEX) {
                DEBUG_OP("=== INSERCIÓN EN LISTA DETECTADA ===");
            ASTNode *idx_node = expr->data.binop.right;
            Value base = eval_expr(idx_node->data.idx.list);
            Value index_val = eval_expr(idx_node->data.idx.index);

            int pos = -1;
            if (index_val.type == VAL_INT) {
                pos = index_val.data.ival;
            } else if (index_val.type == VAL_FLOAT) {
                double f = index_val.data.fval;
                if (f == (double)(int)f) {
                    pos = (int)f;
                }
            }
            int len = left.data.list.count;
            if (pos < 1 || pos > len + 1) {
                pos = len + 1;
            }

            DEBUG_INFO("Insertando elemento en posición %d", pos);
            Value new_list = val_list_empty();
            for (int i = 0; i < left.data.list.count; i++) {
                val_list_append(&new_list, copy_value_secure(left.data.list.items[i]));
            }
            if (pos > new_list.data.list.count + 1) {
                pos = new_list.data.list.count + 1;
            }
            val_list_append(&new_list, val_make_null());
            for (int i = new_list.data.list.count - 1; i > pos - 1; i--) {
                new_list.data.list.items[i] = new_list.data.list.items[i - 1];
            }
            new_list.data.list.items[pos - 1] = copy_value_secure(base);
            DEBUG_VAR("nueva lista", new_list);
            DEBUG_INFO("Devolviendo lista (tipo %d)", new_list.type);
            return new_list;
                }

                if (left.type == VAL_LIST) {
                    error(expr->line, "Operación no soportada con lista y operador '%s'", op_name);
                }

                Value right = eval_expr(expr->data.binop.right);
                DEBUG_INFO("Tipo de right: %d", right.type);

                if (expr->data.binop.op == TOK_EEQ || expr->data.binop.op == TOK_NEQ) {
                    bool equal = false;
                    if (left.type == right.type) {
                        switch (left.type) {
                            case VAL_NULL:   equal = true; break;
                            case VAL_BOOL:   equal = (left.data.bval == right.data.bval); break;
                            case VAL_INT:    equal = (left.data.ival == right.data.ival); break;
                            case VAL_FLOAT:  equal = (left.data.fval == right.data.fval); break;
                            case VAL_STRING: equal = (strcmp(left.data.sval, right.data.sval) == 0); break;
                            default: equal = false;
                        }
                    }
                    return val_bool(expr->data.binop.op == TOK_EEQ ? equal : !equal);
                }

                if (expr->data.binop.op == TOK_LT_OP || expr->data.binop.op == TOK_GT_OP ||
                    expr->data.binop.op == TOK_LE || expr->data.binop.op == TOK_GE) {
                    double lv = (left.type == VAL_INT) ? left.data.ival : (left.type == VAL_FLOAT) ? left.data.fval : 0.0;
                double rv = (right.type == VAL_INT) ? right.data.ival : (right.type == VAL_FLOAT) ? right.data.fval : 0.0;
                bool result = false;
                switch (expr->data.binop.op) {
                    case TOK_LT_OP: result = (lv < rv); break;
                    case TOK_GT_OP: result = (lv > rv); break;
                    case TOK_LE:    result = (lv <= rv); break;
                    case TOK_GE:    result = (lv >= rv); break;
                    default: break;
                }
                return val_bool(result);
                    }

                    if (left.type == VAL_STRING || right.type == VAL_STRING) {
                        char lbuf[64], rbuf[64];
                        const char *ls = left.type == VAL_STRING ? left.data.sval : lbuf;
                        const char *rs = right.type == VAL_STRING ? right.data.sval : rbuf;
                        if (left.type != VAL_STRING)
                            snprintf(lbuf, sizeof(lbuf), "%d", left.type == VAL_INT ? left.data.ival :
                            left.type == VAL_FLOAT ? (int)left.data.fval : left.data.bval ? 1 : 0);
                        if (right.type != VAL_STRING)
                            snprintf(rbuf, sizeof(rbuf), "%d", right.type == VAL_INT ? right.data.ival :
                            right.type == VAL_FLOAT ? (int)right.data.fval : right.data.bval ? 1 : 0);
                        size_t total = strlen(ls) + strlen(rs) + 1;
                        char *buf = malloc(total);
                        if (!buf) error(expr->line, "Memoria insuficiente al concatenar cadenas");
                        snprintf(buf, total, "%s%s", ls, rs);
                        Value result = val_string(buf);
                        free(buf);
                        return result;
                    }

                    if (left.type == VAL_INT && right.type == VAL_INT) {
                        int lv = left.data.ival, rv = right.data.ival;
                        Value result;
                        switch (expr->data.binop.op) {
                            case TOK_PLUS:  result = val_int(lv + rv); break;
                            case TOK_MINUS: result = val_int(lv - rv); break;
                            case TOK_STAR:  result = val_int(lv * rv); break;
                            case TOK_SLASH: if (rv == 0) error(expr->line, "División por cero"); result = val_float((double)lv / rv); break;
                            case TOK_PERCENT: if (rv == 0) error(expr->line, "Módulo por cero"); result = val_int(lv % rv); break;
                            default: error(expr->line, "Operador no soportado");
                        }
                        return result;
                    }

                    double lv = (left.type == VAL_INT) ? left.data.ival :
                    (left.type == VAL_FLOAT) ? left.data.fval :
                    (left.type == VAL_BOOL) ? (left.data.bval ? 1.0 : 0.0) : 0.0;
                    double rv = (right.type == VAL_INT) ? right.data.ival :
                    (right.type == VAL_FLOAT) ? right.data.fval :
                    (right.type == VAL_BOOL) ? (right.data.bval ? 1.0 : 0.0) : 0.0;
                    Value result;
                    switch (expr->data.binop.op) {
                        case TOK_PLUS: result = val_float(lv + rv); break;
                        case TOK_MINUS: result = val_float(lv - rv); break;
                        case TOK_STAR: result = val_float(lv * rv); break;
                        case TOK_SLASH: if (rv == 0) error(expr->line, "División por cero"); result = val_float(lv / rv); break;
                        case TOK_PERCENT: if (rv == 0) error(expr->line, "Módulo por cero"); result = val_float((int)lv % (int)rv); break;
                        default: error(expr->line, "Operador no soportado");
                    }
                    return result;
        }
                        case NODE_CALL: {
                            FuncObject *fobj = func_lookup(expr->data.call.name);
                            if (!fobj) error(expr->line, "Función no definida: %s", expr->data.call.name);

                            if (fobj->kind == FUNC_BUILTIN) {
                                Value *args = malloc(sizeof(Value) * expr->data.call.argc);
                                for (int i = 0; i < expr->data.call.argc; i++) args[i] = eval_expr(expr->data.call.args[i]);
                                Value ret = fobj->builtin(expr->data.call.argc, args);
                                free(args);
                                return ret;
                            } else {
                                ASTNode *func = fobj->def;
                                if (expr->data.call.argc != func->data.func.param_count) {
                                    error(expr->line, "La función '%s' espera %d argumento(s), recibió %d",
                                          expr->data.call.name, func->data.func.param_count, expr->data.call.argc);
                                }
                                Scope *new_scope = scope_new(current_scope, expr->data.call.name);
                                Scope *prev_scope = current_scope;
                                current_scope = new_scope;
                                for (int i = 0; i < func->data.func.param_count; i++) {
                                    Value arg = (i < expr->data.call.argc) ? eval_expr(expr->data.call.args[i]) : val_make_null();
                                    scope_define(new_scope, func->data.func.params[i], func->data.func.ptypes[i], arg);
                                }
                                int saved_cf = control_flow;
                                Value saved_ret = return_value;
                                control_flow = CF_NONE;
                                exec_block(&func->data.func.body);
                                Value ret = (control_flow == CF_RETURN) ? return_value : val_make_null();
                                control_flow = saved_cf;
                                return_value = saved_ret;
                                current_scope = prev_scope;
                                return ret;
                            }
                        }
                        default:
                            error(expr->line, "Expresión no implementada (tipo %d)", expr->kind);
    }
    return val_make_null();
}

/* ─── Ejecución de bloques ────────────────────────────────────── */
void exec_block(NodeList *block) {
    exec_block_from(block, 0);
}

void exec_block_from(NodeList *block, int start_index) {
    for (int i = start_index; i < block->count; i++) {
        if (control_flow != CF_NONE) break;

        ASTNode *stmt = block->stmts[i];
        DEBUG_INFO("Ejecutando sentencia tipo %d en línea %d", stmt->kind, stmt->line);

        current_eval_line = stmt->line;

        switch (stmt->kind) {
            case NODE_EXPR_STMT: {
                eval_expr(stmt->data.expr_stmt.expr);
                break;
            }
            case NODE_CMD_STMT: {
                char *expanded = expand_command(stmt->data.cmd_stmt.cmd);
                int ret = execute_embedded(expanded);
                if (ret == -1) { free(expanded); error(stmt->line, "Comando embebido no encontrado: %s", stmt->data.cmd_stmt.cmd); }
                free(expanded);
                break;
            }
            case NODE_SHELL_CMD: {
                char *expanded = expand_command(stmt->data.shell_cmd.cmd);
                int ret = system(expanded);
                if (ret != 0) error(stmt->line, "falló: %s", stmt->data.shell_cmd.cmd);
                free(expanded);
                break;
            }
            case NODE_ASSIGN: {
                DEBUG_INFO("ASIGNACION: nombre='%s', value->kind=%d", stmt->data.assign.name, stmt->data.assign.value->kind);
                Value val;
                if (stmt->data.assign.is_cmd) {
                    char *cmd = stmt->data.assign.cmd_str;
                    int exit_code = 0;

                    if (stmt->data.assign.vtype == TOK_BOOL) {
                        char *expanded_cmd = expand_command(cmd);
                        FILE *fp = popen(expanded_cmd, "r");
                        if (!fp) {
                            error(stmt->line, "Error al ejecutar comando: %s", expanded_cmd);
                        }
                        char buf[1024];
                        while (fgets(buf, sizeof(buf), fp) != NULL) {}
                        int status = pclose(fp);
                        if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
                        else exit_code = -1;
                        free(expanded_cmd);
                        val = val_bool(exit_code == 0);
                    } else {
                        char *expanded_cmd = expand_command(cmd);
                        FILE *fp = NULL;
                        char *temp_path = NULL;
                        int is_embedded = 0;

                        if (expanded_cmd[0] == '!' && expanded_cmd[strlen(expanded_cmd)-1] == '!') {
                            is_embedded = 1;
                            char *trimmed = strdup(expanded_cmd + 1);
                            trimmed[strlen(trimmed)-1] = '\0';
                            fp = popen_embedded_with_path(trimmed, "r", &temp_path);
                            free(trimmed);
                        } else {
                            fp = popen(expanded_cmd, "r");
                        }

                        if (!fp) {
                            if (is_embedded)
                                error(stmt->line, "Comando embebido no encontrado: %s", expanded_cmd);
                            else
                                error(stmt->line, "Error al ejecutar comando: %s", expanded_cmd);
                        }
                        char buf[4096];
                        char *out = strdup("");
                        while (fgets(buf, sizeof(buf), fp)) {
                            out = realloc(out, strlen(out) + strlen(buf) + 1);
                            strcat(out, buf);
                        }
                        int status = pclose(fp);
                        if (status != 0) {
                            error(stmt->line, "Comando falló: %s", expanded_cmd);
                        }

                        if (temp_path) {
                            unlink(temp_path);
                            free(temp_path);
                        }

                        size_t len = strlen(out);
                        if (len > 0 && out[len-1] == '\n') out[len-1] = '\0';

                        if (stmt->data.assign.vtype == TOK_LIST) {
                            Value list = val_list_empty();
                            char *dup = strdup(out);
                            char *saveptr;
                            char *line = strtok_r(dup, "\n", &saveptr);
                            while (line) {
                                val_list_append(&list, val_string(line));
                                line = strtok_r(NULL, "\n", &saveptr);
                            }
                            free(dup);
                            free(out);
                            val = list;
                        } else {
                            val = val_string(out);
                            free(out);
                        }
                        free(expanded_cmd);
                    }
                } else {
                    if (stmt->data.assign.lhs_index) {
                        VarEntry *var = scope_find(current_scope, stmt->data.assign.name);
                        if (!var || var->value.type != VAL_LIST)
                            error(stmt->line, "Se esperaba una lista en '%s'", stmt->data.assign.name);
                        Value idx_val = eval_expr(stmt->data.assign.lhs_index);
                        if (idx_val.type != VAL_INT) error(stmt->line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                        int idx = idx_val.data.ival;
                        if (idx < 1 || idx > var->value.data.list.count) error(stmt->line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
                        Value new_val = eval_expr(stmt->data.assign.value);
                        var->value.data.list.items[idx - 1] = copy_value_secure(new_val);
                        break;
                    }
                    val = eval_expr(stmt->data.assign.value);
                }

                DEBUG_INFO("Valor obtenido para asignación: tipo %d", val.type);
                int vtype = stmt->data.assign.vtype;

                if (vtype == TOK_STRING && val.type == VAL_LIST) {
                    if (!try_convert_value(&val, TOK_STRING)) {
                        error(stmt->line, "No se pudo convertir la lista a string en la asignación a '%s'",
                              stmt->data.assign.name);
                    }
                }

                if (vtype != 0) {
                    int actual_type = valtype_to_tokentype(val.type);
                    if (vtype != actual_type) {
                        if (!(vtype == TOK_STRING && val.type == VAL_STRING)) {
                            if (!try_convert_value(&val, vtype)) {
                                error(stmt->line, "Error de tipado fijo: se esperaba %s pero se obtuvo %s",
                                      type_name(vtype), type_name(actual_type));
                            }
                        }
                    }
                }

                if (stmt->data.assign.is_global) {
                    scope_define(super_global_scope, stmt->data.assign.name, vtype, val);
                } else if (stmt->data.assign.is_local) {
                    scope_define(current_scope, stmt->data.assign.name, vtype, val);
                } else {
                    VarEntry *var = scope_find(global_scope, stmt->data.assign.name);
                    if (var) {
                        scope_assign(global_scope, stmt->data.assign.name, val, stmt->line);
                        if (var->vtype == 0 && vtype != 0) var->vtype = vtype;
                    } else {
                        scope_define(global_scope, stmt->data.assign.name, vtype, val);
                    }
                }
                break;
            }
            case NODE_IF: {
                Value cond = eval_expr(stmt->data.if_stmt.cond);
                bool truthy = val_is_truthy(cond);
                Scope *block_scope = scope_new(current_scope, NULL);
                Scope *old_scope = current_scope;
                current_scope = block_scope;
                if (truthy)
                    exec_block(&stmt->data.if_stmt.then_block);
                else
                    exec_block(&stmt->data.if_stmt.else_block);
                current_scope = old_scope;
                if (control_flow == CF_REPEAT_LINE) return;
                break;
            }
            case NODE_WHILE: {
                int iter_count = 0;
                while (1) {
                    if (iter_count >= max_loop_iterations)
                        error(stmt->line, "Límite de iteraciones (%d) alcanzado en bucle while", max_loop_iterations);
                    iter_count++;
                    Value cond = eval_expr(stmt->data.while_stmt.cond);
                    if (!val_is_truthy(cond)) break;
                    Scope *block_scope = scope_new(current_scope, NULL);
                    Scope *old_scope = current_scope;
                    current_scope = block_scope;
                    exec_block(&stmt->data.while_stmt.body);
                    current_scope = old_scope;
                    if (control_flow == CF_BREAK) { control_flow = CF_NONE; break; }
                    if (control_flow == CF_CONTINUE) { control_flow = CF_NONE; continue; }
                    if (control_flow == CF_REPEAT_LINE) return;
                    if (control_flow == CF_RETURN) return;
                }
                break;
            }
            case NODE_FOR: {
                Value init_val = eval_expr(stmt->data.for_stmt.init);
                Scope *for_scope = scope_new(current_scope, NULL);
                Scope *old_scope = current_scope;
                current_scope = for_scope;
                scope_define(for_scope, stmt->data.for_stmt.var, stmt->data.for_stmt.vtype, init_val);
                int iter_count = 0;
                while (1) {
                    if (iter_count >= max_loop_iterations)
                        error(stmt->line, "Límite de iteraciones (%d) alcanzado en bucle for", max_loop_iterations);
                    iter_count++;
                    Value cond = eval_expr(stmt->data.for_stmt.cond);
                    if (!val_is_truthy(cond)) break;
                    exec_block(&stmt->data.for_stmt.body);
                    if (control_flow == CF_BREAK) { control_flow = CF_NONE; break; }
                    if (control_flow == CF_CONTINUE) { control_flow = CF_NONE; }
                    if (control_flow == CF_REPEAT_LINE) { current_scope = old_scope; return; }
                    if (control_flow == CF_RETURN) { current_scope = old_scope; return; }
                    eval_expr(stmt->data.for_stmt.incr);
                }
                current_scope = old_scope;
                break;
            }
            case NODE_FOR_IN: {
                Value list_val = eval_expr(stmt->data.for_in.list_expr);
                if (list_val.type != VAL_LIST) error(stmt->line, "Se esperaba una lista en for-in");
                for (int i = 0; i < list_val.data.list.count; i++) {
                    Scope *iter_scope = scope_new(current_scope, NULL);
                    Scope *old_scope = current_scope;
                    current_scope = iter_scope;
                    scope_define(iter_scope, stmt->data.for_in.var, 0, copy_value_secure(list_val.data.list.items[i]));
                    exec_block(&stmt->data.for_in.body);
                    current_scope = old_scope;
                    if (control_flow == CF_BREAK) { control_flow = CF_NONE; break; }
                    if (control_flow == CF_CONTINUE) { control_flow = CF_NONE; continue; }
                    if (control_flow == CF_REPEAT_LINE) return;
                    if (control_flow == CF_RETURN) return;
                }
                break;
            }
            case NODE_FUNC_DEF: break;
            case NODE_RETURN:
                return_value = stmt->data.ret.expr ? eval_expr(stmt->data.ret.expr) : val_make_null();
                control_flow = CF_RETURN;
                return;
            case NODE_BREAK:
                control_flow = CF_BREAK;
                return;
            case NODE_CONTINUE:
                control_flow = CF_CONTINUE;
                return;
            case NODE_PORTAL: {
                const char *name = stmt->data.portal.name;
                bool is_local = stmt->data.portal.is_local;
                Scope *target_scope = is_local ? current_scope : global_scope;
                if (portal_find_in_scope(target_scope, name)) {
                    error(stmt->line, "Portal '%s' ya existe en este ámbito", name);
                }
                int next_line = stmt->line + 1;
                for (int j = i + 1; j < block->count; j++) {
                    if (block->stmts[j]->kind != NODE_PORTAL) {
                        next_line = block->stmts[j]->line;
                        break;
                    }
                }
                portal_define(target_scope, name, next_line);
                break;
            }
            case NODE_REPEAT: {
                if (stmt->data.repeat.portal_name) {
                    PortalEntry *p = portal_find(current_scope, stmt->data.repeat.portal_name);
                    if (!p) error(stmt->line, "Portal '%s' no encontrado", stmt->data.repeat.portal_name);
                    repeat_line_target = p->line;
                } else {
                    Value line_val = eval_expr(stmt->data.repeat.line_expr);
                    if (line_val.type != VAL_INT)
                        error(stmt->line, "repeat line requiere un número entero");
                    repeat_line_target = line_val.data.ival;
                }
                control_flow = CF_REPEAT_LINE;
                return;
            }
            case NODE_IMPORT: {
                Scope *old_scope = current_scope; current_scope = global_scope;
                exec_block(&stmt->data.import.module_block);
                current_scope = old_scope;
                if (control_flow == CF_REPEAT_LINE) return;
                break;
            }
            case NODE_TRY: {
                jmp_buf saved_env; memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
                int saved_raised = exception_raised; exception_raised = 0;
                if (!setjmp(exception_env)) {
                    exec_block(&stmt->data.try_stmt.try_block);
                } else {
                    exception_raised = 0;
                    exec_block(&stmt->data.try_stmt.catch_block);
                }
                memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
                exception_raised = saved_raised;
                if (control_flow == CF_REPEAT_LINE) return;
                break;
            }
            /* ─── EXECUTE ────────────────────────────────────────────────── */
            case NODE_EXECUTE: {
                // 1. Evaluar la expresión de la ruta
                Value path_val = eval_expr(stmt->data.execute.path_expr);
                if (path_val.type != VAL_STRING) {
                    error(stmt->line, "La ruta del script debe ser una cadena");
                }
                char *raw_path = path_val.data.sval;
                char *expanded_path = expand_command(raw_path);
                free(raw_path);
                if (!expanded_path) {
                    error(stmt->line, "Error al expandir la ruta del script");
                }

                // 2. Expandir argumentos
                int expanded_argc = stmt->data.execute.argc;
                char **expanded_args = NULL;
                if (expanded_argc > 0) {
                    expanded_args = malloc(expanded_argc * sizeof(char*));
                    for (int i = 0; i < expanded_argc; i++) {
                        expanded_args[i] = expand_command(stmt->data.execute.args[i]);
                        if (!expanded_args[i]) expanded_args[i] = strdup("");
                    }
                }

                // 3. Guardar estado actual
                int saved_argc = script_argc;
                char **saved_argv = script_argv;
                int saved_flags_arg_index = flags_arg_index;

                // 4. Construir nuevo argv (índice 0 = ruta, índice 1.. = argumentos)
                char **new_argv = malloc((expanded_argc + 2) * sizeof(char*));
                new_argv[0] = expanded_path;
                for (int i = 0; i < expanded_argc; i++) {
                    new_argv[i + 1] = expanded_args[i];
                }
                new_argv[expanded_argc + 1] = NULL;

                script_argc = expanded_argc + 1;
                script_argv = new_argv;
                flags_arg_index = 1;   /* <-- NUEVO: reiniciar el índice para el script hijo */

                // 5. Abrir y ejecutar el script hijo
                FILE *fp = fopen(expanded_path, "r");
                if (!fp) {
                    error(stmt->line, "No se pudo abrir el script '%s'", expanded_path);
                }

                TokenStream saved_ts = ts;
                ts_init();
                tokenize_file(fp);
                fclose(fp);

                NodeList script_block = parse_block(NULL);
                ts = saved_ts;

                Scope *child_scope = scope_new(current_scope, NULL);
                Scope *old_scope = current_scope;
                current_scope = child_scope;

                exec_block(&script_block);

                current_scope = old_scope;

                // 6. Restaurar estado original
                script_argc = saved_argc;
                script_argv = saved_argv;
                flags_arg_index = saved_flags_arg_index;   /* <-- NUEVO: restaurar */

                // 7. Liberar memoria
                free(expanded_path);
                for (int i = 0; i < expanded_argc; i++) free(expanded_args[i]);
                free(expanded_args);
                free(new_argv);

                break;
            }
            case NODE_FLAGS: {
                int mode = stmt->data.flags.mode;
                bool *handled = calloc(script_argc, sizeof(bool));
                FlagSpec *catch_all = NULL;
                for (int s = 0; s < stmt->data.flags.spec_count; s++) {
                    if (stmt->data.flags.specs[s].catch_all) {
                        catch_all = &stmt->data.flags.specs[s];
                        break;
                    }
                }

                int total_matched = 0;

                if (mode > 0) {
                    int arg_idx = flags_arg_index;
                    int consumed = 0;
                    for (int s = 0; s < stmt->data.flags.spec_count; s++) {
                        FlagSpec *spec = &stmt->data.flags.specs[s];
                        if (spec->catch_all) continue;
                        if (arg_idx < script_argc) {
                            char *val_str = script_argv[arg_idx];
                            if (spec->vtype && spec->var_name) {
                                char cleaned[512]; int c = 0;
                                if (val_str[0] == '"' || val_str[0] == '\'') {
                                    char quote = val_str[0];
                                    for (int j=1; val_str[j] && val_str[j] != quote; j++) cleaned[c++] = val_str[j];
                                    cleaned[c] = '\0';
                                } else {
                                    strncpy(cleaned, val_str, sizeof(cleaned));
                                    cleaned[sizeof(cleaned)-1] = '\0';
                                }
                                if (spec->vtype == TOK_FLOAT) {
                                    char *coma = strchr(cleaned, ',');
                                    if (coma) *coma = '.';
                                }
                                Value v;
                                switch (spec->vtype) {
                                    case TOK_INT: v = val_int(atoi(cleaned)); break;
                                    case TOK_FLOAT: v = val_float(atof(cleaned)); break;
                                    case TOK_BOOL: v = val_bool(strcmp(cleaned,"0")!=0 && strlen(cleaned)>0); break;
                                    case TOK_STRING: v = val_string(cleaned); break;
                                    default: v = val_string(cleaned);
                                }
                                if (spec->is_global) {
                                    scope_define(super_global_scope, spec->var_name, spec->vtype, v);
                                } else {
                                    scope_define(global_scope, spec->var_name, spec->vtype, v);
                                }
                            }
                            handled[arg_idx] = true;
                            total_matched++;
                            arg_idx++;
                            consumed++;
                        }
                        if (spec->body_count > 0) {
                            exec_flag_spec(spec);
                        }
                    }
                    flags_arg_index = arg_idx;

                    if (catch_all) {
                        for (int a = 2; a < script_argc; a++)
                            if (!handled[a]) {
                                scope_define(current_scope, "_", 0, val_string(script_argv[a]));
                                exec_flag_spec(catch_all);
                                total_matched++;
                            }
                    }
                } else {
                    /* ─── Modo 0 (flags con nombre) ─── */
                    for (int a = 2; a < script_argc; a++) {
                        char *arg = script_argv[a];
                        char *arg_dup = strdup(arg);
                        char *eq_pos = strchr(arg_dup, '=');
                        if (eq_pos) *eq_pos = '\0';
                        bool matched = false;
                        for (int s = 0; s < stmt->data.flags.spec_count; s++) {
                            FlagSpec *spec = &stmt->data.flags.specs[s];
                            if (spec->catch_all) continue;
                            for (int n = 0; n < spec->name_count; n++) {
                                if (strcmp(arg_dup, spec->names[n]) == 0) {
                                    if (spec->vtype && spec->var_name) {
                                        if (!eq_pos && a + 1 >= script_argc)
                                            error(stmt->line, "Falta el valor para el flag '%s'", arg_dup);
                                        char *val_str = eq_pos ? eq_pos + 1 : script_argv[++a];
                                        char cleaned[512]; int c = 0;
                                        if (val_str[0] == '"' || val_str[0] == '\'') {
                                            char quote = val_str[0];
                                            for (int j=1; val_str[j] && val_str[j] != quote; j++) cleaned[c++] = val_str[j];
                                            cleaned[c] = '\0';
                                        } else {
                                            strncpy(cleaned, val_str, sizeof(cleaned));
                                            cleaned[sizeof(cleaned)-1] = '\0';
                                        }
                                        if (spec->vtype == TOK_FLOAT) {
                                            char *coma = strchr(cleaned, ',');
                                            if (coma) *coma = '.';
                                        }
                                        Value v;
                                        switch (spec->vtype) {
                                            case TOK_INT: v = val_int(atoi(cleaned)); break;
                                            case TOK_FLOAT: v = val_float(atof(cleaned)); break;
                                            case TOK_BOOL: v = val_bool(strcmp(cleaned,"0")!=0 && strlen(cleaned)>0); break;
                                            case TOK_STRING: v = val_string(cleaned); break;
                                            default: v = val_string(cleaned);
                                        }
                                        if (spec->is_global) {
                                            scope_define(super_global_scope, spec->var_name, spec->vtype, v);
                                        } else {
                                            scope_define(global_scope, spec->var_name, spec->vtype, v);
                                        }
                                    }
                                    if (spec->body_count > 0) {
                                        exec_flag_spec(spec);
                                    }
                                    handled[a] = true;
                                    matched = true;
                                    total_matched++;
                                    break;
                                }
                            }
                            if (matched) break;
                        }
                        if (!matched && arg_dup[0] == '-' && arg_dup[1] != '-' && strlen(arg_dup) > 2) {
                            for (int c = 1; arg_dup[c]; c++) {
                                char sn[3] = {'-', arg_dup[c], '\0'};
                                bool found = false;
                                for (int s = 0; s < stmt->data.flags.spec_count; s++) {
                                    FlagSpec *spec = &stmt->data.flags.specs[s];
                                    if (spec->catch_all) continue;
                                    for (int n = 0; n < spec->name_count; n++) {
                                        if (strcmp(sn, spec->names[n]) == 0) {
                                            if (spec->body_count > 0) {
                                                exec_flag_spec(spec);
                                            }
                                            found = true;
                                            total_matched++;
                                            break;
                                        }
                                    }
                                    if (found) break;
                                }
                                if (!found && catch_all) {
                                    scope_define(current_scope, "_", 0, val_string(sn));
                                    exec_flag_spec(catch_all);
                                    total_matched++;
                                }
                            }
                            handled[a] = true;
                        } else if (!matched && catch_all) {
                            scope_define(current_scope, "_", 0, val_string(arg));
                            exec_flag_spec(catch_all);
                            handled[a] = true;
                            total_matched++;
                        }
                        free(arg_dup);
                    }
                }
                if (total_matched == 0 && catch_all != NULL) exec_flag_spec(catch_all);
                free(handled);
                break;
            }
            default: error(stmt->line, "Sentencia no implementada");
        }

        if (control_flow != CF_NONE) break;
    }
}
