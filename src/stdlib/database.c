/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: stdlib/database.c
 *
 * Trabajar con datos persistentes en el sistema de archivos.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <ctype.h>
#include <setjmp.h>
#include "database.h"
#include "core/value.h"
#include "runtime/error.h"
#include "runtime/globals.h"
#include "vm/vm.h"

/* ============================================================
 *  Límites de seguridad
 * ============================================================ */
#define DB_MAX_DEPTH               64
#define DB_MAX_ITEMS          1000000
#define DB_MAX_STRING_LEN     (16ULL * 1024 * 1024)
#define DB_MAX_FILE_SIZE      (256ULL * 1024 * 1024)
#define DB_MAX_SERIALIZED_SIZE (256ULL * 1024 * 1024)

#define INDENT_WIDTH 4

/* ============================================================
 *  Resolución de rutas relativas al script en ejecución
 * ============================================================ */
static char *resolve_script_relative_path(const char *path) {
    if (!path) return NULL;
    if (path[0] == '/') return strdup(path);

    const char *base = current_source_file;
    if (!base) return strdup(path);

    char *base_abs = realpath(base, NULL);
    if (!base_abs) return strdup(path);

    char *slash = strrchr(base_abs, '/');
    if (!slash) {
        free(base_abs);
        return strdup(path);
    }
    *slash = '\0';

    size_t need = strlen(base_abs) + 1 + strlen(path) + 1;
    char *resolved = malloc(need);
    if (!resolved) {
        free(base_abs);
        return strdup(path);
    }
    snprintf(resolved, need, "%s/%s", base_abs, path);
    free(base_abs);
    return resolved;
}

/* ============================================================
 *  Cabecera mágica y versionado para formato binario
 * ============================================================ */
static const unsigned char DB_BINARY_MAGIC[4] = { 'I', 'N', 'D', 'B' };
#define DB_BINARY_VERSION 1
#define DB_BINARY_HEADER_SIZE (sizeof(DB_BINARY_MAGIC) + 1)

/* ============================================================
 *  Buffer dinámico de texto
 * ============================================================ */
static int append_vprintf(char **buffer, size_t *cap, size_t *len,
                          const char *fmt, va_list args) {
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    if (needed < 0) return -1;

    size_t needed_data = (size_t)needed;
    if (needed_data > DB_MAX_SERIALIZED_SIZE - *len) {
        errno = EFBIG;
        return -1;
    }
    size_t new_len = *len + needed_data;
    if (new_len + 1 > *cap) {
        size_t new_cap = new_len + 1024;
        if (new_cap > DB_MAX_SERIALIZED_SIZE + 1)
            new_cap = DB_MAX_SERIALIZED_SIZE + 1;
        char *tmp = realloc(*buffer, new_cap);
        if (!tmp) return -1;
        *buffer = tmp;
        *cap = new_cap;
    }
    vsnprintf(*buffer + *len, *cap - *len, fmt, args);
    *len += needed_data;
    return 0;
}

static int append_printf(char **buffer, size_t *cap, size_t *len,
                         const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = append_vprintf(buffer, cap, len, fmt, args);
    va_end(args);
    return ret;
}

static int append_string(char **buffer, size_t *cap, size_t *len,
                         const char *str) {
    if (!str) str = "";
    size_t slen = strlen(str);
    if (slen > DB_MAX_SERIALIZED_SIZE - *len) {
        errno = EFBIG;
        return -1;
    }
    if (*len + slen + 1 > *cap) {
        size_t new_cap = *len + slen + 1024;
        if (new_cap > DB_MAX_SERIALIZED_SIZE + 1)
            new_cap = DB_MAX_SERIALIZED_SIZE + 1;
        char *tmp = realloc(*buffer, new_cap);
        if (!tmp) return -1;
        *buffer = tmp;
        *cap = new_cap;
    }
    memcpy(*buffer + *len, str, slen);
    *len += slen;
    (*buffer)[*len] = '\0';
    return 0;
}

static int append_char(char **buffer, size_t *cap, size_t *len, char c) {
    if (*len + 1 + 1 > *cap) {
        size_t new_cap = *cap ? *cap * 2 : 1024;
        if (new_cap > DB_MAX_SERIALIZED_SIZE + 1)
            new_cap = DB_MAX_SERIALIZED_SIZE + 1;
        char *tmp = realloc(*buffer, new_cap);
        if (!tmp) return -1;
        *buffer = tmp;
        *cap = new_cap;
    }
    (*buffer)[(*len)++] = c;
    (*buffer)[*len] = '\0';
    return 0;
}

static int append_indent(char **buffer, size_t *cap, size_t *len, int level) {
    for (int i = 0; i < level * INDENT_WIDTH; i++) {
        if (append_char(buffer, cap, len, ' ') < 0) return -1;
    }
    return 0;
}

static int append_quoted_string(char **buffer, size_t *cap, size_t *len,
                                const char *s) {
    if (append_char(buffer, cap, len, '"') < 0) return -1;
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
                case '\\': if (append_string(buffer, cap, len, "\\\\") < 0) return -1; break;
                case '"':  if (append_string(buffer, cap, len, "\\\"") < 0) return -1; break;
                case '\n': if (append_string(buffer, cap, len, "\\n") < 0) return -1; break;
                case '\r': if (append_string(buffer, cap, len, "\\r") < 0) return -1; break;
                case '\t': if (append_string(buffer, cap, len, "\\t") < 0) return -1; break;
                default:
                    if (append_char(buffer, cap, len, (char)*p) < 0) return -1;
            }
        }
    }
    return append_char(buffer, cap, len, '"');
}

/*
 * Escribe una clave de mapa. Si la clave está formada únicamente por
 * caracteres válidos como identificador (letras, dígitos, '_', '.'),
 * o bien contiene '-' pero nunca '--' (que el lexer tokeniza como
 * TOK_DEC), se escribe SIN comillas. En cualquier otro caso se
 * entrecomilla para preservar la clave tal cual.
 */
static int append_map_key(char **buffer, size_t *cap, size_t *len,
                          const char *key) {
    if (!key) key = "";
    bool simple = (key[0] != '\0' &&
                   (isalpha((unsigned char)key[0]) || key[0] == '_'));
    if (simple) {
        for (const char *p = key + 1; *p; p++) {
            unsigned char uc = (unsigned char)*p;
            if (isalnum(uc) || uc == '_' || uc == '.') continue;
            if (uc == '-') {
                /* '--' se tokeniza como TOK_DEC y rompería la clave. */
                if (p[1] == '-') { simple = false; break; }
                continue;
            }
            simple = false;
            break;
        }
    }
    if (simple) return append_string(buffer, cap, len, key);
    return append_quoted_string(buffer, cap, len, key);
}

/* ============================================================
 *  Serialización a texto (formato Infernal)
 * ============================================================ */

static int value_to_text_rec(Value v, char **out, size_t *cap, size_t *len,
                             int depth, int indent_level, bool force_inline) {
    if (depth > DB_MAX_DEPTH)
        error(current_eval_line, "Demasiada profundidad en la serialización de datos");

    switch (v.type) {
        case VAL_INT:
            return append_printf(out, cap, len, "%d", v.data.ival);

        case VAL_FLOAT: {
            if (!isfinite(v.data.fval))
                error(current_eval_line, "No se puede serializar un flotante no finito (NaN/Inf)");
            return append_printf(out, cap, len, "%.17g", v.data.fval);
        }

        case VAL_BOOL:
            return append_string(out, cap, len, v.data.bval ? "true" : "false");

        case VAL_STRING:
            return append_quoted_string(out, cap, len, v.data.sval);

        case VAL_NULL:
            return append_string(out, cap, len, "null");

        case VAL_LIST: {
            if (append_char(out, cap, len, '[') < 0) return -1;
            for (int i = 0; i < v.data.list.count; i++) {
                if (i > 0 && append_string(out, cap, len, ", ") < 0) return -1;
                if (value_to_text_rec(v.data.list.items[i], out, cap, len,
                    depth + 1, indent_level, true) < 0)
                    return -1;
            }
            return append_char(out, cap, len, ']');
        }

        case VAL_MAP: {
            MapData *md = v.data.map;
            if (!md) return append_string(out, cap, len, "[ ]");

            if (md->count == 0)
                return append_string(out, cap, len, "[ ]");

            if (force_inline) {
                if (append_char(out, cap, len, '[') < 0) return -1;
                for (int i = 0; i < md->count; i++) {
                    if (i > 0 && append_string(out, cap, len, ", ") < 0) return -1;
                    if (append_map_key(out, cap, len, md->pairs[i].key) < 0) return -1;
                    if (append_string(out, cap, len, " = ") < 0) return -1;
                    if (value_to_text_rec(md->pairs[i].value, out, cap, len,
                        depth + 1, indent_level, true) < 0)
                        return -1;
                }
                return append_char(out, cap, len, ']');
            }

            if (append_string(out, cap, len, "[\n") < 0) return -1;
            for (int i = 0; i < md->count; i++) {
                if (append_indent(out, cap, len, indent_level + 1) < 0) return -1;
                if (append_map_key(out, cap, len, md->pairs[i].key) < 0) return -1;
                if (append_string(out, cap, len, " = ") < 0) return -1;
                if (value_to_text_rec(md->pairs[i].value, out, cap, len,
                    depth + 1, indent_level + 1, false) < 0)
                    return -1;
                if (append_char(out, cap, len, '\n') < 0) return -1;
            }
            if (append_indent(out, cap, len, indent_level) < 0) return -1;
            return append_char(out, cap, len, ']');
        }

        default:
            error(current_eval_line, "Tipo de dato no soportado para serialización a texto");
    }
    return -1;
}

static char *value_to_text(Value v, size_t *out_len) {
    char *buffer = malloc(1024);
    if (!buffer) return NULL;
    size_t cap = 1024, len = 0;
    buffer[0] = '\0';
    if (value_to_text_rec(v, &buffer, &cap, &len, 0, 0, false) < 0) {
        free(buffer);
        return NULL;
    }
    buffer[len] = '\0';
    *out_len = len;
    return buffer;
}

/* ============================================================
 *  Deserialización de texto (formato Infernal)
 * ============================================================ */

typedef struct {
    const char *input;
    size_t pos;
    size_t len;
} TextParser;

static void skip_ws_and_comments(TextParser *p) {
    while (p->pos < p->len) {
        char c = p->input[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else if (c == '#') {
            while (p->pos < p->len && p->input[p->pos] != '\n') p->pos++;
        } else {
            break;
        }
    }
}

static int parse_string(TextParser *p, char **out) {
    if (p->pos >= p->len || p->input[p->pos] != '"')
        return -1;
    p->pos++;
    size_t start = p->pos;
    while (p->pos < p->len && p->input[p->pos] != '"') {
        if (p->input[p->pos] == '\\' && p->pos + 1 < p->len)
            p->pos += 2;
        else
            p->pos++;
    }
    if (p->pos >= p->len || p->input[p->pos] != '"')
        return -1;
    size_t end = p->pos;
    size_t raw_len = end - start;
    if (raw_len > DB_MAX_STRING_LEN)
        error(current_eval_line, "String demasiado largo en el archivo de texto");
    char *result = malloc(raw_len + 1);
    if (!result) return -1;
    size_t out_i = 0;
    for (size_t i = start; i < end; i++) {
        if (p->input[i] == '\\' && i + 1 < end) {
            i++;
            switch (p->input[i]) {
                case 'n':  result[out_i++] = '\n'; break;
                case 't':  result[out_i++] = '\t'; break;
                case 'r':  result[out_i++] = '\r'; break;
                case '\\': result[out_i++] = '\\'; break;
                case '"':  result[out_i++] = '"';  break;
                default:
                    free(result);
                    error(current_eval_line, "Escape inválido en string: \\%c", p->input[i]);
            }
        } else {
            result[out_i++] = p->input[i];
        }
    }
    result[out_i] = '\0';
    *out = result;
    p->pos++;
    return 0;
}

static bool looks_like_map(TextParser *p) {
    size_t save = p->pos;
    int depth = 0;
    bool in_string = false;
    bool in_comment = false;
    bool result = false;

    while (p->pos < p->len) {
        char c = p->input[p->pos];
        if (in_comment) {
            if (c == '\n') in_comment = false;
            p->pos++;
            continue;
        }
        if (in_string) {
            if (c == '\\' && p->pos + 1 < p->len) { p->pos += 2; continue; }
            if (c == '"') in_string = false;
            p->pos++;
            continue;
        }
        if (c == '#') { in_comment = true; p->pos++; continue; }
        if (c == '"') { in_string = true; p->pos++; continue; }
        if (c == '[') {
            depth++;
        } else if (c == ']') {
            if (depth == 0) break;
            depth--;
        } else if (c == '=' && depth == 0) {
            result = true;
            break;
        } else if (c == ',' && depth == 0) {
            break;
        }
        p->pos++;
    }
    p->pos = save;
    return result;
}

/*
 * Lee una clave de mapa. Acepta:
 *   - una cadena entrecomillada: "lyndscorp.com"
 *   - un identificador simple sin comillas: lyndscorp.com, gnu.org, mi-clave
 *
 * Un identificador válido empieza por letra o '_' y continúa con letras,
 * dígitos, '_', '.' o '-'. La secuencia '--' termina el identificador
 * porque el lexer del lenguaje la tokeniza como TOK_DEC.
 */
static char *parse_map_key(TextParser *p) {
    if (p->pos >= p->len)
        error(current_eval_line, "Se esperaba clave de mapa");
    char c = p->input[p->pos];
    if (c == '"') {
        char *key = NULL;
        if (parse_string(p, &key) != 0)
            error(current_eval_line, "Clave de mapa inválida");
        return key;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        size_t start = p->pos;
        p->pos++;
        while (p->pos < p->len) {
            char cc = p->input[p->pos];
            if (isalnum((unsigned char)cc) || cc == '_' || cc == '.') {
                p->pos++;
                continue;
            }
            if (cc == '-') {
                if (p->pos + 1 < p->len && p->input[p->pos + 1] == '-') break;
                p->pos++;
                continue;
            }
            break;
        }
        size_t klen = p->pos - start;
        char *key = malloc(klen + 1);
        if (!key) error(current_eval_line, "Memoria insuficiente");
        memcpy(key, p->input + start, klen);
        key[klen] = '\0';
        return key;
    }
    error(current_eval_line, "Se esperaba clave de mapa, se encontró '%c'", c);
    return NULL;
}

static Value parse_value(TextParser *p, int depth);

static Value parse_list(TextParser *p, int depth) {
    Value list = val_list_empty();
    skip_ws_and_comments(p);
    if (p->pos < p->len && p->input[p->pos] == ']') {
        p->pos++;
        return list;
    }
    while (1) {
        skip_ws_and_comments(p);
        if (p->pos >= p->len)
            error(current_eval_line, "Lista no cerrada correctamente");
        if (p->input[p->pos] == ']') { p->pos++; break; }
        Value item = parse_value(p, depth + 1);
        if (list.data.list.count >= DB_MAX_ITEMS)
            error(current_eval_line, "Demasiados elementos en la lista");
        val_list_append(&list, item);
        skip_ws_and_comments(p);
        if (p->pos >= p->len)
            error(current_eval_line, "Lista no cerrada correctamente");
        if (p->input[p->pos] == ',') { p->pos++; continue; }
        if (p->input[p->pos] == ']') { p->pos++; break; }
        error(current_eval_line, "Se esperaba ',' o ']' en la lista");
    }
    return list;
}

static Value parse_map(TextParser *p, int depth) {
    Value map = val_map_empty();
    while (1) {
        skip_ws_and_comments(p);
        if (p->pos >= p->len)
            error(current_eval_line, "Mapa no cerrado correctamente");
        if (p->input[p->pos] == ']') { p->pos++; break; }

        char *key = parse_map_key(p);
        skip_ws_and_comments(p);
        if (p->pos >= p->len || p->input[p->pos] != '=') {
            free(key);
            error(current_eval_line, "Se esperaba '=' después de la clave del mapa");
        }
        p->pos++;
        skip_ws_and_comments(p);
        Value val = parse_value(p, depth + 1);
        if (map.data.map->count >= DB_MAX_ITEMS) {
            free(key);
            error(current_eval_line, "Demasiados pares en el mapa");
        }
        val_map_set(&map, key, val);
        free(key);
        skip_ws_and_comments(p);
        if (p->pos < p->len && p->input[p->pos] == ',') p->pos++;
    }
    return map;
}

static Value parse_value(TextParser *p, int depth) {
    if (depth > DB_MAX_DEPTH)
        error(current_eval_line, "Demasiada profundidad en la deserialización");

    skip_ws_and_comments(p);
    if (p->pos >= p->len)
        error(current_eval_line, "Fin de entrada inesperado");

    char c = p->input[p->pos];

    if (c == '"') {
        char *str = NULL;
        if (parse_string(p, &str) != 0)
            error(current_eval_line, "Error al parsear string");
        Value v = val_string(str);
        free(str);
        return v;
    }

    if (c == '[') {
        p->pos++;
        if (p->pos < p->len && p->input[p->pos] == ']') {
            p->pos++;
            return val_list_empty();
        }
        size_t save = p->pos;
        skip_ws_and_comments(p);
        if (p->pos < p->len && p->input[p->pos] == ']') {
            p->pos++;
            return val_map_empty();
        }
        p->pos = save;

        if (looks_like_map(p))
            return parse_map(p, depth + 1);
        return parse_list(p, depth + 1);
    }

    if ((c == 't' || c == 'T') && strncasecmp(p->input + p->pos, "true", 4) == 0) {
        p->pos += 4;
        return val_bool(true);
    }
    if ((c == 'f' || c == 'F') && strncasecmp(p->input + p->pos, "false", 5) == 0) {
        p->pos += 5;
        return val_bool(false);
    }
    if ((c == 'n' || c == 'N') && strncasecmp(p->input + p->pos, "null", 4) == 0) {
        p->pos += 4;
        return val_make_null();
    }

    if (c == '-' || (c >= '0' && c <= '9')) {
        const char *start = p->input + p->pos;
        char *end;

        bool is_float = false;
        const char *scan = start;
        if (*scan == '-' || *scan == '+') scan++;
        while (*scan && isdigit((unsigned char)*scan)) scan++;
        if (*scan == '.' || *scan == 'e' || *scan == 'E') is_float = true;

        if (is_float) {
            errno = 0;
            double fval = strtod(start, &end);
            if (errno == ERANGE || end == start)
                error(current_eval_line, "Número flotante inválido");
            if (!isfinite(fval))
                error(current_eval_line, "Número flotante fuera de rango");
            p->pos = end - p->input;
            return val_float(fval);
        }

        errno = 0;
        long ival = strtol(start, &end, 10);
        if (errno == ERANGE || end == start)
            error(current_eval_line, "Número entero inválido");
        if (ival < INT_MIN || ival > INT_MAX) {
            errno = 0;
            double fval = strtod(start, &end);
            if (errno == ERANGE || end == start)
                error(current_eval_line, "Número fuera de rango");
            if (!isfinite(fval))
                error(current_eval_line, "Número flotante fuera de rango");
            p->pos = end - p->input;
            return val_float(fval);
        }
        p->pos = end - p->input;
        return val_int((int)ival);
    }

    error(current_eval_line, "Token inesperado en la entrada: '%c' (pos %zu)", c, p->pos);
    return val_make_null();
}

/* -----------------------------------------------------------------
 *  text_to_value_nowrap: parsea texto SIN capturar errores.
 *  El llamador debe envolver esta función en setjmp si quiere
 *  recuperarse de un error sin perder memoria.
 * ----------------------------------------------------------------- */
static Value text_to_value_nowrap(const char *text) {
    TextParser p;
    p.input = text;
    p.pos = 0;
    p.len = strlen(text);
    skip_ws_and_comments(&p);
    Value result = parse_value(&p, 0);
    skip_ws_and_comments(&p);
    if (p.pos < p.len) {
        value_free(&result);
        error(current_eval_line, "Sobran caracteres al final de la entrada");
    }
    return result;
}

/* -----------------------------------------------------------------
 *  parse_text_owned
 *
 *  Toma propiedad de `data` (buffer malloc'd). Si `text_to_value_nowrap`
 *  lanza un error via longjmp, este handler libera `data` antes de
 *  re-propagar el error. `data` es un parámetro y no se modifica entre
 *  setjmp y longjmp, por lo que su valor es válido en el handler.
 * ----------------------------------------------------------------- */
static Value parse_text_owned(char *data) {
    jmp_buf saved_env;
    memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
    int saved_raised = exception_raised;

    if (setjmp(exception_env) != 0) {
        free(data);
        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
        exception_raised = saved_raised;
        longjmp(exception_env, 1);
    }

    Value result = text_to_value_nowrap(data);
    free(data);

    memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
    exception_raised = saved_raised;
    return result;
}

/* ============================================================
 *  ByteBuffer para serialización binaria
 * ============================================================ */
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} ByteBuffer;

static void buf_init(ByteBuffer *b) {
    b->cap = 1024;
    b->data = malloc(b->cap);
    if (!b->data) error(current_eval_line, "Memoria insuficiente");
    b->len = 0;
}

static void buf_ensure(ByteBuffer *b, size_t needed) {
    if (b->len + needed > DB_MAX_SERIALIZED_SIZE)
        error(current_eval_line, "Serialización excede el límite de tamaño");
    if (b->len + needed <= b->cap) return;
    size_t new_cap = b->cap * 2;
    if (new_cap < b->len + needed)
        new_cap = b->len + needed;
    if (new_cap > DB_MAX_SERIALIZED_SIZE + 1)
        new_cap = DB_MAX_SERIALIZED_SIZE + 1;
    unsigned char *tmp = realloc(b->data, new_cap);
    if (!tmp) error(current_eval_line, "Memoria insuficiente");
    b->data = tmp;
    b->cap = new_cap;
}

static void buf_append_byte(ByteBuffer *b, unsigned char byte) {
    buf_ensure(b, 1);
    b->data[b->len++] = byte;
}

static void buf_append(ByteBuffer *b, const void *src, size_t n) {
    buf_ensure(b, n);
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void buf_append_u32_le(ByteBuffer *b, uint32_t value) {
    unsigned char bytes[4] = {
        (unsigned char)(value),
        (unsigned char)(value >> 8),
        (unsigned char)(value >> 16),
        (unsigned char)(value >> 24)
    };
    buf_append(b, bytes, sizeof(bytes));
}

static void buf_append_u64_le(ByteBuffer *b, uint64_t value) {
    unsigned char bytes[8] = {
        (unsigned char)(value),
        (unsigned char)(value >> 8),
        (unsigned char)(value >> 16),
        (unsigned char)(value >> 24),
        (unsigned char)(value >> 32),
        (unsigned char)(value >> 40),
        (unsigned char)(value >> 48),
        (unsigned char)(value >> 56)
    };
    buf_append(b, bytes, sizeof(bytes));
}

static uint32_t read_u32_le(const unsigned char *p) {
    return ((uint32_t)p[0]) |
    ((uint32_t)p[1] << 8) |
    ((uint32_t)p[2] << 16) |
    ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const unsigned char *p) {
    return ((uint64_t)p[0]) |
    ((uint64_t)p[1] << 8) |
    ((uint64_t)p[2] << 16) |
    ((uint64_t)p[3] << 24) |
    ((uint64_t)p[4] << 32) |
    ((uint64_t)p[5] << 40) |
    ((uint64_t)p[6] << 48) |
    ((uint64_t)p[7] << 56);
}

static void serialize_value_binary_rec(ByteBuffer *buf, Value v, int depth) {
    if (depth > DB_MAX_DEPTH)
        error(current_eval_line, "Demasiada profundidad en la serialización binaria");

    switch (v.type) {
        case VAL_INT: {
            buf_append_byte(buf, 'I');
            buf_append_u32_le(buf, (uint32_t)(int32_t)v.data.ival);
            break;
        }
        case VAL_FLOAT: {
            if (!isfinite(v.data.fval))
                error(current_eval_line, "No se puede serializar un flotante no finito");
            buf_append_byte(buf, 'F');
            uint64_t tmp;
            memcpy(&tmp, &v.data.fval, sizeof(double));
            buf_append_u64_le(buf, tmp);
            break;
        }
        case VAL_BOOL: {
            buf_append_byte(buf, 'B');
            buf_append_byte(buf, v.data.bval ? 1 : 0);
            break;
        }
        case VAL_STRING: {
            size_t str_len = strlen(v.data.sval);
            if (str_len > DB_MAX_STRING_LEN)
                error(current_eval_line, "String demasiado largo para serializar");
            buf_append_byte(buf, 'S');
            buf_append_u64_le(buf, (uint64_t)str_len);
            buf_append(buf, v.data.sval, str_len);
            break;
        }
        case VAL_LIST: {
            int count = v.data.list.count;
            if (count > DB_MAX_ITEMS)
                error(current_eval_line, "Lista demasiado grande para serializar");
            buf_append_byte(buf, 'L');
            buf_append_u32_le(buf, (uint32_t)count);
            for (int i = 0; i < count; i++) {
                serialize_value_binary_rec(buf, v.data.list.items[i], depth + 1);
            }
            break;
        }
        case VAL_MAP: {
            MapData *md = v.data.map;
            int count = md ? md->count : 0;
            if (count > DB_MAX_ITEMS)
                error(current_eval_line, "Mapa demasiado grande para serializar");
            buf_append_byte(buf, 'M');
            buf_append_u32_le(buf, (uint32_t)count);
            for (int i = 0; i < count; i++) {
                size_t key_len = strlen(md->pairs[i].key);
                if (key_len > DB_MAX_STRING_LEN)
                    error(current_eval_line, "Clave de mapa demasiado larga");
                buf_append_u64_le(buf, (uint64_t)key_len);
                buf_append(buf, md->pairs[i].key, key_len);
                serialize_value_binary_rec(buf, md->pairs[i].value, depth + 1);
            }
            break;
        }
        case VAL_NULL: {
            buf_append_byte(buf, 'N');
            break;
        }
        default:
            error(current_eval_line, "Tipo de dato no soportado para serialización binaria");
    }
}

static char *serialize_value_binary(Value v, size_t *out_len) {
    ByteBuffer buf;
    buf_init(&buf);
    serialize_value_binary_rec(&buf, v, 0);
    *out_len = buf.len;
    return (char*)buf.data;
}

static Value deserialize_value_binary(const unsigned char *buffer, size_t *offset,
                                      size_t total_len, int depth) {
    if (depth > DB_MAX_DEPTH)
        error(current_eval_line, "Demasiada profundidad en la deserialización binaria");
    if (*offset >= total_len)
        error(current_eval_line, "Buffer binario corrupto (offset excede límite)");

    unsigned char type = buffer[*offset];
    (*offset)++;

    switch (type) {
        case 'I': {
            if (sizeof(uint32_t) > total_len - *offset)
                error(current_eval_line, "Buffer corrupto (INT incompleto)");
            uint32_t bits = read_u32_le(buffer + *offset);
            *offset += sizeof(uint32_t);
            int32_t val;
            memcpy(&val, &bits, sizeof(val));
            return val_int((int)val);
        }
        case 'F': {
            if (sizeof(uint64_t) > total_len - *offset)
                error(current_eval_line, "Buffer corrupto (FLOAT incompleto)");
            uint64_t bits = read_u64_le(buffer + *offset);
            *offset += sizeof(uint64_t);
            double val;
            memcpy(&val, &bits, sizeof(val));
            if (!isfinite(val))
                error(current_eval_line, "El archivo contiene un flotante no finito");
            return val_float(val);
        }
        case 'B': {
            if (1 > total_len - *offset)
                error(current_eval_line, "Buffer corrupto (BOOL incompleto)");
            int bval = buffer[*offset];
            (*offset)++;
            return val_bool(bval != 0);
        }
        case 'S': {
            if (sizeof(uint64_t) > total_len - *offset)
                error(current_eval_line, "Buffer corrupto (STRING len incompleto)");
            uint64_t len64 = read_u64_le(buffer + *offset);
            *offset += sizeof(uint64_t);
            if (len64 > DB_MAX_STRING_LEN)
                error(current_eval_line, "String demasiado largo en el buffer");
            size_t str_len = (size_t)len64;
            if (str_len > total_len - *offset)
                error(current_eval_line, "Buffer corrupto (STRING data incompleto)");
            char *str = malloc(str_len + 1);
            if (!str) error(current_eval_line, "Memoria insuficiente en deserialización");
            memcpy(str, buffer + *offset, str_len);
            str[str_len] = '\0';
            *offset += str_len;
            Value res = val_string(str);
            free(str);
            return res;
        }
        case 'L': {
            if (sizeof(uint32_t) > total_len - *offset)
                error(current_eval_line, "Buffer corrupto (LIST count incompleto)");
            uint32_t count = read_u32_le(buffer + *offset);
            *offset += sizeof(uint32_t);
            if (count > DB_MAX_ITEMS)
                error(current_eval_line, "Número de elementos de lista inválido");
            Value list = val_list_empty();
            for (uint32_t i = 0; i < count; i++) {
                Value item = deserialize_value_binary(buffer, offset, total_len, depth + 1);
                val_list_append(&list, item);
            }
            return list;
        }
        case 'M': {
            if (sizeof(uint32_t) > total_len - *offset)
                error(current_eval_line, "Buffer corrupto (MAP count incompleto)");
            uint32_t count = read_u32_le(buffer + *offset);
            *offset += sizeof(uint32_t);
            if (count > DB_MAX_ITEMS)
                error(current_eval_line, "Número de pares de mapa inválido");
            Value map = val_map_empty();
            for (uint32_t i = 0; i < count; i++) {
                if (sizeof(uint64_t) > total_len - *offset)
                    error(current_eval_line, "Buffer corrupto (MAP key len incompleto)");
                uint64_t len64 = read_u64_le(buffer + *offset);
                *offset += sizeof(uint64_t);
                if (len64 > DB_MAX_STRING_LEN)
                    error(current_eval_line, "Clave de mapa demasiado larga");
                size_t key_len = (size_t)len64;
                if (key_len > total_len - *offset)
                    error(current_eval_line, "Buffer corrupto (MAP key data incompleto)");
                char *key = malloc(key_len + 1);
                if (!key) error(current_eval_line, "Memoria insuficiente");
                memcpy(key, buffer + *offset, key_len);
                key[key_len] = '\0';
                *offset += key_len;

                Value val = deserialize_value_binary(buffer, offset, total_len, depth + 1);
                val_map_set(&map, key, val);
                free(key);
            }
            return map;
        }
        case 'N': {
            return val_make_null();
        }
        default:
            error(current_eval_line, "Tipo desconocido en buffer binario: %c", type);
    }
    return val_make_null();
}

/* -----------------------------------------------------------------
 *  deserialize_binary_owned
 *
 *  Toma propiedad de `data`. Si algo falla durante la validación de la
 *  cabecera o la deserialización, libera `data` antes de propagar el
 *  error. `data` es un parámetro y no se modifica, por lo que su valor
 *  es válido en el handler.
 * ----------------------------------------------------------------- */
static Value deserialize_binary_owned(unsigned char *data, size_t data_len) {
    jmp_buf saved_env;
    memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
    int saved_raised = exception_raised;

    if (setjmp(exception_env) != 0) {
        free(data);
        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
        exception_raised = saved_raised;
        longjmp(exception_env, 1);
    }

    if (data_len < DB_BINARY_HEADER_SIZE)
        error(current_eval_line, "Archivo binario demasiado corto (falta cabecera)");
    if (memcmp(data, DB_BINARY_MAGIC, sizeof(DB_BINARY_MAGIC)) != 0)
        error(current_eval_line, "Archivo binario no reconocido (magic incorrecto)");
    if (data[sizeof(DB_BINARY_MAGIC)] != DB_BINARY_VERSION)
        error(current_eval_line, "Versión de formato binario no soportada: %u",
              (unsigned)data[sizeof(DB_BINARY_MAGIC)]);

    size_t offset = DB_BINARY_HEADER_SIZE;
    Value result = deserialize_value_binary(data, &offset, data_len, 0);
    if (offset != data_len) {
        value_free(&result);
        error(current_eval_line, "Datos binarios sobrantes después del valor");
    }

    free(data);

    memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
    exception_raised = saved_raised;
    return result;
}

/* ============================================================
 *  Escritura atómica
 * ============================================================ */
static void write_file_atomic(const char *path, const void *data, size_t len) {
    char *tmp_path = malloc(strlen(path) + 8);
    if (!tmp_path) error(current_eval_line, "Memoria insuficiente");
    snprintf(tmp_path, strlen(path) + 8, "%s.XXXXXX", path);

    int fd = mkstemp(tmp_path);
    if (fd == -1) {
        free(tmp_path);
        error(current_eval_line, "No se pudo crear archivo temporal seguro");
    }

    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(tmp_path);
        free(tmp_path);
        error(current_eval_line, "No se pudo abrir archivo temporal para escritura");
    }

    size_t written = fwrite(data, 1, len, f);
    if (written != len) {
        fclose(f);
        unlink(tmp_path);
        free(tmp_path);
        error(current_eval_line, "Error al escribir en archivo temporal");
    }
    if (fflush(f) != 0 || fsync(fd) != 0) {
        fclose(f);
        unlink(tmp_path);
        free(tmp_path);
        error(current_eval_line, "Error al sincronizar archivo temporal");
    }
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        free(tmp_path);
        error(current_eval_line, "Error al renombrar archivo temporal");
    }
    free(tmp_path);
}

/* ============================================================
 *  read_file_safe
 *
 *  Lee un archivo completo sin llamar nunca a error(). En caso de fallo
 *  devuelve NULL y escribe un mensaje legible en err_buf. Así el llamador
 *  puede liberar sus recursos antes de invocar error() y evitar tanto
 *  fugas como dobles free.
 * ============================================================ */
static char *read_file_safe(const char *path, size_t *out_len,
                            char *err_buf, size_t err_buf_size) {
    *out_len = 0;
    if (err_buf_size > 0) err_buf[0] = '\0';

    FILE *f = fopen(path, "rb");
    if (!f) {
        int saved_errno = errno;
        snprintf(err_buf, err_buf_size,
                 "No se pudo abrir el archivo '%s': %s",
                 path, strerror(saved_errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        int saved_errno = errno;
        fclose(f);
        snprintf(err_buf, err_buf_size,
                 "No se pudo obtener información del archivo '%s': %s",
                 path, strerror(saved_errno));
        return NULL;
    }
    if (st.st_size <= 0) {
        fclose(f);
        snprintf(err_buf, err_buf_size,
                 "El archivo '%s' está vacío", path);
        return NULL;
    }
    if ((uintmax_t)st.st_size > DB_MAX_FILE_SIZE) {
        fclose(f);
        snprintf(err_buf, err_buf_size,
                 "Archivo '%s' demasiado grande (máx %llu bytes)",
                 path, (unsigned long long)DB_MAX_FILE_SIZE);
        return NULL;
    }

    size_t size = (size_t)st.st_size;
    char *buffer = malloc(size + 1);
    if (!buffer) {
        fclose(f);
        snprintf(err_buf, err_buf_size,
                 "Memoria insuficiente al leer '%s'", path);
        return NULL;
    }
    size_t read = fread(buffer, 1, size, f);
    fclose(f);
    if (read != size) {
        free(buffer);
        snprintf(err_buf, err_buf_size,
                 "Error al leer el archivo '%s'", path);
        return NULL;
    }
    buffer[read] = '\0';
    *out_len = read;
    return buffer;
}

/* ============================================================
 *  Funciones built-in
 * ============================================================ */

static Value builtin_tofile(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "tofile() espera exactamente 2 argumentos");
    if (args[1].type != VAL_STRING) error(current_eval_line, "tofile() espera un string como segundo argumento");

    /* Verificamos el destino ANTES de asignar nada, para no tener que
     * limpiar en el error. */
    char *resolved = resolve_script_relative_path(args[1].data.sval);
    if (!resolved) error(current_eval_line, "Memoria insuficiente al resolver la ruta de destino");

    struct stat st;
    if (stat(resolved, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            free(resolved);
            error(current_eval_line,
                  "tofile(): el destino '%s' es un directorio, no un archivo",
                  args[1].data.sval);
        }
        if (!S_ISREG(st.st_mode)) {
            free(resolved);
            error(current_eval_line,
                  "tofile(): el destino '%s' no es un archivo regular",
                  args[1].data.sval);
        }
    } else if (errno != ENOENT) {
        int saved_errno = errno;
        char err_buf[512];
        snprintf(err_buf, sizeof(err_buf),
                 "tofile(): no se pudo verificar el destino '%s': %s",
                 args[1].data.sval, strerror(saved_errno));
        free(resolved);
        error(current_eval_line, "%s", err_buf);
    }

    /* A partir de aquí, `value_to_text` puede lanzar un error via longjmp.
     * Envolvemos el resto en setjmp con limpieza para no perder `resolved`
     * ni los buffers intermedios. Usamos `volatile` para que el compilador
     * no deje estos punteros en registros que longjmp no restauraría. */
    jmp_buf saved_env;
    memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
    int saved_raised = exception_raised;

    char *volatile text = NULL;
    char *volatile with_nl = NULL;

    if (setjmp(exception_env) != 0) {
        free(with_nl);
        free(text);
        free(resolved);
        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
        exception_raised = saved_raised;
        longjmp(exception_env, 1);
    }

    size_t data_len = 0;
    text = value_to_text(args[0], &data_len);
    if (!text) error(current_eval_line, "Error al serializar el valor a texto");

    with_nl = realloc(text, data_len + 2);
    if (!with_nl) error(current_eval_line, "Memoria insuficiente");
    text = NULL; /* la propiedad pasa a with_nl */
    with_nl[data_len] = '\n';
    with_nl[data_len + 1] = '\0';

    write_file_atomic(resolved, with_nl, data_len + 1);

    free(with_nl);
    free(resolved);

    memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
    exception_raised = saved_raised;
    return val_make_null();
}

/* -----------------------------------------------------------------
 *  builtin_fromfile
 *
 *  Lee un archivo de texto y lo deserializa. El archivo se lee con una
 *  función que NO llama a error(); así liberamos `resolved` antes de
 *  cualquier longjmp. La deserialización se hace con parse_text_owned,
 *  que toma propiedad del buffer y lo libera si falla.
 * ----------------------------------------------------------------- */
static Value builtin_fromfile(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "fromfile() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "fromfile() espera un string como argumento");

    char *resolved = resolve_script_relative_path(args[0].data.sval);
    if (!resolved) error(current_eval_line, "Memoria insuficiente al resolver la ruta");

    size_t data_len = 0;
    char err_buf[512];
    char *data = read_file_safe(resolved, &data_len, err_buf, sizeof(err_buf));
    free(resolved);
    resolved = NULL;

    if (!data) {
        /* read_file_safe nunca llamó a error(): podemos limpiar aquí con
         * tranquilidad antes de disparar el error al usuario. */
        error(current_eval_line, "%s", err_buf);
    }

    /* parse_text_owned toma propiedad de `data`. Si algo falla dentro,
     * liberará `data` antes de re-propagar el error. */
    return parse_text_owned(data);
}

static Value builtin_tobinfile(int argc, Value *args) {
    if (argc != 2) error(current_eval_line, "tobinfile() espera exactamente 2 argumentos");
    if (args[1].type != VAL_STRING) error(current_eval_line, "tobinfile() espera un string como segundo argumento");

    char *resolved = resolve_script_relative_path(args[1].data.sval);
    if (!resolved) error(current_eval_line, "Memoria insuficiente al resolver la ruta de destino");

    jmp_buf saved_env;
    memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
    int saved_raised = exception_raised;

    char *volatile serialized = NULL;
    unsigned char *volatile final_buffer = NULL;

    if (setjmp(exception_env) != 0) {
        free(final_buffer);
        free(serialized);
        free(resolved);
        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
        exception_raised = saved_raised;
        longjmp(exception_env, 1);
    }

    size_t data_len = 0;
    serialized = serialize_value_binary(args[0], &data_len);
    if (!serialized) error(current_eval_line, "Error al serializar el valor a binario");

    size_t total_len = DB_BINARY_HEADER_SIZE + data_len;
    if (total_len > DB_MAX_SERIALIZED_SIZE)
        error(current_eval_line, "Serialización con cabecera excede el límite de tamaño");

    final_buffer = malloc(total_len);
    if (!final_buffer) error(current_eval_line, "Memoria insuficiente");

    memcpy(final_buffer, DB_BINARY_MAGIC, sizeof(DB_BINARY_MAGIC));
    final_buffer[sizeof(DB_BINARY_MAGIC)] = DB_BINARY_VERSION;
    memcpy(final_buffer + DB_BINARY_HEADER_SIZE, serialized, data_len);
    free(serialized);
    serialized = NULL;

    write_file_atomic(resolved, final_buffer, total_len);

    free(final_buffer);
    free(resolved);

    memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
    exception_raised = saved_raised;
    return val_make_null();
}

/* -----------------------------------------------------------------
 *  builtin_frombinfile
 *
 *  Igual que builtin_fromfile pero para el formato binario. La lectura
 *  es segura (no longjmp) y la deserialización toma propiedad del buffer
 *  y lo libera en cualquier camino de error.
 * ----------------------------------------------------------------- */
static Value builtin_frombinfile(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "frombinfile() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "frombinfile() espera un string como argumento");

    char *resolved = resolve_script_relative_path(args[0].data.sval);
    if (!resolved) error(current_eval_line, "Memoria insuficiente al resolver la ruta");

    size_t data_len = 0;
    char err_buf[512];
    unsigned char *data = (unsigned char*)read_file_safe(resolved, &data_len, err_buf, sizeof(err_buf));
    free(resolved);
    resolved = NULL;

    if (!data) {
        error(current_eval_line, "%s", err_buf);
    }

    return deserialize_binary_owned(data, data_len);
}

static Value builtin_fileexists(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "fileexists() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "fileexists() espera un string como argumento");

    char *resolved = resolve_script_relative_path(args[0].data.sval);
    if (!resolved) error(current_eval_line, "Memoria insuficiente al resolver la ruta");

    struct stat st;
    int rc = stat(resolved, &st);
    free(resolved);
    if (rc != 0) return val_bool(false);
    return val_bool(S_ISREG(st.st_mode));
}

static Value builtin_deletefile(int argc, Value *args) {
    if (argc != 1) error(current_eval_line, "deletefile() espera exactamente 1 argumento");
    if (args[0].type != VAL_STRING) error(current_eval_line, "deletefile() espera un string como argumento");

    char *resolved = resolve_script_relative_path(args[0].data.sval);
    if (!resolved) error(current_eval_line, "Memoria insuficiente al resolver la ruta");

    int rc = unlink(resolved);
    free(resolved);
    return val_bool(rc == 0);
}

/* ============================================================
 *  Registro
 * ============================================================ */
void register_database_builtins(void) {
    func_register_builtin("tofile",       builtin_tofile);
    func_register_builtin("fromfile",     builtin_fromfile);
    func_register_builtin("tobinfile",    builtin_tobinfile);
    func_register_builtin("frombinfile",  builtin_frombinfile);
    func_register_builtin("fileexists",   builtin_fileexists);
    func_register_builtin("deletefile",   builtin_deletefile);

    vm_register_builtin("tofile",       builtin_tofile);
    vm_register_builtin("fromfile",     builtin_fromfile);
    vm_register_builtin("tobinfile",    builtin_tobinfile);
    vm_register_builtin("frombinfile",  builtin_frombinfile);
    vm_register_builtin("fileexists",   builtin_fileexists);
    vm_register_builtin("deletefile",   builtin_deletefile);
}
