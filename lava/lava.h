/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026 David Baña Szymaniak
 * Licencia Apache 2.0
 * Código fuente de Infernal: lava/lava.h
 *
 * API pública para escribir módulos Lava para Infernal (librerías .lava).
 *
 * Ejemplo:

#include "lava.h"

LAVA_EXPORT void mi_suma(int a, int b) {
    infernal_return_type("int");
    infernal_return_value("%d", a + b);
}

LAVA_MODULE {
    LAVA_REGISTER("suma", mi_suma, "ii");
}

*
* Desde Infernal:
*

import "lava/mi_modulo.lava" as m
print(m.suma(2, 3))   # 5
print(suma(2, 3))     # 5  (también sin prefijo)

*/

#ifndef INFERNAL_LAVA_H
#define INFERNAL_LAVA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef LAVA_EXPORT
#  if defined(_WIN32) || defined(__CYGWIN__)
#    define LAVA_EXPORT __declspec(dllexport)
#  else
#    define LAVA_EXPORT __attribute__((visibility("default")))
#  endif
#endif

#ifdef __cplusplus
#  define LAVA_LINKAGE extern "C"
#else
#  define LAVA_LINKAGE
#endif

#define VAL_NULL      0
#define VAL_INT       1
#define VAL_FLOAT     2
#define VAL_BOOL      3
#define VAL_STRING    4
#define VAL_LIST      5
#define VAL_REFERENCE 6
#define VAL_PTR       7
#define VAL_MAP       8

typedef struct MapData MapData;
typedef struct Value   Value;

struct Value {
    int type;
    union {
        int    ival;
        double fval;
        bool   bval;
        char  *sval;
        struct {
            Value *items;
            int    count, cap;
        } list;
        struct {
            char *container_name;
            char *map_key;
            int   index;
            bool  is_map;
        } ref;
        void    *ptr;
        MapData *map;
    } data;
};

typedef Value lava_list;

#ifdef __cplusplus
extern "C" {
    #endif

    Value val_make_null(void);
    Value val_int(int x);
    Value val_float(double x);
    Value val_bool(bool x);
    Value val_string(const char *s);
    Value val_list_empty(void);

    void  val_list_append(Value *list, Value item);
    void  value_free(Value *value);
    Value copy_value_secure(Value src);

    Value *make_infernal_const(const char *name);
    Value *get_infernal_const (const char *name);

    typedef void (*LavaFnPtr)(void);

    int lava_register_fn(const char *name, LavaFnPtr fn, const char *signature);

    void infernal_return_type (const char *type);
    void infernal_return_value(const char *fmt, ...);
    void infernal_return_list (lava_list *l);

    int          lava_argc      (void);
    int          lava_arg_int   (int index);
    double       lava_arg_float (int index);
    int          lava_arg_bool  (int index);
    const char  *lava_arg_string(int index);
    lava_list   *lava_arg_list  (int index);

    int          lava_list_len          (lava_list *l);
    const char  *lava_list_element_type (lava_list *l, int index);
    int          lava_list_int          (lava_list *l, int index);
    double       lava_list_float        (lava_list *l, int index);
    const char  *lava_list_string       (lava_list *l, int index);
    int          lava_list_bool         (lava_list *l, int index);
    lava_list   *lava_list_list         (lava_list *l, int index);

    lava_list   *lava_list_create(void);

    void         lava_list_add_int    (lava_list *l, int x);
    void         lava_list_add_float  (lava_list *l, double x);
    void         lava_list_add_string (lava_list *l, const char *s);
    void         lava_list_add_bool   (lava_list *l, int x);
    void         lava_list_add_list   (lava_list *l, lava_list *sub);

    void         lava_list_free       (lava_list *l);

    int         lava_string_length(const char *s);
    const char *lava_string_char  (const char *s, int index);

    char       *lava_string_concat (const char *a, const char *b);
    char       *lava_string_replace(const char *s, const char *from, const char *to);
    lava_list  *lava_string_split  (const char *s, const char *sep);

    #ifdef __cplusplus
}
#endif

#define LAVA_MODULE \
LAVA_LINKAGE LAVA_EXPORT void infernal_lava_register(void)

#define LAVA_REGISTER(name, fn, sig) \
lava_register_fn((name), (LavaFnPtr)(fn), (sig))

#endif
