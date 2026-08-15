/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/helpers.c
*/

#include "helpers.h"
#include "evaluator.h"
#include "core/value.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>

bool val_is_truthy(Value v) {
    switch (v.type) {
        case VAL_BOOL:   return v.data.bval;
        case VAL_INT:    return v.data.ival != 0;
        case VAL_FLOAT:  return v.data.fval != 0.0;
        case VAL_STRING: return v.data.sval && strlen(v.data.sval) > 0;
        case VAL_LIST:   return v.data.list.count > 0;
        case VAL_MAP:    return v.data.map && v.data.map->count > 0;
        default:         return false;
    }
}

const char *type_name(int tok_type) {
    switch (tok_type) {
        case TOK_INT:    return "int";
        case TOK_FLOAT:  return "float";
        case TOK_BOOL:   return "bool";
        case TOK_STRING: return "string";
        case TOK_LIST:   return "list";
        case TOK_MAP:    return "map";
        default:         return "desconocido";
    }
}

int get_node_line(ASTNode *node) {
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
        case NODE_MAP:
            for (int i = 0; i < node->data.map.pair_count; i++) {
                int l = get_node_line(node->data.map.pairs[i].key);
                if (l) return l;
                l = get_node_line(node->data.map.pairs[i].value);
                if (l) return l;
            }
            break;
        case NODE_UNARY:
            if (node->data.unary.operand) {
                int l = get_node_line(node->data.unary.operand);
                if (l) return l;
            }
            break;
        default:
            break;
    }
    return 0;
}

Value resolve_reference(Value v, int line) {
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

int extract_integer_index(ASTNode *node, int line) {
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
