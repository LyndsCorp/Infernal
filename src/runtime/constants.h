/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/constants.h
*/

#ifndef RUNTIME_CONSTANTS_H
#define RUNTIME_CONSTANTS_H

#include <stdbool.h>
#include "core/value.h"

/*
 * Las constantes internas pueden exponer un valor propiedad de C mediante
 * callbacks. Esto permite que una constante controle una configuración del
 * intérprete sin duplicar el estado en el registro de constantes.
 */
typedef Value (*ConstantGetter)(void);
typedef void (*ConstantSetter)(const Value *value);

/* Registra las constantes internas de Infernal. Debe llamarse una vez al iniciar. */
void register_all_constants(void);

/* Libera el registro global de constantes al terminar Infernal. */
void constants_cleanup(void);

/* Busca una constante (normal o interna). Devuelve true si existe. */
bool constants_lookup(const char *name, Value *out);

/* Indica si el nombre pertenece al espacio de nombres de constantes. */
bool constants_is_reserved(const char *name);

/* Define una constante desde el lenguaje. La función genera un error si no es válido. */
void constants_define(const char *name, Value value, int line);

/* Las definiciones de constantes solo están permitidas en el script principal. */
void constants_set_definition_allowed(bool allowed);
bool constants_definition_allowed(void);

typedef void (*ConstantVisitor)(const char *name,
                                int vtype,
                                const Value *value,
                                bool internal,
                                void *user_data);

void constants_foreach(ConstantVisitor visitor, void *user_data);

#endif
