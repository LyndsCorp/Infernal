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
 *   gcc sumar.c -shared -fPIC -o sumar.lava
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
 *   · Todos los nombres registrados quedan accesibles como "nombre" y "<modulo>.nombre" (o "<alias>.nombre" si se usó `as`).
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

#define lava_export __attribute__((visibility("default")))

#define lava_module \
    lava_export void infernal_lava_register(void)

#define lava_register(name, fn, sig) \
    lava_register_fn((name), (void (*)(void))(fn), (sig))

#ifdef __cplusplus
}
#endif

#endif /* LAVA_LAVA_H */
