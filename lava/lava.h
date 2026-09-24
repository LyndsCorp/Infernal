/*
 * Infernal — Header público para librerías Lava.
 * Copyright (C) 2026, David Baña Szymaniak
 * Licencia Apache 2.0
 *
 * ----------------------------------------------------------------------
 * Cómo escribir una librería Lava
 * ----------------------------------------------------------------------
 *

#include "lava.h"

void sumar(int a, int b) {
    infernal_return_type("int");
    infernal_return_value("%d", a + b);
}

lava_module {
    lava_register("sumar", sumar, "ii");
}

 *
 * Compilar:
 *   gcc ejemplo.c -shared -fPIC -I.. -o ejemplo.lava
 *
 * Instalar:
 *   cp sumar.lava ~/.infernal/lava/                 (usuario)
 *   sudo cp sumar.lava /usr/share/infernal/lava/    (sistema)
 *
 * ----------------------------------------------------------------------
 * Reglas
 * ----------------------------------------------------------------------
 *   · Cada función debe llamar a infernal_return_type() antes de infernal_return_value().
 *   · Tipos válidos: "int", "float", "string", "bool".
 *   · Firma: un carácter por argumento:
 *       'i' -> int
 *       'f' -> double
 *       's' -> const char*
 *       'b' -> int (bool; 0 = false)
 *       'l' -> lava_list*
 *   · Todos los nombres registrados quedan accesibles como "nombre" y se ejecutan con "<modulo>.nombre" (o "<alias>.nombre" si se usó `as`).
*/

#ifndef LAVA_LAVA_H
#define LAVA_LAVA_H

#ifdef __cplusplus
extern "C" {
    #endif

    void infernal_return_type (const char *type);
    void infernal_return_value(const char *fmt, ...);

    int         lava_argc(void);
    int         lava_arg_int   (int index);
    double      lava_arg_float (int index);
    const char *lava_arg_string(int index);
    int         lava_arg_bool  (int index);

    int lava_register_fn(const char *name, void (*fn)(void), const char *signature);

    typedef struct lava_list lava_list;

    lava_list  *lava_arg_list(int index);
    int         lava_list_len(lava_list *l);
    const char *lava_list_element_type(lava_list *l, int index);
    int         lava_list_int(lava_list *l, int index);
    double      lava_list_float(lava_list *l, int index);
    const char *lava_list_string(lava_list *l, int index);
    int         lava_list_bool(lava_list *l, int index);
    lava_list  *lava_list_list(lava_list *l, int index);

    lava_list  *lava_list_create(void);
    void        lava_list_add_int(lava_list *l, int v);
    void        lava_list_add_float(lava_list *l, double v);
    void        lava_list_add_string(lava_list *l, const char *v);
    void        lava_list_add_bool(lava_list *l, int v);
    void        lava_list_add_list(lava_list *l, lava_list *sub);
    void        lava_list_free(lava_list *l);
    void        infernal_return_list(lava_list *l);

    int         lava_string_length(const char *s);
    const char *lava_string_char(const char *s, int index);
    char       *lava_string_concat(const char *a, const char *b);
    char       *lava_string_replace(const char *s, const char *from, const char *to);
    lava_list  *lava_string_split(const char *s, const char *sep);

    #define LAVA_EXPORT __attribute__((visibility("default")))

    #define LAVA_MODULE \
    LAVA_EXPORT void infernal_lava_register(void)

    #define LAVA_REGISTER(name, fn, sig) \
    lava_register_fn((name), (void (*)(void))(fn), (sig))

    #ifdef __cplusplus
}
#endif

#endif /* LAVA_LAVA_H */
