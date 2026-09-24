/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Apache 2.0 — Código fuente de Infernal: runtime/lava.c
 *
 * Runtime de librerías Lava.
*/

#include "runtime/lava.h"

#include "core/value.h"
#include "core/types.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "vm/vm.h"

#include <dlfcn.h>
#include <ffi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>

typedef Value lava_list;

#ifndef PATH_MAX
#  define PATH_MAX 4096
#endif

#define LAVA_MAX_SLOTS 256
#define LAVA_ARG_SLOT  128   /* bytes por argumento en el storage de libffi */

/* ==================================================================
 * Estado global
 * ================================================================== */

typedef struct LavaEntry {
    char       *name;
    char       *module_path;
    void      (*target_fn)(void);
    char       *signature;
    int         nargs;
    ffi_type  **arg_types;
    ffi_cif     cif;
} LavaEntry;

static LavaEntry *g_slots[LAVA_MAX_SLOTS];
static int        g_slot_count = 0;

typedef struct LavaHandle {
    char *path;
    void *handle;
    struct LavaHandle *next;
} LavaHandle;

static LavaHandle *g_handles = NULL;

/* Estado durante la carga de un módulo (para lava_register_fn). */
static const char *g_loading_module_path = NULL;
static const char *g_loading_prefix      = NULL;

/* Contexto activo durante la llamada a una función Lava. */
typedef struct {
    bool   type_set;
    int    type;      /* TOK_INT, TOK_FLOAT, TOK_STRING, TOK_BOOL, TOK_LIST */
    bool   value_set;
    Value  value;
    int    argc;
    Value *args;
} LavaCtx;

static LavaCtx *g_ctx = NULL;

/* ==================================================================
 * Conversión Value -> C
 * ================================================================== */

static int value_to_c_int(Value v) {
    switch (v.type) {
        case VAL_INT:    return v.data.ival;
        case VAL_FLOAT:  return (int)v.data.fval;
        case VAL_BOOL:   return v.data.bval ? 1 : 0;
        case VAL_STRING: return (int)strtol(v.data.sval ? v.data.sval : "0", NULL, 10);
        default:         return 0;
    }
}

static double value_to_c_double(Value v) {
    switch (v.type) {
        case VAL_INT:    return (double)v.data.ival;
        case VAL_FLOAT:  return v.data.fval;
        case VAL_BOOL:   return v.data.bval ? 1.0 : 0.0;
        case VAL_STRING: return v.data.sval ? atof(v.data.sval) : 0.0;
        default:         return 0.0;
    }
}

/* ==================================================================
 * API pública (llamada por el .lava)
 * ================================================================== */

void infernal_return_type(const char *type) {
    if (!g_ctx) {
        fprintf(stderr,
                "Error Lava: infernal_return_type() fuera de una función Lava\n");
        abort();
    }
    if (g_ctx->type_set) {
        fprintf(stderr, "Error Lava: tipo de retorno ya fijado\n");
        abort();
    }
    if (!type || !*type) {
        fprintf(stderr, "Error Lava: infernal_return_type() con tipo vacío\n");
        abort();
    }
    int t = 0;
    if      (strcmp(type, "int")    == 0) t = TOK_INT;
    else if (strcmp(type, "float")  == 0) t = TOK_FLOAT;
    else if (strcmp(type, "string") == 0) t = TOK_STRING;
    else if (strcmp(type, "bool")   == 0) t = TOK_BOOL;
    else if (strcmp(type, "list")   == 0) t = TOK_LIST;
    else {
        fprintf(stderr,
                "Error Lava: tipo inválido '%s'. "
                "Usa \"int\", \"float\", \"string\", \"bool\" o \"list\".\n", type);
        abort();
    }
    g_ctx->type     = t;
    g_ctx->type_set = true;
}

void infernal_return_value(const char *fmt, ...) {
    (void)fmt;
    if (!g_ctx) {
        fprintf(stderr,
                "Error Lava: infernal_return_value() fuera de una función Lava\n");
        abort();
    }
    if (!g_ctx->type_set) {
        fprintf(stderr,
                "Error Lava: infernal_return_value() antes que "
                "infernal_return_type()\n");
        abort();
    }
    if (g_ctx->value_set) {
        fprintf(stderr, "Error Lava: valor de retorno ya fijado\n");
        abort();
    }

    va_list ap;
    va_start(ap, fmt);
    switch (g_ctx->type) {
        case TOK_INT:    g_ctx->value = val_int(va_arg(ap, int));            break;
        case TOK_FLOAT:  g_ctx->value = val_float(va_arg(ap, double));       break;
        case TOK_BOOL:   g_ctx->value = val_bool(va_arg(ap, int) != 0);      break;
        case TOK_STRING: {
            const char *s = va_arg(ap, const char *);
            g_ctx->value = val_string(s ? s : "");
            break;
        }
        case TOK_LIST:
            /* Para listas usa infernal_return_list(); si alguien llega aquí
             * con TOK_LIST, devolvemos lista vacía. */
            g_ctx->value = val_list_empty();
            break;
        default:
            g_ctx->value = val_make_null();
            break;
    }
    va_end(ap);
    g_ctx->value_set = true;
}

int lava_argc(void) { return g_ctx ? g_ctx->argc : 0; }

static Value lava_arg_at(int index) {
    if (!g_ctx || index < 0 || index >= g_ctx->argc) {
        fprintf(stderr, "Error Lava: índice %d fuera de rango\n", index);
        abort();
    }
    return g_ctx->args[index];
}

int         lava_arg_int   (int i) { return value_to_c_int(lava_arg_at(i)); }
double      lava_arg_float (int i) { return value_to_c_double(lava_arg_at(i)); }

int lava_arg_bool(int i) {
    Value v = lava_arg_at(i);
    switch (v.type) {
        case VAL_INT:    return v.data.ival != 0;
        case VAL_FLOAT:  return v.data.fval != 0.0;
        case VAL_BOOL:   return v.data.bval ? 1 : 0;
        case VAL_STRING: return v.data.sval && v.data.sval[0];
        default:         return 0;
    }
}

const char *lava_arg_string(int i) {
    Value v = lava_arg_at(i);
    if (v.type == VAL_STRING) return v.data.sval ? v.data.sval : "";

    static char bufs[8][64];
    static int  slot = 0;
    char *buf = bufs[slot];
    slot = (slot + 1) & 7;
    switch (v.type) {
        case VAL_INT:   snprintf(buf, 64, "%d",  v.data.ival); break;
        case VAL_FLOAT: snprintf(buf, 64, "%g",  v.data.fval); break;
        case VAL_BOOL:  snprintf(buf, 64, "%s",  v.data.bval ? "true" : "false"); break;
        default:        buf[0] = '\0'; break;
    }
    return buf;
}

/* ==================================================================
 * API de listas
 * ================================================================== */

lava_list *lava_arg_list(int index) {
    if (!g_ctx || index < 0 || index >= g_ctx->argc) return NULL;
    Value *v = &g_ctx->args[index];
    if (v->type != VAL_LIST) return NULL;
    return (lava_list *)v;
}

int lava_list_len(lava_list *l) {
    if (!l) return 0;
    Value *v = (Value *)l;
    if (v->type != VAL_LIST) return 0;
    return v->data.list.count;
}

const char *lava_list_element_type(lava_list *l, int index) {
    if (!l) return NULL;
    Value *v = (Value *)l;
    if (v->type != VAL_LIST) return NULL;
    if (index < 1 || index > v->data.list.count) return NULL;
    switch (v->data.list.items[index - 1].type) {
        case VAL_NULL:   return "null";
        case VAL_INT:    return "int";
        case VAL_FLOAT:  return "float";
        case VAL_BOOL:   return "bool";
        case VAL_STRING: return "string";
        case VAL_LIST:   return "list";
        case VAL_MAP:    return "map";
        default:         return NULL;
    }
}

int lava_list_int(lava_list *l, int index) {
    if (!l) return 0;
    Value *v = (Value *)l;
    if (v->type != VAL_LIST) return 0;
    if (index < 1 || index > v->data.list.count) return 0;
    Value item = v->data.list.items[index - 1];
    switch (item.type) {
        case VAL_INT:    return item.data.ival;
        case VAL_FLOAT:  return (int)item.data.fval;
        case VAL_BOOL:   return item.data.bval ? 1 : 0;
        case VAL_STRING: return (int)strtol(item.data.sval ? item.data.sval : "0", NULL, 10);
        default:         return 0;
    }
}

double lava_list_float(lava_list *l, int index) {
    if (!l) return 0.0;
    Value *v = (Value *)l;
    if (v->type != VAL_LIST) return 0.0;
    if (index < 1 || index > v->data.list.count) return 0.0;
    Value item = v->data.list.items[index - 1];
    switch (item.type) {
        case VAL_INT:    return (double)item.data.ival;
        case VAL_FLOAT:  return item.data.fval;
        case VAL_BOOL:   return item.data.bval ? 1.0 : 0.0;
        case VAL_STRING: return item.data.sval ? atof(item.data.sval) : 0.0;
        default:         return 0.0;
    }
}

const char *lava_list_string(lava_list *l, int index) {
    if (!l) return NULL;
    Value *v = (Value *)l;
    if (v->type != VAL_LIST) return NULL;
    if (index < 1 || index > v->data.list.count) return NULL;
    Value item = v->data.list.items[index - 1];
    if (item.type == VAL_STRING)
        return item.data.sval ? item.data.sval : "";

    static char bufs[4][64];
    static int  slot = 0;
    char *buf = bufs[slot];
    slot = (slot + 1) & 3;
    switch (item.type) {
        case VAL_INT:   snprintf(buf, 64, "%d", item.data.ival); break;
        case VAL_FLOAT: snprintf(buf, 64, "%g", item.data.fval); break;
        case VAL_BOOL:  snprintf(buf, 64, "%s", item.data.bval ? "true" : "false"); break;
        default:        buf[0] = '\0'; break;
    }
    return buf;
}

int lava_list_bool(lava_list *l, int index) {
    if (!l) return 0;
    Value *v = (Value *)l;
    if (v->type != VAL_LIST) return 0;
    if (index < 1 || index > v->data.list.count) return 0;
    Value item = v->data.list.items[index - 1];
    switch (item.type) {
        case VAL_BOOL:   return item.data.bval ? 1 : 0;
        case VAL_INT:    return item.data.ival != 0;
        case VAL_FLOAT:  return item.data.fval != 0.0;
        case VAL_STRING: return item.data.sval && item.data.sval[0];
        default:         return 0;
    }
}

lava_list *lava_list_list(lava_list *l, int index) {
    if (!l) return NULL;
    Value *v = (Value *)l;
    if (v->type != VAL_LIST) return NULL;
    if (index < 1 || index > v->data.list.count) return NULL;
    Value *item = &v->data.list.items[index - 1];
    if (item->type != VAL_LIST) return NULL;
    return (lava_list *)item;   /* prestada, vive dentro del padre */
}

lava_list *lava_list_create(void) {
    Value *v = malloc(sizeof(Value));
    if (!v) return NULL;
    *v = val_list_empty();
    return (lava_list *)v;
}

void lava_list_add_int(lava_list *l, int x) {
    if (!l) return;
    Value *v = (Value *)l;
    if (v->type != VAL_LIST) return;
    val_list_append(v, val_int(x));
}

void lava_list_add_float(lava_list *l, double x) {
    if (!l) return;
    Value *v = (Value *)l;
    if (v->type != VAL_LIST) return;
    val_list_append(v, val_float(x));
}

void lava_list_add_string(lava_list *l, const char *s) {
    if (!l) return;
    Value *v = (Value *)l;
    if (v->type != VAL_LIST) return;
    val_list_append(v, val_string(s ? s : ""));
}

void lava_list_add_bool(lava_list *l, int x) {
    if (!l) return;
    Value *v = (Value *)l;
    if (v->type != VAL_LIST) return;
    val_list_append(v, val_bool(x != 0));
}

void lava_list_add_list(lava_list *l, lava_list *sub) {
    if (!l || !sub) return;
    Value *v = (Value *)l;
    Value *s = (Value *)sub;
    if (v->type != VAL_LIST || s->type != VAL_LIST) return;
    val_list_append(v, copy_value_secure(*s));   /* copia profunda */
}

void lava_list_free(lava_list *l) {
    if (!l) return;
    Value *v = (Value *)l;
    value_free(v);
    free(v);
}

void infernal_return_list(lava_list *l) {
    if (!g_ctx) {
        fprintf(stderr,
                "Error Lava: infernal_return_list() fuera de una función Lava\n");
        abort();
    }
    if (g_ctx->type_set) {
        fprintf(stderr,
                "Error Lava: tipo de retorno ya fijado antes de "
                "infernal_return_list()\n");
        abort();
    }
    if (g_ctx->value_set) {
        fprintf(stderr, "Error Lava: valor de retorno ya fijado\n");
        abort();
    }
    g_ctx->type     = TOK_LIST;
    g_ctx->type_set = true;

    if (!l) {
        g_ctx->value = val_list_empty();
    } else {
        Value *v = (Value *)l;
        g_ctx->value = *v;    /* transferimos el contenido */
        free(v);              /* liberamos solo el wrapper */
    }
    g_ctx->value_set = true;
}

/* ==================================================================
 * Helpers UTF-8 para strings
 * ================================================================== */

static size_t utf8_seq_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

int lava_string_length(const char *s) {
    if (!s) return 0;
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if ((*p & 0xC0) != 0x80) n++;
        p++;
    }
    return n;
}

const char *lava_string_char(const char *s, int index) {
    static char buf[8];
    if (!s || index < 1) { buf[0] = '\0'; return buf; }

    const unsigned char *p = (const unsigned char *)s;
    int n = 1;
    while (*p && n < index) {
        p += utf8_seq_len(*p);
        n++;
    }
    if (!*p) { buf[0] = '\0'; return buf; }

    size_t len = utf8_seq_len(*p);
    if (len > 7) len = 1;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return buf;
}

char *lava_string_concat(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la > SIZE_MAX - lb - 1) return NULL;
    char *buf = malloc(la + lb + 1);
    if (!buf) return NULL;
    memcpy(buf, a, la);
    memcpy(buf + la, b, lb + 1);
    return buf;
}

char *lava_string_replace(const char *s, const char *from, const char *to) {
    if (!s)    s    = "";
    if (!from) from = "";
    if (!to)   to   = "";
    size_t from_len = strlen(from);
    if (from_len == 0) return strdup(s);

    size_t count = 0;
    const char *p = s;
    while ((p = strstr(p, from)) != NULL) { count++; p += from_len; }

    size_t s_len   = strlen(s);
    size_t to_len  = strlen(to);
    size_t out_len;
    if (to_len >= from_len) {
        size_t diff = to_len - from_len;
        if (count > 0 && diff > (SIZE_MAX - s_len) / count) return NULL;
        out_len = s_len + count * diff;
    } else {
        size_t diff = from_len - to_len;
        if (count > 0 && diff * count > s_len) return NULL;
        out_len = s_len - count * diff;
    }

    char *result = malloc(out_len + 1);
    if (!result) return NULL;

    char *dst = result;
    p = s;
    const char *next;
    while ((next = strstr(p, from)) != NULL) {
        size_t chunk = (size_t)(next - p);
        memcpy(dst, p, chunk);
        dst += chunk;
        memcpy(dst, to, to_len);
        dst += to_len;
        p = next + from_len;
    }
    strcpy(dst, p);
    return result;
}

lava_list *lava_string_split(const char *s, const char *sep) {
    if (!s || !sep || !*sep) return NULL;

    size_t sep_len = strlen(sep);
    Value *result = malloc(sizeof(Value));
    if (!result) return NULL;
    *result = val_list_empty();

    const char *p = s;
    const char *next;
    while ((next = strstr(p, sep)) != NULL) {
        size_t piece_len = (size_t)(next - p);
        char *piece = malloc(piece_len + 1);
        if (piece) {
            memcpy(piece, p, piece_len);
            piece[piece_len] = '\0';
            val_list_append(result, val_string(piece));
            free(piece);
        }
        p = next + sep_len;
    }
    val_list_append(result, val_string(p));

    return (lava_list *)result;
}

/* ==================================================================
 * Registro (llamado por el .lava)
 * ================================================================== */

int lava_register_fn(const char *name, void (*fn)(void), const char *sig) {
    if (!g_loading_module_path) {
        fprintf(stderr,
                "Error Lava: lava_register_fn() fuera de la carga de un módulo\n");
        return -1;
    }
    if (!name || !*name || !fn) {
        fprintf(stderr, "Error Lava: lava_register_fn() con name/fn inválido\n");
        return -1;
    }
    if (!sig) sig = "";

    if (g_slot_count >= LAVA_MAX_SLOTS) {
        fprintf(stderr,
                "Error Lava: límite de %d funciones; '%s' no se registra\n",
                LAVA_MAX_SLOTS, name);
        return -1;
    }

    int nargs = (int)strlen(sig);
    ffi_type **types = NULL;
    if (nargs > 0) {
        types = malloc(sizeof(ffi_type*) * (size_t)nargs);
        if (!types) return -1;
        for (int i = 0; i < nargs; i++) {
            switch (sig[i]) {
                case 'i': types[i] = &ffi_type_sint;    break;
                case 'f': types[i] = &ffi_type_double;  break;
                case 's': types[i] = &ffi_type_pointer; break;
                case 'b': types[i] = &ffi_type_sint;    break;
                case 'l': types[i] = &ffi_type_pointer; break;
                default:
                    fprintf(stderr,
                            "Error Lava: firma '%s' inválida para '%s'. "
                            "Solo i, f, s, b, l.\n", sig, name);
                    free(types);
                    return -1;
            }
        }
    }

    LavaEntry *e = calloc(1, sizeof(LavaEntry));
    if (!e) { free(types); return -1; }
    e->name        = strdup(name);
    e->module_path = strdup(g_loading_module_path);
    e->target_fn   = fn;
    e->signature   = strdup(sig);
    e->nargs       = nargs;
    e->arg_types   = types;

    if (ffi_prep_cif(&e->cif, FFI_DEFAULT_ABI,
                     (unsigned)nargs, &ffi_type_void, types) != FFI_OK) {
        fprintf(stderr, "Error Lava: ffi_prep_cif falló para '%s'\n", name);
        free(e->arg_types); free(e->name); free(e->signature);
        free(e->module_path); free(e);
        return -1;
    }

    int slot = g_slot_count++;
    g_slots[slot] = e;

    extern Value (*lava_thunk_table[LAVA_MAX_SLOTS])(int, Value *);
    Value (*thunk)(int, Value *) = lava_thunk_table[slot];

    /* Nombre base. */
    func_register_builtin(name, thunk);
    vm_register_builtin(strdup(name), thunk);

    /* Nombre con prefijo (módulo o alias). */
    if (g_loading_prefix && *g_loading_prefix) {
        size_t plen = strlen(g_loading_prefix) + 1 + strlen(name) + 1;
        char *prefixed = malloc(plen);
        if (prefixed) {
            snprintf(prefixed, plen, "%s.%s", g_loading_prefix, name);
            func_register_builtin(prefixed, thunk);
            vm_register_builtin(prefixed, thunk);
        }
    }
    return 0;
}

/* ==================================================================
 * Thunks
 * ================================================================== */

static Value lava_dispatch(int slot, int argc, Value *args);

#define LAVA_THUNK_LIST(X) \
    X(0)   X(1)   X(2)   X(3)   X(4)   X(5)   X(6)   X(7)   \
    X(8)   X(9)   X(10)  X(11)  X(12)  X(13)  X(14)  X(15)  \
    X(16)  X(17)  X(18)  X(19)  X(20)  X(21)  X(22)  X(23)  \
    X(24)  X(25)  X(26)  X(27)  X(28)  X(29)  X(30)  X(31)  \
    X(32)  X(33)  X(34)  X(35)  X(36)  X(37)  X(38)  X(39)  \
    X(40)  X(41)  X(42)  X(43)  X(44)  X(45)  X(46)  X(47)  \
    X(48)  X(49)  X(50)  X(51)  X(52)  X(53)  X(54)  X(55)  \
    X(56)  X(57)  X(58)  X(59)  X(60)  X(61)  X(62)  X(63)  \
    X(64)  X(65)  X(66)  X(67)  X(68)  X(69)  X(70)  X(71)  \
    X(72)  X(73)  X(74)  X(75)  X(76)  X(77)  X(78)  X(79)  \
    X(80)  X(81)  X(82)  X(83)  X(84)  X(85)  X(86)  X(87)  \
    X(88)  X(89)  X(90)  X(91)  X(92)  X(93)  X(94)  X(95)  \
    X(96)  X(97)  X(98)  X(99)  X(100) X(101) X(102) X(103) \
    X(104) X(105) X(106) X(107) X(108) X(109) X(110) X(111) \
    X(112) X(113) X(114) X(115) X(116) X(117) X(118) X(119) \
    X(120) X(121) X(122) X(123) X(124) X(125) X(126) X(127) \
    X(128) X(129) X(130) X(131) X(132) X(133) X(134) X(135) \
    X(136) X(137) X(138) X(139) X(140) X(141) X(142) X(143) \
    X(144) X(145) X(146) X(147) X(148) X(149) X(150) X(151) \
    X(152) X(153) X(154) X(155) X(156) X(157) X(158) X(159) \
    X(160) X(161) X(162) X(163) X(164) X(165) X(166) X(167) \
    X(168) X(169) X(170) X(171) X(172) X(173) X(174) X(175) \
    X(176) X(177) X(178) X(179) X(180) X(181) X(182) X(183) \
    X(184) X(185) X(186) X(187) X(188) X(189) X(190) X(191) \
    X(192) X(193) X(194) X(195) X(196) X(197) X(198) X(199) \
    X(200) X(201) X(202) X(203) X(204) X(205) X(206) X(207) \
    X(208) X(209) X(210) X(211) X(212) X(213) X(214) X(215) \
    X(216) X(217) X(218) X(219) X(220) X(221) X(222) X(223) \
    X(224) X(225) X(226) X(227) X(228) X(229) X(230) X(231) \
    X(232) X(233) X(234) X(235) X(236) X(237) X(238) X(239) \
    X(240) X(241) X(242) X(243) X(244) X(245) X(246) X(247) \
    X(248) X(249) X(250) X(251) X(252) X(253) X(254) X(255)

#define LAVA_DEFINE_THUNK(N) \
    static Value lava_thunk_##N(int argc, Value *args) { \
        return lava_dispatch(N, argc, args); \
    }
LAVA_THUNK_LIST(LAVA_DEFINE_THUNK)
#undef LAVA_DEFINE_THUNK

#define LAVA_THUNK_PTR(N) lava_thunk_##N,
Value (*lava_thunk_table[LAVA_MAX_SLOTS])(int, Value *) = {
    LAVA_THUNK_LIST(LAVA_THUNK_PTR)
};
#undef LAVA_THUNK_PTR

/* ==================================================================
 * Dispatch real
 * ================================================================== */

static Value lava_dispatch(int slot, int argc, Value *args) {
    if (slot < 0 || slot >= LAVA_MAX_SLOTS || !g_slots[slot]) {
        error(0, "Error interno Lava: slot %d no registrado", slot);
    }
    LavaEntry *e = g_slots[slot];

    if (argc != e->nargs) {
        error(0, "Lava: '%s' espera %d argumento(s), recibió %d",
              e->name, e->nargs, argc);
    }

    jmp_buf saved_env;
    memcpy(&saved_env, &exception_env, sizeof(jmp_buf));
    int saved_raised = exception_raised;

    void  * volatile storage = NULL;
    void ** volatile values  = NULL;
    LavaCtx ctx;
    LavaCtx *saved_ctx = g_ctx;
    const char *err_msg = NULL;
    char err_buf[512];

    if (setjmp(exception_env) != 0) {
        free((void *)values);
        free((void *)storage);
        g_ctx = saved_ctx;
        memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
        exception_raised = saved_raised;
        longjmp(exception_env, 1);
    }

    if (e->nargs > 0) {
        values  = malloc(sizeof(void *) * (size_t)e->nargs);
        storage = calloc((size_t)e->nargs, LAVA_ARG_SLOT);
        if (!values || !storage) err_msg = "Lava: memoria insuficiente";
    }

    if (!err_msg) {
        for (int i = 0; i < e->nargs; i++) {
            values[i] = (char *)storage + (size_t)i * LAVA_ARG_SLOT;
            switch (e->signature[i]) {
                case 'i': *(int *)values[i] = value_to_c_int(args[i]); break;
                case 'f': *(double *)values[i] = value_to_c_double(args[i]); break;
                case 'b': *(int *)values[i] = value_to_c_int(args[i]) != 0; break;
                case 'l':
                    /* Pasamos la dirección del Value en args[]. Vive
                     * durante toda la llamada, así que es seguro desde
                     * dentro de la función Lava. */
                    *(Value **)values[i] = (args[i].type == VAL_LIST)
                                           ? &args[i] : NULL;
                    break;
                case 's': {
                    if (args[i].type == VAL_STRING) {
                        *(const char **)values[i] =
                            args[i].data.sval ? args[i].data.sval : "";
                    } else {
                        /* Buffer inline tras el puntero. */
                        char *s = (char *)values[i] + sizeof(void *);
                        switch (args[i].type) {
                            case VAL_INT:
                                snprintf(s, LAVA_ARG_SLOT - sizeof(void *),
                                         "%d", args[i].data.ival);
                                break;
                            case VAL_FLOAT:
                                snprintf(s, LAVA_ARG_SLOT - sizeof(void *),
                                         "%g", args[i].data.fval);
                                break;
                            case VAL_BOOL:
                                snprintf(s, LAVA_ARG_SLOT - sizeof(void *),
                                         "%s",
                                         args[i].data.bval ? "true" : "false");
                                break;
                            default:
                                s[0] = '\0';
                                break;
                        }
                        *(const char **)values[i] = s;
                    }
                    break;
                }
                default:
                    *(int *)values[i] = 0;
                    break;
            }
        }
    }

    Value result = val_make_null();
    if (!err_msg) {
        memset(&ctx, 0, sizeof(ctx));
        ctx.argc = argc;
        ctx.args = args;
        g_ctx = &ctx;

        ffi_call(&e->cif, (void (*)(void))e->target_fn, NULL, values);

        g_ctx = saved_ctx;

        if (!ctx.type_set) {
            snprintf(err_buf, sizeof(err_buf),
                     "Lava: '%s' no llamó a infernal_return_type() "
                     "ni a infernal_return_list()", e->name);
            err_msg = err_buf;
        } else if (ctx.value_set) {
            result = ctx.value;
        } else {
            switch (ctx.type) {
                case TOK_INT:    result = val_int(0);         break;
                case TOK_FLOAT:  result = val_float(0.0);     break;
                case TOK_BOOL:   result = val_bool(false);    break;
                case TOK_STRING: result = val_string("");     break;
                case TOK_LIST:   result = val_list_empty();   break;
                default:         result = val_make_null();    break;
            }
        }
    }

    free((void *)values);
    free((void *)storage);

    memcpy(&exception_env, &saved_env, sizeof(jmp_buf));
    exception_raised = saved_raised;

    if (err_msg) error(0, "%s", err_msg);
    return result;
}

/* ==================================================================
 * Carga de módulos
 * ================================================================== */

static int lava_load_from_path(const char *path, const char *prefix) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "Lava: dlopen('%s') falló: %s\n", path, dlerror());
        return 0;
    }

    void (*reg)(void) = (void (*)(void))dlsym(h, "infernal_lava_register");
    if (!reg) {
        fprintf(stderr,
                "Lava: '%s' no exporta infernal_lava_register() "
                "(¿olvidaste LAVA_MODULE?)\n", path);
        dlclose(h);
        return 0;
    }

    LavaHandle *entry = calloc(1, sizeof(LavaHandle));
    if (entry) {
        entry->path   = strdup(path);
        entry->handle = h;
        entry->next   = g_handles;
        g_handles     = entry;
    }

    const char *saved_path   = g_loading_module_path;
    const char *saved_prefix = g_loading_prefix;
    g_loading_module_path    = path;
    g_loading_prefix         = prefix;

    reg();

    g_loading_module_path = saved_path;
    g_loading_prefix      = saved_prefix;
    return 1;
}

static void append_tried(char *tried, size_t tried_size, size_t *used,
                         const char *path) {
    if (!tried || tried_size == 0) return;
    if (*used >= tried_size - 1) return;
    int n = snprintf(tried + *used, tried_size - *used,
                     "        %s\n", path);
    if (n > 0) *used += (size_t)n;
}

static int try_load_at(const char *path, const char *prefix,
                       char *tried, size_t tried_size, size_t *used) {
    append_tried(tried, tried_size, used, path);

    if (access(path, R_OK) != 0) return 0;
    return lava_load_from_path(path, prefix);
}

/*
 * Busca `name` como librería Lava en los directorios estándar. `name`
 * puede contener subdirectorios (p. ej. "build/lava/ejemplo.lava");
 * simplemente se usa tal cual dentro del directorio base, y se le
 * añade siempre la extensión ".lava".
 */
int lava_try_import(const char *name, const char *prefix,
                    char *tried, size_t tried_size) {
    if (tried && tried_size > 0) tried[0] = '\0';
    if (!name || !*name) return 0;

    char path[PATH_MAX];
    size_t used = 0;
    const char *home = getenv("HOME");
    const char *use_prefix = prefix ? prefix : name;

    if (home && *home) {
        snprintf(path, sizeof(path),
                 "%s/.infernal/lava/%s.lava", home, name);
        if (try_load_at(path, use_prefix, tried, tried_size, &used)) return 1;
    }

    snprintf(path, sizeof(path),
             "/usr/share/infernal/lava/%s.lava", name);
    if (try_load_at(path, use_prefix, tried, tried_size, &used)) return 1;

    return 0;
}

/*
 * Carga desde una RUTA concreta (para imports entre comillas).
 * Prueba la ruta tal cual y, si no termina en ".lava", también con la
 * extensión añadida.
 */
int lava_try_import_path(const char *path, const char *prefix,
                         char *tried, size_t tried_size) {
    if (tried && tried_size > 0) tried[0] = '\0';
    if (!path || !*path) return 0;

    size_t used = 0;
    char buf[PATH_MAX];
    const char *use_prefix = prefix ? prefix : path;

    size_t plen = strlen(path);
    int has_ext = (plen >= 5 && strcmp(path + plen - 5, ".lava") == 0);

    if (has_ext) {
        snprintf(buf, sizeof(buf), "%s", path);
        return try_load_at(buf, use_prefix, tried, tried_size, &used);
    }

    snprintf(buf, sizeof(buf), "%s", path);
    if (try_load_at(buf, use_prefix, tried, tried_size, &used)) return 1;

    snprintf(buf, sizeof(buf), "%s.lava", path);
    if (try_load_at(buf, use_prefix, tried, tried_size, &used)) return 1;

    return 0;
}

void lava_cleanup(void) {
    while (g_handles) {
        LavaHandle *h = g_handles;
        g_handles = h->next;
        if (h->handle) dlclose(h->handle);
        free(h->path);
        free(h);
    }
    for (int i = 0; i < g_slot_count; i++) {
        LavaEntry *e = g_slots[i];
        if (!e) continue;
        free(e->name);
        free(e->signature);
        free(e->module_path);
        free(e->arg_types);
        free(e);
        g_slots[i] = NULL;
    }
    g_slot_count = 0;
}
