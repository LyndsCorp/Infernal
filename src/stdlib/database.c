/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: stdlib/database.c
 *
 * Trabajar con datos persistentes en el sistema de archivos.
 * Perfecto para estudiar stdio.h :)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
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

/* ============================================================
 *  Cabecera mágica y versionado para formato binario
 * ============================================================ */
static const unsigned char DB_BINARY_MAGIC[4] = { 'I', 'N', 'D', 'B' };
#define DB_BINARY_VERSION 1
#define DB_BINARY_HEADER_SIZE (sizeof(DB_BINARY_MAGIC) + 1)  // magic + version byte

/* ============================================================
 *  Funciones auxiliares de buffer dinámico (para texto)
 * ============================================================ */
static int append_vprintf(char **buffer, size_t *cap, size_t *len,
                          const char *fmt, va_list args) {
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    if (needed < 0) return -1;

    // Comprobar overflow antes de sumar
    // Necesitamos espacio para los datos + 1 (para el NUL final)
    size_t needed_data = (size_t)needed;
    if (needed_data > DB_MAX_SERIALIZED_SIZE - *len) {
        errno = EFBIG;
        return -1;
    }
    size_t new_len = *len + needed_data;
    // Asegurar que cabe el NUL (new_len + 1)
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

                                                                            /* ============================================================
                                                                             *  Serialización a texto (recursiva)
                                                                             * ============================================================ */
                                                                            static int value_to_text_rec(Value v, char **out, size_t *cap, size_t *len,
                                                                                                         int depth) {
                                                                                if (depth > DB_MAX_DEPTH)
                                                                                    error(current_eval_line, "Demasiada profundidad en la serialización de datos");

                                                                                switch (v.type) {
                                                                                    case VAL_INT:
                                                                                        return append_printf(out, cap, len, "%d", v.data.ival);
                                                                                    case VAL_FLOAT: {
                                                                                        if (!isfinite(v.data.fval)) {
                                                                                            error(current_eval_line,
                                                                                                  "No se puede serializar un valor flotante no finito (NaN o Inf) a texto");
                                                                                        }
                                                                                        // Usamos %.17g para conservar la precisión completa de un double IEEE-754
                                                                                        return append_printf(out, cap, len, "%.17g", v.data.fval);
                                                                                    }
                                                                                    case VAL_BOOL:
                                                                                        return append_string(out, cap, len, v.data.bval ? "true" : "false");
                                                                                    case VAL_STRING: {
                                                                                        // Calcular longitud escapada para reservar memoria de una vez
                                                                                        size_t escaped_len = 2; // comillas
                                                                                        for (const char *p = v.data.sval; *p; p++) {
                                                                                            if (*p == '"' || *p == '\\')
                                                                                                escaped_len += 2;
                                                                                            else
                                                                                                escaped_len += 1;
                                                                                        }
                                                                                        if (escaped_len > DB_MAX_STRING_LEN + 2)
                                                                                            error(current_eval_line, "String demasiado largo para serializar");
                                                                                        char *escaped = malloc(escaped_len + 1);
                                                                                        if (!escaped) return -1;
                                                                                        char *dst = escaped;
                                                                                        *dst++ = '"';
                                                                                        for (const char *p = v.data.sval; *p; p++) {
                                                                                            if (*p == '"' || *p == '\\') {
                                                                                                *dst++ = '\\';
                                                                                                *dst++ = *p;
                                                                                            } else {
                                                                                                *dst++ = *p;
                                                                                            }
                                                                                        }
                                                                                        *dst++ = '"';
                                                                                        *dst = '\0';
                                                                                        int ret = append_string(out, cap, len, escaped);
                                                                                        free(escaped);
                                                                                        return ret;
                                                                                    }
                                                                                    case VAL_LIST: {
                                                                                        if (append_string(out, cap, len, "[") < 0) return -1;
                                                                                        for (int i = 0; i < v.data.list.count; i++) {
                                                                                            if (i > 0 && append_string(out, cap, len, ", ") < 0) return -1;
                                                                                            if (value_to_text_rec(v.data.list.items[i], out, cap, len, depth + 1) < 0)
                                                                                                return -1;
                                                                                        }
                                                                                        return append_string(out, cap, len, "]");
                                                                                    }
                                                                                    case VAL_MAP: {
                                                                                        if (append_string(out, cap, len, "{") < 0) return -1;
                                                                                        MapData *md = v.data.map;
                                                                                        for (int i = 0; i < md->count; i++) {
                                                                                            if (i > 0 && append_string(out, cap, len, ", ") < 0) return -1;
                                                                                            // clave
                                                                                            if (append_string(out, cap, len, "\"") < 0) return -1;
                                                                                            // Calcular longitud escapada de la clave
                                                                                            size_t escaped_len = 0;
                                                                                            for (const char *p = md->pairs[i].key; *p; p++) {
                                                                                                if (*p == '"' || *p == '\\')
                                                                                                    escaped_len += 2;
                                                                                                else
                                                                                                    escaped_len += 1;
                                                                                            }
                                                                                            if (escaped_len > DB_MAX_STRING_LEN)
                                                                                                error(current_eval_line, "Clave de mapa demasiado larga para serializar");
                                                                                            char *escaped = malloc(escaped_len + 1);
                                                                                            if (!escaped) return -1;
                                                                                            char *dst = escaped;
                                                                                            for (const char *p = md->pairs[i].key; *p; p++) {
                                                                                                if (*p == '"' || *p == '\\') {
                                                                                                    *dst++ = '\\';
                                                                                                    *dst++ = *p;
                                                                                                } else {
                                                                                                    *dst++ = *p;
                                                                                                }
                                                                                            }
                                                                                            *dst = '\0';
                                                                                            if (append_string(out, cap, len, escaped) < 0) {
                                                                                                free(escaped);
                                                                                                return -1;
                                                                                            }
                                                                                            free(escaped);
                                                                                            if (append_string(out, cap, len, "\": ") < 0) return -1;
                                                                                            if (value_to_text_rec(md->pairs[i].value, out, cap, len, depth + 1) < 0)
                                                                                                return -1;
                                                                                        }
                                                                                        return append_string(out, cap, len, "}");
                                                                                    }
                                                                                    case VAL_NULL:
                                                                                        return append_string(out, cap, len, "null");
                                                                                    default:
                                                                                        error(current_eval_line, "Tipo de dato no soportado para serialización a texto");
                                                                                }
                                                                                return -1;
                                                                                                         }

                                                                                                         static char *value_to_text(Value v, size_t *out_len) {
                                                                                                             char *buffer = malloc(1024);
                                                                                                             if (!buffer) return NULL;
                                                                                                             size_t cap = 1024, len = 0;
                                                                                                             if (value_to_text_rec(v, &buffer, &cap, &len, 0) < 0) {
                                                                                                                 free(buffer);
                                                                                                                 return NULL;
                                                                                                             }
                                                                                                             buffer[len] = '\0';
                                                                                                             *out_len = len;
                                                                                                             return buffer;
                                                                                                         }

                                                                                                         /* ============================================================
                                                                                                          *  Deserialización de texto (parser recursivo)
                                                                                                          * ============================================================ */
                                                                                                         typedef struct {
                                                                                                             const char *input;
                                                                                                             size_t pos;
                                                                                                             size_t len;
                                                                                                         } TextParser;

                                                                                                         static void skip_spaces(TextParser *p) {
                                                                                                             while (p->pos < p->len && (p->input[p->pos] == ' ' || p->input[p->pos] == '\t' ||
                                                                                                                 p->input[p->pos] == '\n' || p->input[p->pos] == '\r'))
                                                                                                                 p->pos++;
                                                                                                         }

                                                                                                         static int parse_string(TextParser *p, char **out) {
                                                                                                             if (p->pos >= p->len || p->input[p->pos] != '"')
                                                                                                                 return -1;
                                                                                                             p->pos++; // saltar comilla inicial
                                                                                                             size_t start = p->pos;
                                                                                                             while (p->pos < p->len && p->input[p->pos] != '"') {
                                                                                                                 if (p->input[p->pos] == '\\' && p->pos + 1 < p->len) {
                                                                                                                     p->pos += 2; // saltar escapado
                                                                                                                 } else {
                                                                                                                     p->pos++;
                                                                                                                 }
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
                                                                                                                         case 'n': result[out_i++] = '\n'; break;
                                                                                                                         case 't': result[out_i++] = '\t'; break;
                                                                                                                         case 'r': result[out_i++] = '\r'; break;
                                                                                                                         case '\\': result[out_i++] = '\\'; break;
                                                                                                                         case '"': result[out_i++] = '"'; break;
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
                                                                                                             p->pos++; // saltar comilla final
                                                                                                             return 0;
                                                                                                         }

                                                                                                         static Value parse_value(TextParser *p, int depth);

                                                                                                         static Value parse_list(TextParser *p, int depth) {
                                                                                                             if (depth > DB_MAX_DEPTH)
                                                                                                                 error(current_eval_line, "Demasiada profundidad en la deserialización");
                                                                                                             if (p->pos >= p->len || p->input[p->pos] != '[')
                                                                                                                 error(current_eval_line, "Se esperaba '[' en lista");
                                                                                                             p->pos++;
                                                                                                             skip_spaces(p);
                                                                                                             Value list = val_list_empty();
                                                                                                             if (p->pos < p->len && p->input[p->pos] == ']') {
                                                                                                                 p->pos++;
                                                                                                                 return list;
                                                                                                             }
                                                                                                             while (1) {
                                                                                                                 skip_spaces(p);
                                                                                                                 Value item = parse_value(p, depth + 1);
                                                                                                                 if (list.data.list.count >= DB_MAX_ITEMS)
                                                                                                                     error(current_eval_line, "Demasiados elementos en la lista");
                                                                                                                 val_list_append(&list, item);
                                                                                                                 skip_spaces(p);
                                                                                                                 if (p->pos >= p->len) error(current_eval_line, "Lista no cerrada correctamente");
                                                                                                                 if (p->input[p->pos] == ',') {
                                                                                                                     p->pos++;
                                                                                                                     skip_spaces(p);
                                                                                                                     continue;
                                                                                                                 }
                                                                                                                 if (p->input[p->pos] == ']') {
                                                                                                                     p->pos++;
                                                                                                                     break;
                                                                                                                 }
                                                                                                                 error(current_eval_line, "Se esperaba ',' o ']' en lista");
                                                                                                             }
                                                                                                             return list;
                                                                                                         }

                                                                                                         static Value parse_map(TextParser *p, int depth) {
                                                                                                             if (depth > DB_MAX_DEPTH) {
                                                                                                                 error(current_eval_line, "Demasiada profundidad en la deserialización");
                                                                                                             }
                                                                                                             if (p->pos >= p->len || p->input[p->pos] != '{') {
                                                                                                                 error(current_eval_line, "Se esperaba '{' en mapa");
                                                                                                             }
                                                                                                             p->pos++;
                                                                                                             skip_spaces(p);
                                                                                                             Value map = val_map_empty();
                                                                                                             if (p->pos < p->len && p->input[p->pos] == '}') {
                                                                                                                 p->pos++;
                                                                                                                 return map;
                                                                                                             }
                                                                                                             while (1) {
                                                                                                                 skip_spaces(p);
                                                                                                                 char *key = NULL;
                                                                                                                 if (parse_string(p, &key) != 0)
                                                                                                                     error(current_eval_line, "Se esperaba string como clave del mapa");
                                                                                                                 skip_spaces(p);
                                                                                                                 if (p->pos >= p->len || p->input[p->pos] != ':')
                                                                                                                     error(current_eval_line, "Se esperaba ':' después de la clave");
                                                                                                                 p->pos++;
                                                                                                                 skip_spaces(p);
                                                                                                                 Value val = parse_value(p, depth + 1);
                                                                                                                 if (map.data.map->count >= DB_MAX_ITEMS)
                                                                                                                     error(current_eval_line, "Demasiados pares en el mapa");
                                                                                                                 val_map_set(&map, key, val);
                                                                                                                 free(key);
                                                                                                                 skip_spaces(p);
                                                                                                                 if (p->pos >= p->len) error(current_eval_line, "Mapa no cerrado correctamente");
                                                                                                                 if (p->input[p->pos] == ',') {
                                                                                                                     p->pos++;
                                                                                                                     skip_spaces(p);
                                                                                                                     continue;
                                                                                                                 }
                                                                                                                 if (p->input[p->pos] == '}') {
                                                                                                                     p->pos++;
                                                                                                                     break;
                                                                                                                 }
                                                                                                                 error(current_eval_line, "Se esperaba ',' o '}' en mapa");
                                                                                                             }
                                                                                                             return map;
                                                                                                         }

                                                                                                         static Value parse_value(TextParser *p, int depth) {
                                                                                                             skip_spaces(p);
                                                                                                             if (p->pos >= p->len) error(current_eval_line, "Fin de entrada inesperado");

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
                                                                                                                 return parse_list(p, depth);
                                                                                                             }
                                                                                                             if (c == '{') {
                                                                                                                 return parse_map(p, depth);
                                                                                                             }
                                                                                                             if (c == 't' && strncmp(p->input + p->pos, "true", 4) == 0) {
                                                                                                                 p->pos += 4;
                                                                                                                 return val_bool(true);
                                                                                                             }
                                                                                                             if (c == 'f' && strncmp(p->input + p->pos, "false", 5) == 0) {
                                                                                                                 p->pos += 5;
                                                                                                                 return val_bool(false);
                                                                                                             }
                                                                                                             if (c == 'n' && strncmp(p->input + p->pos, "null", 4) == 0) {
                                                                                                                 p->pos += 4;
                                                                                                                 return val_make_null();
                                                                                                             }
                                                                                                             if (c == '-' || (c >= '0' && c <= '9')) {
                                                                                                                 char *end;
                                                                                                                 errno = 0;
                                                                                                                 long ival = strtol(p->input + p->pos, &end, 10);
                                                                                                                 if (errno == ERANGE || ival < INT_MIN || ival > INT_MAX) {
                                                                                                                     // probar float
                                                                                                                     errno = 0;   // Resetear antes de strtod
                                                                                                                     double fval = strtod(p->input + p->pos, &end);
                                                                                                                     if (errno == ERANGE || end == p->input + p->pos) {
                                                                                                                         error(current_eval_line, "Número fuera de rango o mal formado");
                                                                                                                     }
                                                                                                                     if (!isfinite(fval))
                                                                                                                         error(current_eval_line, "Número flotante fuera de rango (infinito)");
                                                                                                                     p->pos = end - p->input;
                                                                                                                     return val_float(fval);
                                                                                                                 }
                                                                                                                 if (end == p->input + p->pos) {
                                                                                                                     // probar float (si no se consumió nada como entero)
                                                                                                                     errno = 0;
                                                                                                                     double fval = strtod(p->input + p->pos, &end);
                                                                                                                     if (errno == ERANGE || end == p->input + p->pos)
                                                                                                                         error(current_eval_line, "Número mal formado");
                                                                                                                     if (!isfinite(fval))
                                                                                                                         error(current_eval_line, "Número flotante fuera de rango (infinito)");
                                                                                                                     p->pos = end - p->input;
                                                                                                                     return val_float(fval);
                                                                                                                 }
                                                                                                                 // Si hay punto o 'e', es float
                                                                                                                 const char *after = end;
                                                                                                                 if (*after == '.' || *after == 'e' || *after == 'E') {
                                                                                                                     errno = 0;
                                                                                                                     double fval = strtod(p->input + p->pos, &end);
                                                                                                                     if (errno == ERANGE || end == p->input + p->pos)
                                                                                                                         error(current_eval_line, "Número flotante mal formado o fuera de rango");
                                                                                                                     if (!isfinite(fval))
                                                                                                                         error(current_eval_line, "Número flotante fuera de rango (infinito)");
                                                                                                                     p->pos = end - p->input;
                                                                                                                     return val_float(fval);
                                                                                                                 }
                                                                                                                 p->pos = end - p->input;
                                                                                                                 return val_int((int)ival);
                                                                                                             }
                                                                                                             error(current_eval_line, "Token inesperado en la entrada: '%c' (pos %zu)", c, p->pos);
                                                                                                             return val_make_null();
                                                                                                         }

                                                                                                         static Value text_to_value(const char *text) {
                                                                                                             TextParser p;
                                                                                                             p.input = text;
                                                                                                             p.pos = 0;
                                                                                                             p.len = strlen(text);
                                                                                                             skip_spaces(&p);
                                                                                                             Value result = parse_value(&p, 0);
                                                                                                             skip_spaces(&p);
                                                                                                             if (p.pos < p.len)
                                                                                                                 error(current_eval_line, "Sobran caracteres al final de la entrada");
                                                                                                             return result;
                                                                                                         }

                                                                                                         /* ============================================================
                                                                                                          *  ByteBuffer para serialización binaria eficiente
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
                                                                                                             // Necesitamos espacio para los datos + 1 (por si acaso, aunque no se usa directamente)
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

                                                                                                         /* ============================================================
                                                                                                          *  Funciones de escritura/lectura de números little-endian (portable)
                                                                                                          * ============================================================ */
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

                                                                                                         /* ============================================================
                                                                                                          *  Serialización binaria portable (con cabecera y versión)
                                                                                                          * ============================================================ */
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
                                                                                                                         error(current_eval_line, "No se puede serializar un flotante no finito (NaN/Inf)");
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
                                                                                                                     int count = md->count;
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

                                                                                                         static Value deserialize_value_binary(const unsigned char *buffer, size_t *offset, size_t total_len, int depth) {
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
                                                                                                                         error(current_eval_line, "El archivo contiene un valor flotante no finito");
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
                                                                                                                         error(current_eval_line, "Número de elementos de lista inválido o demasiado grande");
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
                                                                                                                         error(current_eval_line, "Número de pares de mapa inválido o demasiado grande");
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

                                                                                                         /* ============================================================
                                                                                                          *  Funciones de escritura/lectura atómicas y seguras
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

                                                                                                         static char *read_file(const char *path, size_t *out_len) {
                                                                                                             FILE *f = fopen(path, "rb");
                                                                                                             if (!f)
                                                                                                                 error(current_eval_line, "No se pudo abrir el archivo para lectura");

                                                                                                             struct stat st;
                                                                                                             if (fstat(fileno(f), &st) != 0) {
                                                                                                                 fclose(f);
                                                                                                                 error(current_eval_line, "No se pudo obtener información del archivo");
                                                                                                             }
                                                                                                             if (st.st_size < 0) {
                                                                                                                 fclose(f);
                                                                                                                 error(current_eval_line, "Tamaño de archivo inválido");
                                                                                                             }
                                                                                                             off_t file_size = st.st_size;
                                                                                                             if ((uintmax_t)file_size > DB_MAX_FILE_SIZE) {
                                                                                                                 fclose(f);
                                                                                                                 error(current_eval_line, "Archivo demasiado grande (máx %llu bytes)",
                                                                                                                       (unsigned long long)DB_MAX_FILE_SIZE);
                                                                                                             }
                                                                                                             if (file_size == 0) {
                                                                                                                 fclose(f);
                                                                                                                 error(current_eval_line, "El archivo está vacío");
                                                                                                             }

                                                                                                             size_t size = (size_t)file_size;
                                                                                                             char *buffer = malloc(size + 1);
                                                                                                             if (!buffer) {
                                                                                                                 fclose(f);
                                                                                                                 error(current_eval_line, "Memoria insuficiente");
                                                                                                             }
                                                                                                             size_t read = fread(buffer, 1, size, f);
                                                                                                             fclose(f);
                                                                                                             if (read != size) {
                                                                                                                 free(buffer);
                                                                                                                 error(current_eval_line, "Error al leer el archivo");
                                                                                                             }
                                                                                                             buffer[read] = '\0';   // Terminador NUL, imprescindible para texto, inofensivo para binario
                                                                                                             *out_len = read;
                                                                                                             return buffer;
                                                                                                         }

                                                                                                         /* ============================================================
                                                                                                          *  Funciones built-in
                                                                                                          * ============================================================ */

                                                                                                         static Value builtin_tofile(int argc, Value *args) {
                                                                                                             if (argc != 2) error(current_eval_line, "tofile() espera exactamente 2 argumentos");
                                                                                                             if (args[1].type != VAL_STRING) error(current_eval_line, "tofile() espera un string como segundo argumento");

                                                                                                             size_t data_len;
                                                                                                             char *text = value_to_text(args[0], &data_len);
                                                                                                             if (!text) error(current_eval_line, "Error al serializar el valor a texto");

                                                                                                             write_file_atomic(args[1].data.sval, text, data_len);
                                                                                                             free(text);
                                                                                                             return val_make_null();
                                                                                                         }

                                                                                                         static Value builtin_fromfile(int argc, Value *args) {
                                                                                                             if (argc != 1) error(current_eval_line, "fromfile() espera exactamente 1 argumento");
                                                                                                             if (args[0].type != VAL_STRING) error(current_eval_line, "fromfile() espera un string como argumento");

                                                                                                             size_t data_len;
                                                                                                             char *data = read_file(args[0].data.sval, &data_len);
                                                                                                             Value result = text_to_value(data);
                                                                                                             free(data);
                                                                                                             return result;
                                                                                                         }

                                                                                                         static Value builtin_tobinfile(int argc, Value *args) {
                                                                                                             if (argc != 2) error(current_eval_line, "tobinfile() espera exactamente 2 argumentos");
                                                                                                             if (args[1].type != VAL_STRING) error(current_eval_line, "tobinfile() espera un string como segundo argumento");

                                                                                                             size_t data_len;
                                                                                                             char *serialized = serialize_value_binary(args[0], &data_len);
                                                                                                             if (!serialized) error(current_eval_line, "Error al serializar el valor a binario");

                                                                                                             // Añadir cabecera mágica + versión
                                                                                                             size_t total_len = DB_BINARY_HEADER_SIZE + data_len;
                                                                                                             if (total_len > DB_MAX_SERIALIZED_SIZE) {
                                                                                                                 free(serialized);
                                                                                                                 error(current_eval_line, "Serialización con cabecera excede el límite de tamaño");
                                                                                                             }
                                                                                                             unsigned char *final_buffer = malloc(total_len);
                                                                                                             if (!final_buffer) {
                                                                                                                 free(serialized);
                                                                                                                 error(current_eval_line, "Memoria insuficiente");
                                                                                                             }
                                                                                                             memcpy(final_buffer, DB_BINARY_MAGIC, sizeof(DB_BINARY_MAGIC));
                                                                                                             final_buffer[sizeof(DB_BINARY_MAGIC)] = DB_BINARY_VERSION;
                                                                                                             memcpy(final_buffer + DB_BINARY_HEADER_SIZE, serialized, data_len);
                                                                                                             free(serialized);

                                                                                                             write_file_atomic(args[1].data.sval, final_buffer, total_len);
                                                                                                             free(final_buffer);
                                                                                                             return val_make_null();
                                                                                                         }

                                                                                                         static Value builtin_frombinfile(int argc, Value *args) {
                                                                                                             if (argc != 1) error(current_eval_line, "frombinfile() espera exactamente 1 argumento");
                                                                                                             if (args[0].type != VAL_STRING) error(current_eval_line, "frombinfile() espera un string como argumento");

                                                                                                             size_t data_len;
                                                                                                             unsigned char *data = (unsigned char*)read_file(args[0].data.sval, &data_len);
                                                                                                             if (data_len < DB_BINARY_HEADER_SIZE) {
                                                                                                                 free(data);
                                                                                                                 error(current_eval_line, "Archivo binario demasiado corto (falta cabecera)");
                                                                                                             }
                                                                                                             if (memcmp(data, DB_BINARY_MAGIC, sizeof(DB_BINARY_MAGIC)) != 0) {
                                                                                                                 free(data);
                                                                                                                 error(current_eval_line, "Archivo binario no reconocido (magic incorrecto)");
                                                                                                             }
                                                                                                             unsigned char version = data[sizeof(DB_BINARY_MAGIC)];
                                                                                                             if (version != DB_BINARY_VERSION) {
                                                                                                                 free(data);
                                                                                                                 error(current_eval_line, "Versión de formato binario no soportada: %u (esperada %u)",
                                                                                                                       version, DB_BINARY_VERSION);
                                                                                                             }

                                                                                                             size_t offset = DB_BINARY_HEADER_SIZE;
                                                                                                             Value result = deserialize_value_binary(data, &offset, data_len, 0);
                                                                                                             if (offset != data_len) {
                                                                                                                 free(data);
                                                                                                                 error(current_eval_line, "Datos binarios sobrantes después del valor");
                                                                                                             }
                                                                                                             free(data);
                                                                                                             return result;
                                                                                                         }

                                                                                                         static Value builtin_fileexists(int argc, Value *args) {
                                                                                                             if (argc != 1) error(current_eval_line, "fileexists() espera exactamente 1 argumento");
                                                                                                             if (args[0].type != VAL_STRING) error(current_eval_line, "fileexists() espera un string como argumento");

                                                                                                             struct stat st;
                                                                                                             if (stat(args[0].data.sval, &st) != 0)
                                                                                                                 return val_bool(false);
                                                                                                             return val_bool(S_ISREG(st.st_mode));
                                                                                                         }

                                                                                                         static Value builtin_deletefile(int argc, Value *args) {
                                                                                                             if (argc != 1) error(current_eval_line, "deletefile() espera exactamente 1 argumento");
                                                                                                             if (args[0].type != VAL_STRING) error(current_eval_line, "deletefile() espera un string como argumento");

                                                                                                             const char *path = args[0].data.sval;
                                                                                                             int ret = unlink(path);
                                                                                                             if (ret == 0) return val_bool(true);
                                                                                                             // Si falla por ENOENT, devolvemos false; otros errores también false.
                                                                                                             return val_bool(false);
                                                                                                         }

                                                                                                         /* ============================================================
                                                                                                          *  Registro
                                                                                                          * ============================================================ */
                                                                                                         void register_database_builtins(void) {
                                                                                                             func_register_builtin("tofile", builtin_tofile);
                                                                                                             func_register_builtin("fromfile", builtin_fromfile);
                                                                                                             func_register_builtin("tobinfile", builtin_tobinfile);
                                                                                                             func_register_builtin("frombinfile", builtin_frombinfile);
                                                                                                             func_register_builtin("fileexists", builtin_fileexists);
                                                                                                             func_register_builtin("deletefile", builtin_deletefile);

                                                                                                             vm_register_builtin("tofile", builtin_tofile);
                                                                                                             vm_register_builtin("fromfile", builtin_fromfile);
                                                                                                             vm_register_builtin("tobinfile", builtin_tobinfile);
                                                                                                             vm_register_builtin("frombinfile", builtin_frombinfile);
                                                                                                             vm_register_builtin("fileexists", builtin_fileexists);
                                                                                                             vm_register_builtin("deletefile", builtin_deletefile);
                                                                                                         }
