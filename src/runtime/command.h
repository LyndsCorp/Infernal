/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/command.h
*/

#ifndef RUNTIME_COMMAND_H
#define RUNTIME_COMMAND_H

#include <stdio.h>
#include "core/value.h"

char *expand_command(const char *cmd);
char *get_var_string(const char *name);
char *expand_command_with_locals(const char *cmd, char **names, Value *values, int count);
int execute_embedded(const char *full_cmd);
FILE *popen_embedded_with_path(const char *full_cmd, const char *mode, char **temp_path);
void set_embedded_tmp_dir(const char *dir);
void cleanup_embedded_temp_dir(void);
int run_shell_command(const char *cmd);
int run_command_get_exit_code(const char *cmd);

#endif
