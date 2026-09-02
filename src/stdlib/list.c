/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: stdlib/list.c
*/

#include <stdio.h>
#include <string.h>
#include "list.h"
#include "core/value.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "vm/vm.h"
#include "stdlib/output.h"


/* ================================================
 *  Funciones de la biblioteca
 * ================================================ */

/* --- listLatest() --- */
static Value builtin_listLatest(int argc, Value *args) {
    if (argc != 1)
        error(current_eval_line, "listLatest() espera exactamente 1 argumento");
    if (args[0].type != VAL_LIST)
        error(current_eval_line, "listLatest() espera una lista");
    if (args[0].data.list.count == 0)
        error(current_eval_line, "listLatest() no puede aplicarse a una lista vacía");

    Value last = args[0].data.list.items[args[0].data.list.count - 1];
    return copy_value_secure(last);
}


/* ================================================
 *  Registro de funciones
 * ================================================ */

void register_io_builtins(void) {
    func_register_builtin("listLatest", builtin_listLatest);

    vm_register_builtin("listLatest", builtin_listLatest);
}
