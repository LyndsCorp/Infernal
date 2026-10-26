/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/evaluator/eval_stmt.c
 *
 * Implementación de la ejecución de sentencias.
*/

#include "eval_stmt.h"
#include "eval_flag.h"
#include "helpers.h"
#include "evaluator.h"
#include "core/value.h"
#include "core/ast.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/constants.h"
#include "runtime/command.h"
#include "runtime/error.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "developer/debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <limits.h>

#define MAX_COMMAND_OUTPUT (8u * 1024u * 1024u)

Value eval_embedded(ASTNode *expr) {
    Scope *new_scope = scope_new(current_scope, NULL);
    Scope *prev_scope = current_scope;

    jmp_buf saved_env;
    memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
    int saved_raised = exception_raised;
    int saved_cf = control_flow;
    Value saved_ret = return_value;
    bool state_active = false;

    if (setjmp(exception_env) != 0) {
        /* Error propagándose desde dentro del bloque: restauramos el
         * scope anterior, liberamos el nuevo y re-propagamos. Si ya
         * habíamos activado el estado de "función", también restauramos
         * control_flow y return_value. */
        current_scope = prev_scope;
        if (state_active) {
            value_free(&return_value);
            return_value = saved_ret;
            control_flow = saved_cf;
        }
        scope_free(new_scope);
        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
        exception_raised = saved_raised;
        longjmp(exception_env, 1);
    }

    current_scope = new_scope;
    return_value = val_make_null();
    control_flow = CF_NONE;
    state_active = true;

    exec_block_impl(&expr->data.embedded.body);

    if (control_flow != CF_RETURN) {
        /* Ninguna ruta del bloque terminó en 'return'. La expresión no
         * tiene valor ni tipo, así que es un error. Limpiamos todo antes
         * de disparar el error para no dejar nada colgando. */
        value_free(&return_value);
        return_value = saved_ret;
        control_flow = saved_cf;
        current_scope = prev_scope;
        scope_free(new_scope);
        state_active = false;

        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
        exception_raised = saved_raised;
        error(expr->line,
              "El bloque embebido '{ ... }' no alcanzó ningún 'return'.\n"
              "    Un bloque embebido es como una función anónima sin parámetros: su valor\n"
              "    es el del 'return' que se ejecute. Como ninguna ruta del bloque terminó\n"
              "    en 'return', la expresión no tiene ni tipo ni valor.\n"
              "    Asegúrate de que todas las ramas del bloque terminen con 'return'.");
    }

    /* Transferimos la propiedad del valor de retorno a la expresión.
     * Dejamos return_value como estaba antes de entrar al bloque. */
    Value ret = return_value;
    control_flow = saved_cf;
    return_value = saved_ret;
    state_active = false;
    current_scope = prev_scope;
    scope_free(new_scope);
    memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
    exception_raised = saved_raised;
    return ret;
}

/* --- Funciones auxiliares de bloque --- */
static size_t repeat_line_hash(int line) {
    uint32_t hash = (uint32_t)line;
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    hash *= 0x846ca68bu;
    hash ^= hash >> 16;
    return (size_t)hash;
}

static RepeatLineIndex *repeat_line_index_build(NodeList *block) {
    size_t capacity = 16;
    while (capacity < (size_t)block->count * 2) capacity *= 2;

    RepeatLineIndex *index = calloc(1, sizeof(*index));
    if (!index) error(current_eval_line, "Memoria insuficiente al indexar líneas del bloque");
    index->lines = calloc(capacity, sizeof(*index->lines));
    index->indices = malloc(capacity * sizeof(*index->indices));
    if (!index->lines || !index->indices) {
        free(index->lines);
        free(index->indices);
        free(index);
        error(current_eval_line, "Memoria insuficiente al indexar líneas del bloque");
    }
    index->capacity = capacity;
    for (size_t i = 0; i < capacity; i++) index->indices[i] = -1;

    for (int i = 0; i < block->count; i++) {
        ASTNode *stmt = block->stmts[i];
        if (!stmt) continue;
        size_t slot = repeat_line_hash(stmt->line) & (capacity - 1);
        while (index->indices[slot] >= 0 && index->lines[slot] != stmt->line)
            slot = (slot + 1) & (capacity - 1);
        if (index->indices[slot] < 0) {
            index->lines[slot] = stmt->line;
            index->indices[slot] = i;
        }
    }
    return index;
}

static int repeat_line_index_find(RepeatLineIndex *index, int line) {
    size_t slot = repeat_line_hash(line) & (index->capacity - 1);
    while (index->indices[slot] >= 0) {
        if (index->lines[slot] == line) return index->indices[slot];
        slot = (slot + 1) & (index->capacity - 1);
    }
    return -1;
}

void exec_block_impl(NodeList *block) {
    exec_block_from_impl(block, 0);
}

void exec_block_from_impl(NodeList *block, int start_index) {
    int i = start_index;

    while (i < block->count) {
        if (control_flow == CF_REPEAT_LINE) {
            int target_line = repeat_line_target;
            int target_index = -1;
            if (block->count < 16) {
                for (int j = 0; j < block->count; j++) {
                    ASTNode *candidate = block->stmts[j];
                    if (candidate && candidate->line == target_line) {
                        target_index = j;
                        break;
                    }
                }
            } else {
                RepeatLineIndex *index = block->repeat_line_index;
                if (!index) {
                    index = repeat_line_index_build(block);
                    block->repeat_line_index = index;
                }
                target_index = repeat_line_index_find(index, target_line);
            }

            if (target_index < 0) {
                return;
            }

            DEBUG_INFO("exec_block: repeat -> línea %d (índice %d)",
                       target_line, target_index);
            control_flow = CF_NONE;
            i = target_index;
            continue;
        }

        if (control_flow != CF_NONE) break;
        ASTNode *stmt = block->stmts[i];
        current_eval_line = stmt->line;
        DEBUG_INFO("exec_block: ejecutando sentencia tipo %d en línea %d", stmt->kind, stmt->line);
        exec_stmt(stmt);

        if (control_flow == CF_REPEAT_LINE) {
            continue;
        }

        ++i;
    }
}

static bool switch_values_equal(Value left, Value right) {
    if ((left.type == VAL_UINT || left.type == VAL_INT) &&
        (right.type == VAL_UINT || right.type == VAL_INT))
        return bigint_compare(value_as_bigint(left), value_as_bigint(right)) == 0;
    if (left.type != right.type) return false;
    switch (left.type) {
        case VAL_NULL: return true;
        case VAL_BOOL: return left.data.bval == right.data.bval;
        case VAL_INT: case VAL_UINT:
            return bigint_compare(value_as_bigint(left), value_as_bigint(right)) == 0;
        case VAL_FLOAT: return left.data.fval == right.data.fval;
        case VAL_STRING:
            return left.data.sval && right.data.sval && strcmp(left.data.sval, right.data.sval) == 0;
        default: return false;
    }
}

/* Compara dos Value por igualdad de valor. Se usa en for-in para localizar
 * el elemento visitado dentro de la lista viva. Listas y mapas se comparan
 * por identidad de tipo pero no por contenido: no merece la pena recorrer
 * estructuras enteras en cada iteración, y en la práctica los elementos que
 * se eliminan con `-= [i]` son escalares (int, float, bool, string). */
static bool forin_values_equal(Value a, Value b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case VAL_NULL:   return true;
        case VAL_INT: case VAL_UINT:
            return bigint_compare(value_as_bigint(a), value_as_bigint(b)) == 0;
        case VAL_FLOAT:  return a.data.fval == b.data.fval;
        case VAL_BOOL:   return a.data.bval == b.data.bval;
        case VAL_STRING:
            if (!a.data.sval || !b.data.sval) return a.data.sval == b.data.sval;
            return strcmp(a.data.sval, b.data.sval) == 0;
        default:         return false;
    }
}

/* ====================================================================
 *  Helpers UTF-8 para manipulación cruda de strings
 * ====================================================================
 *
 * Infernal cuenta caracteres UTF-8, no bytes. Estas funciones se usan
 * en assign_nested_index cuando el contenedor es un string. Son
 * homólogas a las de eval_binop.c, pero con sufijo _stmt para evitar
 * colisiones de símbolos si algún día se unifican los translation units.
 */

static size_t utf8_char_count_stmt(const char *s) {
    size_t n = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) s += 1;
        else if ((c & 0xE0) == 0xC0) s += 2;
        else if ((c & 0xF0) == 0xE0) s += 3;
        else if ((c & 0xF8) == 0xF0) s += 4;
        else s += 1;
        n++;
    }
    return n;
}

static const char *utf8_advance_stmt(const char *s, size_t n) {
    while (*s && n > 0) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) s += 1;
        else if ((c & 0xE0) == 0xC0) s += 2;
        else if ((c & 0xF0) == 0xE0) s += 3;
        else if ((c & 0xF8) == 0xF0) s += 4;
        else s += 1;
        n--;
    }
    return s;
}

/* Sustituye el carácter en la posición `pos` (1-based) por `new_char`
 * (que debe tener exactamente 1 carácter UTF-8). Devuelve un buffer
 * malloc'd con el resultado. */
static char *utf8_replace_char(const char *s, int pos,
                               const char *new_char, int line) {
    size_t total = utf8_char_count_stmt(s);
    if (pos < 1 || pos > (int)total) {
        error(line, "Índice fuera de rango. No se admiten índices de números negativos ni números decimales.");
    }
    const char *start_ptr = utf8_advance_stmt(s, (size_t)(pos - 1));
    const char *end_ptr   = utf8_advance_stmt(s, (size_t)pos);
    size_t prefix_len = (size_t)(start_ptr - s);
    size_t new_len    = strlen(new_char);
    size_t suffix_len = strlen(end_ptr);

    char *out = malloc(prefix_len + new_len + suffix_len + 1);
    if (!out) error(line, "Memoria insuficiente al reemplazar carácter");
    memcpy(out, s, prefix_len);
    memcpy(out + prefix_len, new_char, new_len);
    memcpy(out + prefix_len + new_len, end_ptr, suffix_len);
    out[prefix_len + new_len + suffix_len] = '\0';
    return out;
}

/* --- Asignación con índice anidado ---
 *
 * Recorre la cadena de NODE_INDEX desde el nivel más interno (primera
 * clave de la ruta) hasta el más externo (última clave). Si un nivel
 * intermedio no existe, se crea como map vacío. El último nivel asigna
 * `new_val` en la clave/posición correspondiente.
 *
 * `var_value` debe ser un map, un list o un string; `lhs_index` es el
 * NODE_INDEX más externo (aquel cuyo .index es la última clave). */
static void assign_nested_index(Value *var_value, ASTNode *lhs_index,
                                Value new_val, int line,
                                const char *var_name) {
    ASTNode *stack[64];
    int depth = 0;
    ASTNode *cur = lhs_index;
    while (cur && cur->kind == NODE_INDEX && depth < 64) {
        stack[depth++] = cur;
        cur = cur->data.idx.list;
    }
    if (depth == 0) {
        error(line, "Estructura de índice inválida en asignación");
    }

    const char *name = var_name ? var_name : "(sin nombre)";
    if (cur && cur->kind == NODE_INDEX) {
        error(line,
              "La ruta de índice '%s[...]' es demasiado profunda "
              "(máximo 64 niveles)", name);
    }
    Value *container = var_value;

    for (int i = depth - 1; i >= 0; i--) {
        ASTNode *idx_expr = stack[i]->data.idx.index;
        Value key = eval_expr(idx_expr);
        if (key.type == VAL_REFERENCE)
            key = resolve_reference(key, line);

        bool is_last = (i == 0);

        if (container->type == VAL_MAP) {
            if (key.type != VAL_STRING) {
                int key_type = key.type;
                value_free(&key);
                error(line,
                      "La clave de %s[...] debe ser string (se recibió \'%s\').",
                      name, value_type_name(key_type));
            }
            if (is_last) {
                val_map_set(container, key.data.sval, new_val);
            } else {
                if (!val_map_has(*container, key.data.sval)) {
                    val_map_set(container, key.data.sval, val_map_empty());
                }
                MapData *md = container->data.map;
                bool found = false;
                for (int j = 0; j < md->count; j++) {
                    if (strcmp(md->pairs[j].key, key.data.sval) == 0) {
                        container = &md->pairs[j].value;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    value_free(&key);
                    error(line, "Error interno al crear submapa");
                }
            }
        } else if (container->type == VAL_LIST) {
            if (key.type != VAL_INT && key.type != VAL_UINT && key.type != VAL_FLOAT) {
                if (key.type == VAL_STRING) {
                    char msg_buf[1024];
                    snprintf(msg_buf, sizeof(msg_buf),
                        "No se puede hacer %s[%s] = ...: el índice debe ser int "
                        "(se recibió \'string\').\n"
                        "    Un list usa índices numéricos enteros (1, 2, 3...), "
                        "no claves de texto.\n"
                        "    Si querías usar claves de texto, declara la variable como \'map\' "
                        "en lugar de \'list\':\n"
                        "        map %s = []\n"
                        "    Un map usa claves de texto (p. ej. %s[\"clave\"] = valor).",
                        name, key.data.sval ? key.data.sval : "", name, name);
                    value_free(&key);
                    error(line, "%s", msg_buf);
                }
                int key_type = key.type;
                value_free(&key);
                error(line,
                      "No se puede hacer %s[?] = ...: el índice debe ser int "
                      "(se recibió \'%s\').",
                      name, value_type_name(key_type));
            }
            if (key.type == VAL_UINT || (key.type == VAL_INT && VALUE_HAS_WIDE_INTEGER(key))) {
                int64_t index_value;
                if (!value_integer_to_i64(key, &index_value) || index_value < INT_MIN || index_value > INT_MAX) {
                    value_free(&key);
                    error(line, "El índice entero excede el tamaño máximo de una lista");
                }
                key = val_int((int)index_value);
            }
            if (key.type == VAL_FLOAT) {
                double f = key.data.fval;
                if (!isfinite(f) || trunc(f) != f ||
                    f < (double)INT_MIN || f > (double)INT_MAX) {
                    value_free(&key);
                error(line,
                      "No se puede hacer %s[%g] = ...: el índice debe ser int.",
                      name, f);
                    }
                    key.type = VAL_INT;
                    key.data.ival = (int)f;
            }

            int k = key.data.ival;
            int total = container->data.list.count;

            /* Caso especial: lista vacía + índice 1.
             *
             * En Infernal los índices empiezan en 1, así que no hay ninguna
             * posición válida todavía. Pero "escribir en la posición 1 de una
             * lista vacía" es la forma natural de crear el primer elemento, así
             * que lo tratamos como un append en lugar de un error. */
            if (is_last && total == 0 && k == 1) {
                val_list_append(container, copy_value_secure(new_val));
                value_free(&key);
                continue;
            }

            if (k < 1 || k > total) {
                value_free(&key);
                error(line,
                      "No se puede hacer %s[%d] = ...: índice fuera de rango. Rango válido: 1-%d.\n"
                      "    Si querías añadirlo al final usa, %s += valor\n"
                      "    o si lo quieres en una posición concreta usa  %s += valor[n]",
                      name, k, total > 0 ? total : 1,
                      name, name);
            }

            if (is_last) {
                value_free(&container->data.list.items[k - 1]);
                container->data.list.items[k - 1] = copy_value_secure(new_val);
            } else {
                container = &container->data.list.items[k - 1];
            }
        } else if (container->type == VAL_STRING) {
            if (!is_last) {
                value_free(&key);
                error(line,
                      "No se puede indexar un carácter del string '%s' como contenedor.",
                      name);
            }
            if (key.type == VAL_UINT || (key.type == VAL_INT && VALUE_HAS_WIDE_INTEGER(key))) {
                int64_t index_value;
                if (!value_integer_to_i64(key, &index_value) || index_value < INT_MIN || index_value > INT_MAX) {
                    value_free(&key);
                    error(line, "El índice entero excede el tamaño máximo de un string");
                }
                key = val_int((int)index_value);
            }
            if (key.type != VAL_INT) {
                int key_type = key.type;
                value_free(&key);
                error(line,
                      "No se puede hacer %s[?] = ...: el índice del string \'%s\' debe ser int "
                      "(se recibió \'%s\').",
                      name, name, value_type_name(key_type));
            }
            if (new_val.type != VAL_STRING ||
                utf8_char_count_stmt(new_val.data.sval) != 1) {
                int idx_val = key.data.ival;
                value_free(&key);
                error(line,
                      "No se puede hacer %s[%d] = ...: solo se admite 1 carácter.",
                      name, idx_val);
                }
                int k = key.data.ival;
                char *new_str = utf8_replace_char(container->data.sval, k,
                                                  new_val.data.sval, line);
                free(container->data.sval);
                container->data.sval = new_str;
        } else {
            value_free(&key);
            error(line,
                  "No se puede hacer %s[...] = ...: '%s' es de tipo '%s'. Solo list, map y string admiten [...].",
                  name, name, value_type_name(container->type));
        }
        value_free(&key);
    }
                                }

/* --- Implementación de exec_stmt --- */
void exec_stmt(ASTNode *stmt) {
    DEBUG_INFO("exec_stmt: kind=%d, line=%d", stmt->kind, stmt->line);

    switch (stmt->kind) {

        case NODE_EXPR_STMT: {
            Value result = eval_expr(stmt->data.expr_stmt.expr);
            value_free(&result);
            break;
        }

        case NODE_POST_INC:
        case NODE_POST_DEC: {
            Value result = eval_expr(stmt);
            value_free(&result);
            break;
        }

        case NODE_CMD_STMT: {
            char *expanded = expand_command(stmt->data.cmd_stmt.cmd);
            int ret = execute_embedded(expanded);
            if (ret != 0 && command_fail_error) {
                free(expanded);
                error(stmt->line, "Comando embebido falló (código %d): %s", ret, stmt->data.cmd_stmt.cmd);
            }
            free(expanded);
            break;
        }

        case NODE_SHELL_CMD: {
            char *expanded = expand_command(stmt->data.shell_cmd.cmd);
            int ret = run_shell_command(expanded);
            if (ret != 0 && command_fail_error) {
                char msg[1024];
                snprintf(msg, sizeof(msg), "falló: %s", expanded);
                free(expanded);
                error(stmt->line, "%s", msg);
            }
            free(expanded);
            break;
        }

        case NODE_DEFINE: {
            Value value = eval_expr(stmt->data.define.value);
            /* NUEVO: rechazar void como valor de constante. */
            if (value.type == VAL_VOID) {
                error(stmt->line,
                      "La expresión de 'define %s' no devolvió ningún valor (void).",
                      stmt->data.define.name);
            }
            constants_define(stmt->data.define.name, value, stmt->line);
            DEBUG_INFO("Constante '%s' definida", stmt->data.define.name);
            break;
        }

        case NODE_ASSIGN: {
            DEBUG_INFO("ASIGNACION: nombre='%s', is_cmd=%d", stmt->data.assign.name, stmt->data.assign.is_cmd);

            if (constants_is_reserved(stmt->data.assign.name))
                error(stmt->line, "La constante '%s' no se puede sobreescribir", stmt->data.assign.name);

            /* Si la asignación no trae tipo explícito (vtype == 0) y la variable
             * ya existe en el scope, heredamos su tipo declarado. Así
             *     list files = ls $dirActual
             *     ...
             *     files = ls            # sigue siendo list, no string
             * mantiene la coherencia de tipos sin obligar a repetir `list` en cada
             * reasignación. El tipo explícito y el scope (local/global) siguen
             * reservados para la declaración inicial. */
            int effective_vtype = stmt->data.assign.vtype;
            if (effective_vtype == 0 &&
                !stmt->data.assign.is_local &&
                !stmt->data.assign.is_global) {
                VarEntry *existing = scope_find(current_scope, stmt->data.assign.name);
            if (existing) effective_vtype = existing->vtype;
                }

                Value val = val_make_null();

            if (stmt->data.assign.is_cmd) {
                char *cmd = stmt->data.assign.cmd_str;
                int exit_code = 0;

                char *expanded_cmd = expand_command(cmd);
                if (!expanded_cmd) {
                    error(stmt->line, "Error al expandir comando: %s", cmd);
                }

                FuncObject *bare_func = func_lookup(expanded_cmd);
                if (bare_func && bare_func->kind == FUNC_BUILTIN &&
                    strpbrk(expanded_cmd, " \t\r\n") == NULL) {
                    val = bare_func->builtin(0, NULL);
                free(expanded_cmd);
                goto assign_value;
                    }

                    size_t expanded_cmd_len = strlen(expanded_cmd);
                    int is_embedded = (expanded_cmd_len >= 2 &&
                    expanded_cmd[0] == '!' &&
                    expanded_cmd[expanded_cmd_len - 1] == '!');

                    char *cmd_with_redir = NULL;
                    int asprintf_ret = 0;
                    if (is_embedded) {
                        cmd_with_redir = strdup(expanded_cmd);
                    } else {
                        if (effective_vtype == TOK_BOOL) {
                            asprintf_ret = asprintf(&cmd_with_redir, "%s 2>/dev/null", expanded_cmd);
                        } else {
                            asprintf_ret = asprintf(&cmd_with_redir, "%s 2>&1", expanded_cmd);
                        }
                    }
                    free(expanded_cmd);

                    if (asprintf_ret < 0 || !cmd_with_redir) {
                        error(stmt->line, "Memoria insuficiente para redirigir comando");
                    }

                    FILE *fp = NULL;
                    char *temp_path = NULL;

                    if (is_embedded) {
                        size_t embedded_len = strlen(cmd_with_redir);
                        if (embedded_len < 2) {
                            free(cmd_with_redir);
                            error(stmt->line, "Comando embebido inválido");
                        }
                        char *trimmed = strdup(cmd_with_redir + 1);
                        if (!trimmed) {
                            free(cmd_with_redir);
                            error(stmt->line, "Memoria insuficiente para preparar comando embebido");
                        }
                        trimmed[embedded_len - 2] = '\0';
                        fp = popen_embedded_with_path(trimmed, "r", &temp_path);
                        free(trimmed);
                    } else {
                        fp = popen_infernal_shell(cmd_with_redir, "r");
                    }

                    free(cmd_with_redir);

                    if (!fp) {
                        error(stmt->line, "Error al ejecutar comando: %s", cmd);
                    }

                    if (effective_vtype == TOK_BOOL) {
                        char buf[1024];
                        while (fgets(buf, sizeof(buf), fp) != NULL) {}
                        int status = pclose(fp);
                        if (WIFEXITED(status)) {
                            exit_code = WEXITSTATUS(status);
                        } else {
                            exit_code = -1;
                        }
                        if (temp_path) {
                            unlink(temp_path);
                            free(temp_path);
                        }
                        val = val_bool(exit_code == 0);
                    } else {
                        char buf[4096];
                        char *out = strdup("");
                        if (!out) {
                            pclose(fp);
                            if (temp_path) { unlink(temp_path); free(temp_path); }
                            error(stmt->line, "Memoria insuficiente para capturar salida");
                        }
                        while (fgets(buf, sizeof(buf), fp)) {
                            size_t old_len = strlen(out);
                            size_t add_len = strlen(buf);
                            if (old_len > MAX_COMMAND_OUTPUT || add_len > MAX_COMMAND_OUTPUT - old_len - 1) {
                                free(out);
                                pclose(fp);
                                if (temp_path) { unlink(temp_path); free(temp_path); }
                                error(stmt->line, "Salida de comando demasiado grande");
                            }
                            char *tmp_out = realloc(out, old_len + add_len + 1);
                            if (!tmp_out) {
                                free(out);
                                pclose(fp);
                                if (temp_path) { unlink(temp_path); free(temp_path); }
                                error(stmt->line, "Memoria insuficiente para salida de comando");
                            }
                            out = tmp_out;
                            memcpy(out + old_len, buf, add_len + 1);
                        }

                        int status = pclose(fp);

                        if (status != 0 && status != -1 && command_fail_error) {
                            int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

                            char err_output[1024] = "";
                            if (out && out[0]) {
                                size_t out_len = strlen(out);
                                while (out_len > 0 &&
                                    (out[out_len - 1] == '\n' || out[out_len - 1] == '\r')) {
                                    out[out_len - 1] = '\0';
                                out_len--;
                                    }
                                    snprintf(err_output, sizeof(err_output), "%.900s", out);
                            }

                            /* Reexpandir `cmd` para mostrar el comando TAL
                             * COMO lo vio el shell (con $VAR resueltas y ?
                             * ya traducido a $). Sin esto, el mensaje muestra
                             * el texto fuente con $VAR y ? sin resolver, y el
                             * usuario cree que la expansión no funcionó.
                             *
                             * Es seguro reexpandir: el primer expand_command()
                             * ya tuvo éxito y el estado de Infernal no ha
                             * cambiado (el shell corrió como subproceso). Si
                             * por algún motivo la reexpansión falla (no debería),
                             * caemos al texto fuente original `cmd`. */
                            char *display_cmd = expand_command(cmd);
                            if (!display_cmd) display_cmd = (char *)cmd;

                            if (code == 127) {
                                char first_token[256] = "";
                                sscanf(display_cmd, "%255s", first_token);
                                free(out);
                                if (temp_path) { unlink(temp_path); free(temp_path); }
                                VarEntry *same_name = scope_find(current_scope, first_token);
                                if (same_name) {
                                    char msg[4096];
                                    snprintf(msg, sizeof(msg),
                                             "El comando '%s' falló: comando no encontrado.\n"
                                             "    Se ejecutó un comando que no existe y que tiene el mismo nombre que una variable.\n"
                                             "    Quizás querías obtener su valor. Usa '$%s' para clonar su valor.\n"
                                             "    El '$' también representa que vas a trabajar con variables en lugar de comandos.",
                                             display_cmd, first_token);
                                    if (display_cmd != cmd) free(display_cmd);
                                    error(stmt->line, "%s", msg);
                                } else {
                                    char msg[4096];
                                    snprintf(msg, sizeof(msg),
                                             "El comando '%s' falló: comando no encontrado ('%s').",
                                             display_cmd, first_token);
                                    if (display_cmd != cmd) free(display_cmd);
                                    error(stmt->line, "%s", msg);
                                }
                            }

                            free(out);
                            if (temp_path) { unlink(temp_path); free(temp_path); }
                            if (err_output[0]) {
                                char msg[4096];
                                snprintf(msg, sizeof(msg),
                                         "El comando '%s' falló (código de salida %d).\n"
                                         "    Salida del comando:\n        %s",
                                         display_cmd, code, err_output);
                                if (display_cmd != cmd) free(display_cmd);
                                error(stmt->line, "%s", msg);
                            } else {
                                char msg[4096];
                                snprintf(msg, sizeof(msg),
                                         "El comando '%s' falló (código de salida %d)",
                                         display_cmd, code);
                                if (display_cmd != cmd) free(display_cmd);
                                error(stmt->line, "%s", msg);
                            }
                        }

                        if (temp_path) {
                            unlink(temp_path);
                            free(temp_path);
                        }
                        size_t len = strlen(out);
                        if (len > 0 && out[len-1] == '\n') out[len-1] = '\0';

                        if (effective_vtype == TOK_LIST) {
                            Value list = val_list_empty();
                            char *dup = strdup(out);
                            if (!dup) {
                                value_free(&list);
                                free(out);
                                error(stmt->line, "Memoria insuficiente para dividir la salida en líneas");
                            }
                            char *saveptr = NULL;
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
                    }
            } else {
                // Asignación normal (no comando)
                if (stmt->data.assign.lhs_index) {
                    /* Asignación con índice, potencialmente anidado:
                     *   lista[2] = 5
                     *   stats["vida"] = 1000
                     *   texto[1] = "X"            (string)
                     *   pages[web][pagina] = []   (declaración con tipo)
                     *   map pages[web]            (sin valor: default del tipo)
                     *
                     * El último caso (sin `= valor`) antes ignoraba el
                     * índice y sólo inicializaba `pages`. Ahora respeta la
                     * ruta: `map pages[web]` crea `pages[web]` como mapa
                     * vacío, coherente con el modelo "map = caja de
                     * variables". */
                    VarEntry * volatile var = scope_find(current_scope, stmt->data.assign.name);
                    if (!var) {
                        int vtype = stmt->data.assign.vtype;
                        if (vtype == TOK_MAP || vtype == TOK_LIST) {
                            Value init = (vtype == TOK_MAP)
                            ? val_map_empty()
                            : val_list_empty();
                            scope_define(global_scope, stmt->data.assign.name,
                                         vtype, init);
                            var = scope_find_current(global_scope,
                                                     stmt->data.assign.name);
                        }
                        if (!var) {
                            error(stmt->line, "Variable no definida: %s",
                                  stmt->data.assign.name);
                        }
                    }

                    Value new_val;
                    if (stmt->data.assign.value != NULL) {
                        new_val = eval_expr(stmt->data.assign.value);
                    } else {
                        /* Declaración con índice sin valor: usamos el
                         * valor por defecto del tipo declarado. */
                        int vtype = stmt->data.assign.vtype;
                        if (vtype == TOK_INT) new_val = val_int(0);
                        else if (vtype == TOK_UINT) new_val = val_uint(0);
                        else if (vtype == TOK_FLOAT)  new_val = val_float(0.0);
                        else if (vtype == TOK_BOOL)   new_val = val_bool(false);
                        else if (vtype == TOK_STRING) new_val = val_string("");
                        else if (vtype == TOK_LIST)   new_val = val_list_empty();
                        else if (vtype == TOK_MAP)    new_val = val_map_empty();
                        else {
                            error(stmt->line,
                                  "Declaración con índice sin tipo ni valor: "
                                  "no se puede determinar el tipo de %s[...]",
                                  stmt->data.assign.name);
                        }
                    }

                    /* Rechazar void como valor a asignar. */
                    if (new_val.type == VAL_VOID) {
                        value_free(&new_val);
                        error(stmt->line,
                              "La expresión a la derecha de '=' no devolvió ningún valor (void).\n"
                              "    No se puede asignar 'nada' a '%s[...]'.",
                              stmt->data.assign.name);
                    }

                    /* assign_nested_index() puede lanzar desde eval_expr(),
                     * resolve_reference() o una validación de contenedor. El
                     * Value pertenece a este frame hasta que la asignación
                     * termina, así que hay que liberarlo también en el salto. */
                    jmp_buf assign_saved_env;
                    memcpy(&assign_saved_env, &exception_env, sizeof(jmp_buf));
                    int assign_saved_raised = exception_raised;
                    if (setjmp(exception_env) != 0) {
                        value_free(&new_val);
                        memcpy(&exception_env, &assign_saved_env, sizeof(jmp_buf));
                        exception_raised = assign_saved_raised;
                        longjmp(exception_env, 1);
                    }

                    assign_nested_index(&var->value, stmt->data.assign.lhs_index,
                                        new_val, stmt->line,
                                        stmt->data.assign.name);
                    memcpy(&exception_env, &assign_saved_env, sizeof(jmp_buf));
                    exception_raised = assign_saved_raised;
                    value_free(&new_val);
                    break;
                }
                // Asignación sin índice
                if (stmt->data.assign.value != NULL) {
                    if (stmt->data.assign.value->kind == NODE_EMPTY_AMBIGUOUS) {
                        int vtype = stmt->data.assign.vtype;
                        if (vtype == TOK_LIST) {
                            val = val_list_empty();
                        } else if (vtype == TOK_MAP) {
                            val = val_map_empty();
                        } else {
                            VarEntry *existing =
                            scope_find(current_scope, stmt->data.assign.name);
                            if (!existing) {
                                error(stmt->line,
                                      "'[]' es ambiguo: la variable '%s' no existe todavía, "
                                      "así que hay que declararla con tipo explícito. "
                                      "Usa 'list %s = []' o 'map %s = []'.",
                                      stmt->data.assign.name,
                                      stmt->data.assign.name,
                                      stmt->data.assign.name);
                            }
                            if (existing->vtype == TOK_LIST) {
                                val = val_list_empty();
                            } else if (existing->vtype == TOK_MAP) {
                                val = val_map_empty();
                            } else {
                                error(stmt->line,
                                      "'[]' solo se puede asignar a variables list o map; "
                                      "la variable '%s' es de tipo %s.",
                                      stmt->data.assign.name,
                                      type_name(existing->vtype));
                            }
                        }
                    } else {
                        val = eval_expr(stmt->data.assign.value);
                    }
                }
                else if (stmt->data.assign.vtype == TOK_INT)
                    val = val_int(0);
                else if (stmt->data.assign.vtype == TOK_UINT)
                    val = val_uint(0);
                else if (stmt->data.assign.vtype == TOK_FLOAT)
                    val = val_float(0.0);
                else if (stmt->data.assign.vtype == TOK_BOOL)
                    val = val_bool(false);
                else if (stmt->data.assign.vtype == TOK_STRING)
                    val = val_string("");
                else if (stmt->data.assign.vtype == TOK_LIST)
                    val = val_list_empty();
                else if (stmt->data.assign.vtype == TOK_MAP)
                    val = val_map_empty();
            }

            assign_value: {
                int vtype = effective_vtype;

                /* Si la expresión de la derecha evaluó a una referencia
                 * (por ejemplo, 'lista[$i]' donde 'lista' es una variable
                 * sin '$'), resolvemos la referencia para obtener el valor
                 * apuntado. Una referencia no es un valor con tipo propio:
                 * apunta a un valor que sí lo tiene. En un contexto de
                 * asignación (RHS) siempre queremos el valor apuntado,
                 * nunca la referencia. Sin esto, 'string x = lista[$i]'
                 * fallaría con "se obtuvo desconocido" aunque 'lista[$i]'
                 * sea un string. */
                if (val.type == VAL_REFERENCE) {
                    val = resolve_reference(val, stmt->line);
                }

                /* NUEVO: rechazar void como valor a asignar. */
                if (val.type == VAL_VOID) {
                    error(stmt->line,
                          "La función o expresión a la derecha de '=' no devolvió ningún valor (void).\n"
                          "    No se puede asignar 'nada' a la variable '%s'.\n"
                          "    Si la función no debe devolver nada, no la uses en una asignación\n"
                          "    ni en ninguna expresión que espere un valor.",
                          stmt->data.assign.name);
                }

                if (vtype == TOK_STRING && val.type == VAL_LIST) {
                    if (!try_convert_value(&val, TOK_STRING)) {
                        error(stmt->line, "No se pudo convertir la lista a string en la asignación a '%s'",
                              stmt->data.assign.name);
                    }
                }

                if (vtype != 0) {
                    int actual_type = valtype_to_tokentype(val.type);
                    if (vtype != actual_type) {
                        if (!try_convert_value(&val, vtype)) {
                            /* Si el valor es null, casi siempre viene de una función que
                             * terminó sin ejecutar 'return'. Nombramos la función si la
                             * encontramos en el árbol de la expresión. */
                            if (val.type == VAL_NULL) {
                                report_null_from_missing_return(stmt->data.assign.value,
                                                                vtype, stmt->line);
                            }
                            error(stmt->line, "Error de tipado fijo: se esperaba %s pero se obtuvo %s",
                                  type_name(vtype),
                                  val.type == VAL_NULL ? "null" : type_name(actual_type));
                        }
                    }
                }

                DEBUG_INFO("NODE_ASSIGN: is_global=%d, is_local=%d, nombre='%s'",
                           stmt->data.assign.is_global, stmt->data.assign.is_local, stmt->data.assign.name);
                if (stmt->data.assign.is_global) {
                    DEBUG_INFO("Definiendo global '%s' en super_global_scope", stmt->data.assign.name);
                    scope_define(super_global_scope, stmt->data.assign.name, vtype, val);
                } else if (stmt->data.assign.is_local) {
                    DEBUG_INFO("Definiendo local '%s' en current_scope", stmt->data.assign.name);
                    scope_define(current_scope, stmt->data.assign.name, vtype, val);
                } else {
                    VarEntry *var = scope_find(current_scope, stmt->data.assign.name);
                    if (var) {
                        DEBUG_INFO("Variable '%s' encontrada en ámbito %p, actualizando", stmt->data.assign.name, (void*)var);
                        scope_assign(current_scope, stmt->data.assign.name, val, stmt->line);
                        value_free(&val);
                        if (vtype != 0) var->vtype = vtype;
                    } else {
                        DEBUG_INFO("Variable '%s' no encontrada, definiendo en global_scope", stmt->data.assign.name);
                        scope_define(global_scope, stmt->data.assign.name, vtype, val);
                    }
                }
                break;
            }
        }

        case NODE_IF: {
            Value cond = val_make_null();
            ASTNode *cond_node = stmt->data.if_stmt.cond;

            /* Antes se interceptaba cualquier error con setjmp para adornar
             * el caso de una variable directa inexistente. Ese caso se puede
             * identificar sin cambiar el entorno de excepciones; los errores
             * de expresiones anidadas siguen propagándose normalmente. */
            if (cond_node->kind == NODE_VAR) {
                const char *name = cond_node->data.var.name;
                if (*name == '$') name++;
                if (!scope_find(current_scope, name) && !constants_is_reserved(name)) {
                    error(stmt->line,
                          "La condición del 'if' usa la variable '%s', pero no existe.\n"
                          "    - Si querías preguntar si existe, usa:  varexists(\"%s\")\n"
                          "    - Si querías comparar su valor, primero defínela y luego usa una comparación explícita:  ==, !=, <, >, <=, >=.\n",
                          cond_node->data.var.name, cond_node->data.var.name);
                }
            }

            cond = eval_expr(cond_node);

            /* Unificamos el chequeo de condiciones con while y for:
             * se acepta cualquier valor truthy (int != 0, string no vacío,
             * lista/mapa no vacíos, bool). Un valor void (función sin
             * return) se trata como falso, igual que en while/for. */
            bool truthy = val_is_truthy(cond);
            value_free(&cond);

            if (truthy) {
                exec_block_impl(&stmt->data.if_stmt.then_block);
            } else {
                exec_block_impl(&stmt->data.if_stmt.else_block);
            }
            break;
        }

        case NODE_SWITCH: {
            volatile Value selector = val_make_null();
            jmp_buf sw_saved_env;
            memcpy(&sw_saved_env, &exception_env, sizeof(jmp_buf));
            int sw_saved_raised = exception_raised;
            if (setjmp(exception_env) != 0) {
                Value sw_tmp = selector;
                value_free(&sw_tmp);
                memcpy(&exception_env, &sw_saved_env, sizeof(jmp_buf));
                exception_raised = sw_saved_raised;
                longjmp(exception_env, 1);
            }
            selector = eval_expr(stmt->data.switch_stmt.expr);

            int start_idx = -1;
            for (int i = 0; i < stmt->data.switch_stmt.case_count; i++) {
                SwitchCase *swcase = &stmt->data.switch_stmt.cases[i];
                Value case_value = eval_expr(swcase->value);
                Value sel_copy = selector;
                bool equal = switch_values_equal(sel_copy, case_value);
                value_free(&case_value);
                if (equal) {
                    start_idx = i;
                    break;
                }
            }

            if (start_idx >= 0) {
                bool broke = false;
                for (int i = start_idx; i < stmt->data.switch_stmt.case_count; i++) {
                    SwitchCase *swcase = &stmt->data.switch_stmt.cases[i];
                    if (swcase->body.count > 0) {
                        exec_block_impl(&swcase->body);
                    }
                    if (control_flow == CF_BREAK) {
                        control_flow = CF_NONE;
                        broke = true;
                        break;
                    }
                    if (control_flow == CF_CONTINUE) {
                        broke = true;
                        break;
                    }
                    if (control_flow == CF_RETURN || control_flow == CF_REPEAT_LINE) {
                        {
                            Value sw_tmp = selector;
                            value_free(&sw_tmp);
                        }
                        memcpy(&exception_env, &sw_saved_env, sizeof(jmp_buf));
                        exception_raised = sw_saved_raised;
                        return;
                    }
                }

                if (!broke && stmt->data.switch_stmt.has_default) {
                    exec_block_impl(&stmt->data.switch_stmt.default_block);
                    if (control_flow == CF_BREAK) {
                        control_flow = CF_NONE;
                    }
                    if (control_flow == CF_RETURN || control_flow == CF_REPEAT_LINE) {
                        {
                            Value sw_tmp = selector;
                            value_free(&sw_tmp);
                        }
                        memcpy(&exception_env, &sw_saved_env, sizeof(jmp_buf));
                        exception_raised = sw_saved_raised;
                        return;
                    }
                }
            } else if (stmt->data.switch_stmt.has_default) {
                exec_block_impl(&stmt->data.switch_stmt.default_block);
                if (control_flow == CF_BREAK) {
                    control_flow = CF_NONE;
                }
                if (control_flow == CF_RETURN || control_flow == CF_REPEAT_LINE) {
                    {
                        Value sw_tmp = selector;
                        value_free(&sw_tmp);
                    }
                    memcpy(&exception_env, &sw_saved_env, sizeof(jmp_buf));
                    exception_raised = sw_saved_raised;
                    return;
                }
            }

            {
                Value sw_tmp = selector;
                value_free(&sw_tmp);
            }
            memcpy(&exception_env, &sw_saved_env, sizeof(jmp_buf));
            exception_raised = sw_saved_raised;
            break;
        }

        case NODE_WHILE: {
            ASTNode *condition = stmt->data.while_stmt.cond;
            volatile bool constant_true = condition && condition->kind == NODE_LITERAL &&
                                          condition->data.lit.type == TOK_BOOL &&
                                          condition->data.lit.bval;
            volatile int iter_count = 0;
            while (1) {
                if (!constant_true && iter_count >= max_loop_iterations)
                    error(stmt->line, "Límite de iteraciones (%d) alcanzado en bucle while.\n    Si necesitas ampliar el límite de iteraciones, haz:\n\tdefine _MAX_LOOP_LIMIT x\n    Pero reemplazando ese x por el número que quieres que sea el límite de iteraciones.", max_loop_iterations);
                if (!constant_true) iter_count++;

                if (!constant_true) {
                    Value cond = eval_expr(condition);
                    bool truthy = val_is_truthy(cond);
                    value_free(&cond);
                    if (!truthy) break;
                }

                Scope *block_scope = scope_new(current_scope, NULL);
                Scope *old_scope = current_scope;
                jmp_buf saved_env;
                memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
                int saved_raised = exception_raised;

                if (setjmp(exception_env) != 0) {
                    current_scope = old_scope;
                    scope_free(block_scope);
                    memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
                    exception_raised = saved_raised;
                    longjmp(exception_env, 1);
                }

                current_scope = block_scope;
                exec_block_impl(&stmt->data.while_stmt.body);
                current_scope = old_scope;
                scope_free(block_scope);
                memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
                exception_raised = saved_raised;

                if (control_flow == CF_BREAK) { control_flow = CF_NONE; break; }
                if (control_flow == CF_CONTINUE) { control_flow = CF_NONE; continue; }
                if (control_flow == CF_REPEAT_LINE) return;
                if (control_flow == CF_RETURN) return;
            }
            break;
        }

        case NODE_FOR: {
            DEBUG_INFO("=== EJECUTANDO NODE_FOR en línea %d ===", stmt->line);

            Scope *old_scope = current_scope;
            bool is_global_for = stmt->data.for_stmt.is_global;

            Value init_val;

            ASTNode *init_expr = NULL;
            if (stmt->data.for_stmt.init &&
                stmt->data.for_stmt.init->kind == NODE_ASSIGN) {
                init_expr = stmt->data.for_stmt.init->data.assign.value;
            }

            if (init_expr) {
                current_scope = old_scope;
                init_val = eval_expr(init_expr);
                DEBUG_INFO("NODE_FOR: init_val desde expresión");
            } else {
                int vtype = stmt->data.for_stmt.vtype;
                switch (vtype) {
                    case TOK_INT:    init_val = val_int(0);       break;
                    case TOK_UINT:   init_val = val_uint(0);      break;
                    case TOK_FLOAT:  init_val = val_float(0.0);   break;
                    case TOK_BOOL:   init_val = val_bool(false);  break;
                    case TOK_STRING: init_val = val_string("");   break;
                    case TOK_LIST:   init_val = val_list_empty(); break;
                    case TOK_MAP:    init_val = val_map_empty();  break;
                    default:         init_val = val_make_null();  break;
                }
                DEBUG_INFO("NODE_FOR: init_val por defecto del tipo %d", vtype);
            }

            /* El scope del bucle se crea SIEMPRE como hijo de old_scope,
             * para que la condición y el cuerpo puedan ver las variables
             * locales del contexto que contiene al for. Si es 'for global',
             * la variable del bucle se define además en super_global_scope
             * (el incremento y el cuerpo la actualizan allí), pero el
             * scope de evaluación sigue siendo hijo de old_scope. */
            Scope * volatile for_scope = scope_new(old_scope, NULL);
            Scope * volatile assign_scope = is_global_for ? super_global_scope : for_scope;
            DEBUG_INFO("NODE_FOR: %s",
                       is_global_for ? "variable en super_global_scope"
                                     : "variable en for_scope");

            jmp_buf saved_env;
            memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
            int saved_raised = exception_raised;
            if (setjmp(exception_env) != 0) {
                current_scope = old_scope;
                scope_free(for_scope);
                memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
                exception_raised = saved_raised;
                longjmp(exception_env, 1);
            }

            const char *var_name = stmt->data.for_stmt.var;
            int vtype = stmt->data.for_stmt.vtype;

            VarEntry *existing = scope_find_current(assign_scope, var_name);

            if (existing) {
                Value copied = copy_value_secure(init_val);
                value_free(&existing->value);
                existing->value = copied;
                value_free(&init_val);

                if (vtype != 0) {
                    existing->vtype = vtype;
                }

                DEBUG_INFO("NODE_FOR: variable '%s' ya existía; actualizada", var_name);
            } else {
                scope_define(assign_scope, var_name, vtype, init_val);
                DEBUG_INFO("NODE_FOR: variable '%s' definida en %s",
                           var_name,
                           is_global_for ? "super_global_scope" : "for_scope");
            }

            current_scope = for_scope;

            volatile int iter_count = 0;

            while (1) {
                if (iter_count >= max_loop_iterations) {
                    error(stmt->line,
                          "Límite de iteraciones (%d) alcanzado en bucle for.\n    Si necesitas ampliar el límite de iteraciones, haz:\n\tdefine _MAX_LOOP_LIMIT x\n    Pero reemplazando ese x por el número que quieres que sea el límite de iteraciones.",
                          max_loop_iterations);
                }

                iter_count++;

                current_scope = for_scope;

                Value cond = eval_expr(stmt->data.for_stmt.cond);

                if (!val_is_truthy(cond)) {
                    value_free(&cond);
                    break;
                }

                Scope *body_scope = scope_new(for_scope, NULL);
                Scope *old_body = current_scope;
                jmp_buf body_saved_env;
                memcpy(&body_saved_env, &exception_env, sizeof(jmp_buf));
                int body_saved_raised = exception_raised;

                if (setjmp(exception_env) != 0) {
                    current_scope = old_body;
                    scope_free(body_scope);
                    value_free(&cond);
                    memcpy(&exception_env, &body_saved_env, sizeof(jmp_buf));
                    exception_raised = body_saved_raised;
                    longjmp(exception_env, 1);
                }

                current_scope = body_scope;
                exec_block_impl(&stmt->data.for_stmt.body);

                current_scope = old_body;
                scope_free(body_scope);
                memcpy(&exception_env, &body_saved_env, sizeof(jmp_buf));
                exception_raised = body_saved_raised;
                value_free(&cond);

                if (control_flow == CF_BREAK) {
                    control_flow = CF_NONE;
                    break;
                }

                if (control_flow == CF_CONTINUE) {
                    control_flow = CF_NONE;
                }

                if (control_flow == CF_REPEAT_LINE ||
                    control_flow == CF_RETURN) {

                    current_scope = old_scope;
                    scope_free(for_scope);
                    memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
                    exception_raised = saved_raised;

                    return;
                }

                if (stmt->data.for_stmt.incr) {
                    current_scope = for_scope;
                    exec_stmt(stmt->data.for_stmt.incr);
                }
            }

            current_scope = old_scope;
            scope_free(for_scope);
            memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
            exception_raised = saved_raised;

            break;
        }

        case NODE_FOR_IN: {
            Value volatile iterable = eval_expr(stmt->data.for_in.list_expr);

            /* Si la expresión devolvió una referencia (por ejemplo,
             * `for x in lista[1]`), la resolvemos antes de iterar. */
            if (iterable.type == VAL_REFERENCE)
                iterable = resolve_reference(iterable, stmt->line);

            if (iterable.type != VAL_LIST &&
                iterable.type != VAL_STRING &&
                iterable.type != VAL_MAP) {
                value_free((Value *)&iterable);
            error(stmt->line, "Se esperaba una lista, string o mapa en for-in");
                }

                /* Si el contenedor es una variable simple, guardamos su VarEntry
                 * para poder recalcular `i` como la posición viva del elemento. */
                const char * volatile live_var_name = NULL;
                if (stmt->data.for_in.list_expr &&
                    stmt->data.for_in.list_expr->kind == NODE_VAR) {
                    const char *vname = stmt->data.for_in.list_expr->data.var.name;
                if (vname && (vname[0] == '$' || vname[0] == '?')) vname++;
                if (vname && *vname)
                    live_var_name = vname;
                    }

                    volatile int count = 0;
                if (iterable.type == VAL_LIST) {
                    count = iterable.data.list.count;
                } else if (iterable.type == VAL_MAP) {
                    count = iterable.data.map ? iterable.data.map->count : 0;
                } else {
                    const unsigned char *p = (const unsigned char *)iterable.data.sval;
                    while (*p) {
                        unsigned char c = *p;
                        size_t width = 1;
                        if (c < 0x80) width = 1;
                        else if ((c & 0xE0) == 0xC0 && p[1]) width = 2;
                        else if ((c & 0xF0) == 0xE0 && p[1] && p[2]) width = 3;
                        else if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) width = 4;
                        p += width;
                        count++;
                    }
                }

                const unsigned char * volatile string_iter =
                    iterable.type == VAL_STRING
                        ? (const unsigned char *)iterable.data.sval : NULL;
                for (volatile int iter_idx = 0; iter_idx < count; iter_idx++) {
                    Scope *old_scope = current_scope;
                    volatile Scope *iter_scope = scope_new(old_scope, NULL);
                    volatile Value item = val_make_null();

                    jmp_buf saved_env;
                    memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
                    int saved_raised = exception_raised;
                    if (setjmp(exception_env) != 0) {
                        current_scope = old_scope;
                        scope_free((Scope *)iter_scope);
                        value_free((Value *)&iterable);
                        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
                        exception_raised = saved_raised;
                        longjmp(exception_env, 1);
                    }

                    current_scope = (Scope *)iter_scope;

                    /* --- Obtener el elemento a visitar desde el snapshot --- */
                    if (iterable.type == VAL_LIST) {
                        item = copy_value_secure(iterable.data.list.items[iter_idx]);
                    } else if (iterable.type == VAL_MAP) {
                        item = val_string(iterable.data.map->pairs[iter_idx].key);
                    } else {
                        const unsigned char *p = string_iter;
                        size_t width = 1;
                        unsigned char c = *p;
                        if (c >= 0xC0 && (c & 0xE0) == 0xC0 && p[1]) width = 2;
                        else if (c >= 0xE0 && (c & 0xF0) == 0xE0 && p[1] && p[2]) width = 3;
                        else if (c >= 0xF0 && (c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) width = 4;
                        string_iter = p + width;
                        char ch[5];
                        memcpy(ch, p, width);
                        ch[width] = '\0';
                        item = val_string(ch);
                    }

                    /* --- Calcular `i` como la posición viva del elemento.
                     *
                     * Buscamos el valor del elemento visitado en la lista actual.
                     * Si lo encontramos, esa es la posición (1-based). Si no lo
                     * encontramos (porque el usuario ya lo eliminó, o porque el
                     * contenedor no es una lista viva), caemos al número de
                     * secuencia. --- */
                    int live_index = iter_idx + 1;  /* fallback: número de secuencia */

                    const char *lvname = (const char *)live_var_name;
                    VarEntry *lv = iterable.type == VAL_LIST && lvname
                                 ? scope_find(current_scope, lvname) : NULL;
                    if (lv && lv->value.type == VAL_LIST &&
                        iterable.type == VAL_LIST) {
                        Value *live_items = lv->value.data.list.items;
                        int live_count = lv->value.data.list.count;
                        for (int j = 0; j < live_count; j++) {
                            if (forin_values_equal(live_items[j], item)) {
                                live_index = j + 1;
                                break;
                            }
                        }
                    }

                        /* --- Publicar las variables del bucle en el scope --- */
                        scope_define((Scope *)iter_scope, stmt->data.for_in.var, 0, (Value)item);
                        item = val_make_null(); /* propiedad transferida al scope */

                        if (stmt->data.for_in.index_var) {
                            scope_define((Scope *)iter_scope, stmt->data.for_in.index_var,
                                         TOK_INT, val_int(live_index));
                        }

                        exec_block_impl(&stmt->data.for_in.body);
                        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
                        exception_raised = saved_raised;
                        current_scope = old_scope;
                        scope_free((Scope *)iter_scope);

                        if (control_flow == CF_BREAK) { control_flow = CF_NONE; break; }
                        if (control_flow == CF_CONTINUE) { control_flow = CF_NONE; continue; }
                        if (control_flow == CF_REPEAT_LINE || control_flow == CF_RETURN) {
                            value_free((Value *)&iterable);
                            return;
                        }
                }
                value_free((Value *)&iterable);
                break;
        }

        case NODE_FUNC_DEF:
            break;

        case NODE_RETURN: {
            Value result = stmt->data.ret.expr ? eval_expr(stmt->data.ret.expr) : val_void();

            if (stmt->data.ret.rtype != 0) {
                int expected = stmt->data.ret.rtype;
                if (expected == TOK_STRING && result.type == VAL_LIST) {
                    if (!try_convert_value(&result, expected)) {
                        value_free(&result);
                        error(stmt->line, "No se pudo convertir el valor de retorno a %s", type_name(expected));
                    }
                }

                if (expected == TOK_UINT && !try_convert_value(&result, TOK_UINT)) {
                    value_free(&result);
                    error(stmt->line, "El retorno uint requiere un entero válido");
                }

                int actual = valtype_to_tokentype(result.type);
                if (actual != expected) {
                    bool was_void = (result.type == VAL_VOID);
                    value_free(&result);
                    if (was_void) {
                        error(stmt->line,
                              "Tipado del retorno: se esperaba %s, "
                              "pero la función no devolvió ningún valor (void).",
                              type_name(expected));
                    } else {
                        error(stmt->line, "Tipado del retorno: se esperaba %s, se recibió %s",
                              type_name(expected), type_name(actual));
                    }
                }
            }

            value_free(&return_value);
            return_value = result;
            control_flow = CF_RETURN;
            return;
        }

        case NODE_BREAK:
            control_flow = CF_BREAK;
            return;

        case NODE_CONTINUE:
            control_flow = CF_CONTINUE;
            return;

        case NODE_PORTAL: {
            const char *name = stmt->data.portal.name;
            Scope *target_scope = current_scope;

            if (portal_find_in_scope(target_scope, name)) {
                error(stmt->line, "Portal '%s' ya existe en este ámbito", name);
            }
            int next_line = stmt->line + 1;
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
                if (line_val.type == VAL_UINT || (line_val.type == VAL_INT && VALUE_HAS_WIDE_INTEGER(line_val))) {
                    int64_t number;
                    if (!value_integer_to_i64(line_val, &number) || number < INT_MIN || number > INT_MAX) {
                        value_free(&line_val);
                        error(stmt->line, "repeat line excede el rango de líneas");
                    }
                    line_val = val_int((int)number);
                }
                if (line_val.type != VAL_INT)
                    error(stmt->line, "repeat line requiere un número entero");
                repeat_line_target = line_val.data.ival;
            }
            control_flow = CF_REPEAT_LINE;
            return;
        }

        case NODE_IMPORT: {
            ImportModule *module = stmt->data.import.cache;
            if (module && module->loaded) break;
            Scope *old_scope = current_scope;
            jmp_buf saved_env;
            memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
            int saved_raised = exception_raised;

            if (setjmp(exception_env) != 0) {
                current_scope = old_scope;
                memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
                exception_raised = saved_raised;
                longjmp(exception_env, 1);
            }

            current_scope = global_scope;
            exec_block_impl(module ? &module->block : &stmt->data.import.module_block);
            current_scope = old_scope;
            memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
            exception_raised = saved_raised;
            if (module && control_flow == CF_NONE) module->loaded = true;
            if (control_flow == CF_REPEAT_LINE) return;
            break;
        }

        case NODE_TRY: {
            jmp_buf saved_env;
            memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
            int saved_raised = exception_raised;
            exception_raised = 0;

            volatile bool caught = false;

            if (!setjmp(exception_env)) {
                exec_block_impl(&stmt->data.try_stmt.try_block);
            } else {
                caught = true;
            }

            memcpy(&exception_env, &saved_env, sizeof(jmp_buf));

            if (caught) {
                exception_raised = 0;
                exec_block_impl(&stmt->data.try_stmt.catch_block);
            }

            exception_raised = saved_raised;
            if (control_flow == CF_REPEAT_LINE) return;
            break;
        }

        case NODE_EXECUTE: {
            bool saved_constants_definition_allowed = constants_definition_allowed();
            ASTNode *path_node = stmt->data.execute.path_expr;
            Value path_val = val_make_null();
            char * volatile path_owned = NULL;
            char * volatile expanded_path = NULL;
            char * volatile resolved_path = NULL;
            char ** volatile expanded_args = NULL;
            int expanded_argc = stmt->data.execute.argc;
            char ** volatile new_argv = NULL;
            volatile bool state_switched = false;
            volatile bool child_scope_active = false;
            Scope * volatile child_scope = NULL;
            Scope * volatile old_scope = NULL;
            FILE * volatile script_fp = NULL;

            TokenStream saved_ts = ts;
            char **saved_source_lines = source_lines;
            int saved_source_line_count = source_line_count;
            char *saved_source_file = current_source_file;
            int saved_argc = script_argc;
            char **saved_argv = script_argv;
            int saved_flags_arg_index = flags_arg_index;

            jmp_buf saved_env;
            memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
            int saved_raised = exception_raised;

            if (setjmp(exception_env) != 0) {
                if (script_fp) {
                    fclose((FILE *)script_fp);
                    script_fp = NULL;
                }

                if (child_scope_active) {
                    current_scope = (Scope *)old_scope;
                    scope_free((Scope *)child_scope);
                    child_scope_active = false;
                }

                if (state_switched) {
                    if (ts.tokens) {
                        for (int i = 0; i < ts.count; i++) free(ts.tokens[i].lexeme);
                        free(ts.tokens);
                    }
                    ts = saved_ts;
                    if (source_lines) {
                        for (int i = 0; i < source_line_count; i++) free(source_lines[i]);
                        free(source_lines);
                    }
                    source_lines = saved_source_lines;
                    source_line_count = saved_source_line_count;
                    current_source_file = saved_source_file;
                    state_switched = false;
                }

                constants_set_definition_allowed(saved_constants_definition_allowed);
                script_argc = saved_argc;
                script_argv = saved_argv;
                flags_arg_index = saved_flags_arg_index;

                for (int i = 0; expanded_args && i < expanded_argc; i++)
                    free(expanded_args[i]);
                free((void *)expanded_args);
                free((void *)new_argv);
                free((void *)resolved_path);
                free((void *)expanded_path);
                free((void *)path_owned);

                memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
                exception_raised = saved_raised;
                longjmp(exception_env, 1);
            }

            constants_set_definition_allowed(false);

            bool looks_like_path = false;
            if (path_node && path_node->kind == NODE_VAR) {
                const char *n = path_node->data.var.name;
                if (n && (strchr(n, '/') != NULL || strchr(n, '.') != NULL))
                    looks_like_path = true;
            }

            if (looks_like_path) {
                path_val = val_string(path_node->data.var.name);
            } else {
                path_val = eval_expr(path_node);
            }

            if (path_val.type != VAL_STRING) {
                value_free(&path_val);
                error(stmt->line, "La ruta del script debe ser una cadena");
            }
            path_owned = path_val.data.sval;
            path_val.type = VAL_NULL;
            path_val.data.sval = NULL;
            expanded_path = expand_command(path_owned ? path_owned : "");
            free((void *)path_owned);
            path_owned = NULL;
            if (!expanded_path) {
                error(stmt->line, "Error al expandir la ruta del script");
            }

            if (expanded_path[0] != '/') {
                const char *base = current_source_file;
                char *base_abs = base ? realpath(base, NULL) : NULL;
                if (base_abs) {
                    char *slash = strrchr(base_abs, '/');
                    if (slash) *slash = '\0';
                    size_t base_len = strlen(base_abs);
                    size_t rel_len = strlen(expanded_path);
                    if (base_len > SIZE_MAX - rel_len - 2) {
                        free(base_abs);
                        error(stmt->line, "Ruta del script demasiado larga");
                    }
                    resolved_path = malloc(base_len + rel_len + 2);
                    if (!resolved_path) {
                        free(base_abs);
                        error(stmt->line, "Memoria insuficiente al resolver la ruta del script");
                    }
                    snprintf(resolved_path, base_len + rel_len + 2,
                             "%s/%s", base_abs, expanded_path);
                    free(base_abs);
                }
            }
            const char *final_path = resolved_path ? resolved_path : expanded_path;

            if (expanded_argc > 0) {
                if ((size_t)expanded_argc > SIZE_MAX / sizeof(char *))
                    error(stmt->line, "Demasiados argumentos para execute");
                expanded_args = calloc((size_t)expanded_argc, sizeof(char *));
                if (!expanded_args)
                    error(stmt->line, "Memoria insuficiente para argumentos de execute");
                for (int i = 0; i < expanded_argc; i++) {
                    expanded_args[i] = expand_command(stmt->data.execute.args[i]);
                    if (!expanded_args[i]) expanded_args[i] = strdup("");
                }
            }

            if ((size_t)expanded_argc > (SIZE_MAX / sizeof(char *)) - 2)
                error(stmt->line, "Demasiados argumentos para execute");
            new_argv = malloc(((size_t)expanded_argc + 2) * sizeof(char *));
            if (!new_argv)
                error(stmt->line, "Memoria insuficiente para argv de execute");
            new_argv[0] = (char *)final_path;
            for (int i = 0; i < expanded_argc; i++)
                new_argv[i + 1] = expanded_args[i];
            new_argv[expanded_argc + 1] = NULL;

            script_argc = expanded_argc + 1;
            script_argv = new_argv;
            flags_arg_index = 1;

            script_fp = fopen(final_path, "r");
            if (!script_fp) {
                error(stmt->line, "No se pudo abrir el script '%s'", final_path);
            }

            ts_init();
            source_lines = NULL;
            source_line_count = 0;
            current_source_file = (char *)final_path;
            state_switched = true;

            tokenize_file((FILE *)script_fp);
            fclose((FILE *)script_fp);
            script_fp = NULL;

            NodeList script_block = parse_block(NULL);

            old_scope = current_scope;
            child_scope = scope_new((Scope *)old_scope, NULL);
            child_scope_active = true;
            current_scope = (Scope *)child_scope;
            exec_block_impl(&script_block);

            current_scope = (Scope *)old_scope;
            scope_free((Scope *)child_scope);
            child_scope_active = false;
            nodelist_free(&script_block);

            if (state_switched) {
                if (ts.tokens) {
                    for (int i = 0; i < ts.count; i++) free(ts.tokens[i].lexeme);
                    free(ts.tokens);
                }
                if (source_lines) {
                    for (int i = 0; i < source_line_count; i++) free(source_lines[i]);
                    free(source_lines);
                }
                ts = saved_ts;
                source_lines = saved_source_lines;
                source_line_count = saved_source_line_count;
                current_source_file = saved_source_file;
                state_switched = false;
            }

            script_argc = saved_argc;
            script_argv = saved_argv;
            flags_arg_index = saved_flags_arg_index;
            constants_set_definition_allowed(saved_constants_definition_allowed);

            free((void *)resolved_path);
            free((void *)expanded_path);
            for (int i = 0; expanded_args && i < expanded_argc; i++)
                free(expanded_args[i]);
            free((void *)expanded_args);
            free((void *)new_argv);

            memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
            exception_raised = saved_raised;
            break;
        }

        case NODE_FLAGS: {
            exec_flags(stmt);
            break;
        }

        default:
            DEBUG_ERROR("Nodo desconocido en exec_stmt: kind=%d, line=%d", stmt->kind, stmt->line);
            error(stmt->line, "Error interno: nodo no reconocido (tipo %d). Asegúrate de que la sintaxis sea correcta.", stmt->kind);
    }
}
