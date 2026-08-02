/*
 * Infernal: el lenguaje de programación.
 * Copyright (C) 2026, Lynds Corp., David Baña Szymaniak, GPL v3+ License.
 * Código fuente de Infernal: developer/debug.c
 *
 * Implementación de funciones de depuración (si se necesitan).
 * Por ahora solo contiene inicialización.
*/

#include "debug.h"
#include <stdlib.h>

/* ─── Inicialización del sistema de depuración ──────────────── */
#ifdef DEBUG
static int debug_initialized = 0;

void debug_init(void) {
    if (debug_initialized) return;
    debug_initialized = 1;
    DEBUG_INFO("Sistema de depuración iniciado");
}

/* Se puede llamar desde main si se compila con DEBUG */
#else
void debug_init(void) {
    /* Nada en modo producción */
}
#endif

/* ─── Función para activar/desactivar logs en tiempo de ejecución ── */
#ifdef DEBUG
static int debug_enabled = 1;

void debug_set_enabled(int enabled) {
    debug_enabled = enabled;
}

int debug_is_enabled(void) {
    return debug_enabled;
}
#else
void debug_set_enabled(int enabled) {
    (void)enabled;
}
int debug_is_enabled(void) {
    return 0;
}
#endif
