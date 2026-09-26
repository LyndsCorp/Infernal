/*
 * Infernal: el intérprete de Aro Infernal.
 * Copyright (C) 2026, David Baña Szymaniak
 * Este software se distribuye bajo la licencia Apache 2.0
 * Código fuente de Infernal: runtime/command.c
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <sys/types.h>
#include <pwd.h>
#include <dlfcn.h>
#include <sys/wait.h>
#include <stdint.h>

#include "command.h"
#include "core/memory.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "runtime/error.h"
#include "embedded/embedded.h"
#include "vm/vm.h"
#include "developer/debug.h"
#include "runtime/constants.h"

/* --- Límite de seguridad para descompresión --- */
#define MAX_DECOMPRESSED_SIZE (500 * 1024 * 1024)  // 500 MiB

static char *embedded_tmp_dir = NULL;

void set_embedded_tmp_dir(const char *dir) {
    if (embedded_tmp_dir) free(embedded_tmp_dir);
    embedded_tmp_dir = dir ? infernal_strdup(dir) : NULL;
}

char *get_var_string(const char *name) {
    VarEntry *e = scope_find(current_scope, name);
    Value constant_value;
    if (!e && constants_lookup(name, &constant_value)) {
        char buf[256];
        char *result = NULL;
        switch (constant_value.type) {
            case VAL_INT: snprintf(buf, sizeof(buf), "%d", constant_value.data.ival); result = strdup(buf); break;
            case VAL_FLOAT: snprintf(buf, sizeof(buf), "%g", constant_value.data.fval); result = strdup(buf); break;
            case VAL_BOOL: result = strdup(constant_value.data.bval ? "true" : "false"); break;
            case VAL_STRING: result = strdup(constant_value.data.sval); break;
            default: break;
        }
        value_free(&constant_value);
        return result;
    }
    if (!e) return NULL;
    Value *v = &e->value;
    char buf[256];
    switch (v->type) {
        case VAL_INT: snprintf(buf, sizeof(buf), "%d", v->data.ival); break;
        case VAL_FLOAT: snprintf(buf, sizeof(buf), "%g", v->data.fval); break;
        case VAL_BOOL: return strdup(v->data.bval ? "true" : "false");
        case VAL_STRING: return strdup(v->data.sval);
        default: return NULL;
    }
    return strdup(buf);
}

static bool grow_text_buffer(char **buffer, size_t *cap, size_t needed) {
    if (needed <= *cap) return true;
    size_t new_cap = *cap ? *cap : 64;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }
    if (new_cap < needed) return false;
    char *tmp = realloc(*buffer, new_cap);
    if (!tmp) return false;
    *buffer = tmp;
    *cap = new_cap;
    return true;
}

/* --- Función auxiliar para añadir '$' + nombre al buffer --- */
static void append_dollar_name(char **result, size_t *len, size_t *cap, const char *name) {
    size_t nlen = strlen(name);
    if (*len > SIZE_MAX - nlen - 2)
        error(current_eval_line, "Comando expandido demasiado largo");
    size_t needed = *len + nlen + 2;
    if (!grow_text_buffer(result, cap, needed))
        error(current_eval_line, "Memoria insuficiente al expandir comando");
    (*result)[(*len)++] = '$';
    memcpy(*result + *len, name, nlen);
    *len += nlen;
}

/* --- Expansión de comandos --- */
char *expand_command(const char *cmd) {
    if (!cmd) return NULL;
    size_t cmd_len = strlen(cmd);
    if (cmd_len > (SIZE_MAX - 65) / 2)
        error(current_eval_line, "Comando demasiado largo");
    size_t cap = cmd_len * 2 + 64;
    char *result = malloc(cap);
    if (!result) error(current_eval_line, "Memoria insuficiente al expandir comando");
    size_t len = 0;
    const char *p = cmd;

    while (*p) {
        if ((*p == '$' || *p == '?') && (isalpha((unsigned char)p[1]) || p[1] == '_')) {
            const char *start = p + 1;
            while (isalnum((unsigned char)*start) || *start == '_') start++;
            size_t nlen = (size_t)(start - (p + 1));
            if (nlen > 127) nlen = 127;
            char name[128];
            memcpy(name, p + 1, nlen);
            name[nlen] = '\0';

            if (*p == '?') {
                append_dollar_name(&result, &len, &cap, name);
                p = start;
                continue;
            }

            char *val = get_var_string(name);
            if (!val) {
                free(result);
                error(current_eval_line, "Variable '%s' no definida", name);
            }
            size_t vlen = strlen(val);
            if (len > SIZE_MAX - vlen - 1) {
                free(val);
                free(result);
                error(current_eval_line, "Comando expandido demasiado largo");
            }
            if (!grow_text_buffer(&result, &cap, len + vlen + 1)) {
                free(val);
                free(result);
                error(current_eval_line, "Memoria insuficiente al expandir comando");
            }
            memcpy(result + len, val, vlen);
            len += vlen;
            free(val);
            p = start;
            continue;
        }
        if (len > SIZE_MAX - 2) {
            free(result);
            error(current_eval_line, "Comando expandido demasiado largo");
        }
        if (!grow_text_buffer(&result, &cap, len + 2)) {
            free(result);
            error(current_eval_line, "Memoria insuficiente al expandir comando");
        }
        result[len++] = *p++;
    }
    result[len] = '\0';
    char *tmp = realloc(result, len + 1);
    return tmp ? tmp : result;
}

/* --- Expansión de comandos usando arrays de locales (para la VM) --- */
char *expand_command_with_locals(const char *cmd, char **names, Value *values, int count) {
    if (!cmd) return NULL;
    size_t cmd_len = strlen(cmd);
    if (cmd_len > (SIZE_MAX - 65) / 2)
        error(current_eval_line, "Comando demasiado largo");
    size_t cap = cmd_len * 2 + 64;
    char *result = malloc(cap);
    if (!result) error(current_eval_line, "Memoria insuficiente al expandir comando");
    size_t len = 0;
    const char *p = cmd;

    while (*p) {
        if ((*p == '$' || *p == '?') &&
            (isalpha((unsigned char)p[1]) || p[1] == '_')) {
            const char *start = p + 1;
            while (isalnum((unsigned char)*start) || *start == '_') start++;
            size_t nlen = (size_t)(start - (p + 1));
            if (nlen > 127) nlen = 127;
            char name[128];
            memcpy(name, p + 1, nlen);
            name[nlen] = '\0';

            if (*p == '?') {
                append_dollar_name(&result, &len, &cap, name);
                p = start;
                continue;
            }

            char *val = NULL;
            for (int i = 0; i < count; i++) {
                if (names[i] && strcmp(names[i], name) == 0) {
                    Value v = values[i];
                    char buf[256];
                    switch (v.type) {
                        case VAL_INT: snprintf(buf, sizeof(buf), "%d", v.data.ival); val = strdup(buf); break;
                        case VAL_FLOAT: snprintf(buf, sizeof(buf), "%g", v.data.fval); val = strdup(buf); break;
                        case VAL_BOOL: val = strdup(v.data.bval ? "true" : "false"); break;
                        case VAL_STRING: val = strdup(v.data.sval); break;
                        default: val = NULL;
                    }
                    break;
                }
            }
            if (!val) {
                VarEntry *e = scope_find(current_scope, name);
                if (e) {
                    Value v = e->value;
                    char buf[256];
                    switch (v.type) {
                        case VAL_INT: snprintf(buf, sizeof(buf), "%d", v.data.ival); val = strdup(buf); break;
                        case VAL_FLOAT: snprintf(buf, sizeof(buf), "%g", v.data.fval); val = strdup(buf); break;
                        case VAL_BOOL: val = strdup(v.data.bval ? "true" : "false"); break;
                        case VAL_STRING: val = strdup(v.data.sval); break;
                        default: val = NULL;
                    }
                }
            }
            if (!val) {
                Value constant_value;
                if (constants_lookup(name, &constant_value)) {
                    Value v = constant_value;
                    char buf[256];
                    switch (v.type) {
                        case VAL_INT: snprintf(buf, sizeof(buf), "%d", v.data.ival); val = strdup(buf); break;
                        case VAL_FLOAT: snprintf(buf, sizeof(buf), "%g", v.data.fval); val = strdup(buf); break;
                        case VAL_BOOL: val = strdup(v.data.bval ? "true" : "false"); break;
                        case VAL_STRING: val = strdup(v.data.sval); break;
                        default: val = NULL;
                    }
                    value_free(&constant_value);
                }
            }
            if (!val) {
                int gidx = vm_find_global_index(name);
                if (gidx >= 0) {
                    Value v = vm_globals[gidx];
                    char buf[256];
                    switch (v.type) {
                        case VAL_INT: snprintf(buf, sizeof(buf), "%d", v.data.ival); val = strdup(buf); break;
                        case VAL_FLOAT: snprintf(buf, sizeof(buf), "%g", v.data.fval); val = strdup(buf); break;
                        case VAL_BOOL: val = strdup(v.data.bval ? "true" : "false"); break;
                        case VAL_STRING: val = strdup(v.data.sval); break;
                        default: val = NULL;
                    }
                }
            }
            if (val) {
                size_t vlen = strlen(val);
                if (len > SIZE_MAX - vlen - 1) {
                    free(val);
                    free(result);
                    error(current_eval_line, "Comando expandido demasiado largo");
                }
                if (!grow_text_buffer(&result, &cap, len + vlen + 1)) {
                    free(val);
                    free(result);
                    error(current_eval_line, "Memoria insuficiente al expandir comando");
                }
                memcpy(result + len, val, vlen);
                len += vlen;
                free(val);
                p = start;
                continue;
            }

            free(result);
            error(current_eval_line, "Variable '%s' no definida", name);
        }
        if (!grow_text_buffer(&result, &cap, len + 2)) {
            free(result);
            error(current_eval_line, "Memoria insuficiente al expandir comando");
        }
        result[len++] = *p++;
    }
    result[len] = '\0';
    char *tmp = realloc(result, len + 1);
    return tmp ? tmp : result;
}

/* --- Descompresión usando libz cargada dinámicamente --- */
static unsigned char *gunzip_data(const unsigned char *compressed, size_t compressed_len, size_t *out_len) {
    static void *zlib_handle = NULL;
    static int zlib_available = -1;
    static const char *zlib_version_str = NULL;

    if (zlib_available == -1) {
        zlib_handle = dlopen("libz.so.1", RTLD_LAZY);
        if (!zlib_handle) {
            zlib_available = 0;
        } else {
            if (!dlsym(zlib_handle, "inflateInit2_") ||
                !dlsym(zlib_handle, "inflate") ||
                !dlsym(zlib_handle, "inflateEnd")) {
                dlclose(zlib_handle);
            zlib_handle = NULL;
            zlib_available = 0;
                } else {
                    typedef const char *(*zlibVersion_t)(void);
                    zlibVersion_t p_zlibVersion = (zlibVersion_t)dlsym(zlib_handle, "zlibVersion");
                    zlib_version_str = p_zlibVersion ? p_zlibVersion() : "1.2.0";
                    zlib_available = 1;
                }
        }
    }

    if (zlib_available == 0) {
        fprintf(stderr, "Error en descompresión de embebidos comprimidos: falta zlib (no está disponible en el sistema).\n");
        return NULL;
    }

    typedef void *(*alloc_func)(void *opaque, unsigned items, unsigned size);
    typedef void  (*free_func)(void *opaque, void *address);

    typedef struct z_stream_s {
        unsigned char *next_in;
        unsigned     avail_in;
        unsigned long total_in;
        unsigned char *next_out;
        unsigned     avail_out;
        unsigned long total_out;
        char         *msg;
        void         *state;
        alloc_func   zalloc;
        free_func    zfree;
        void         *opaque;
        int          data_type;
        unsigned long adler;
        unsigned long reserved;
    } z_stream;

    typedef int (*inflateInit2_t)(z_stream *strm, int windowBits, const char *version, int stream_size);
    typedef int (*inflate_t)(z_stream *strm, int flush);
    typedef int (*inflateEnd_t)(z_stream *strm);

    inflateInit2_t p_inflateInit2 = (inflateInit2_t)dlsym(zlib_handle, "inflateInit2_");
    inflate_t      p_inflate      = (inflate_t)dlsym(zlib_handle, "inflate");
    inflateEnd_t   p_inflateEnd   = (inflateEnd_t)dlsym(zlib_handle, "inflateEnd");

    #define Z_OK            0
    #define Z_STREAM_END    1
    #define Z_NO_FLUSH      0
    #define Z_STREAM_ERROR (-2)
    #define Z_DATA_ERROR   (-3)
    #define Z_BUF_ERROR    (-5)
    #define MAX_WBITS       15

    z_stream strm = {0};
    if (p_inflateInit2(&strm, 16 + MAX_WBITS, zlib_version_str, sizeof(strm)) != Z_OK) {
        fprintf(stderr, "Error: no se pudo inicializar la descompresión zlib (versión %s)\n", zlib_version_str);
        return NULL;
    }

    size_t buf_size;
    if (compressed_len > (MAX_DECOMPRESSED_SIZE - 1024) / 4)
        buf_size = MAX_DECOMPRESSED_SIZE;
    else
        buf_size = compressed_len * 4 + 1024;
    unsigned char *out = malloc(buf_size);
    if (!out) { p_inflateEnd(&strm); return NULL; }

    strm.next_in  = (unsigned char *)compressed;
    strm.avail_in = compressed_len;
    strm.next_out = out;
    strm.avail_out = buf_size;

    int ret;
    for (;;) {
        ret = p_inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK) {
            free(out);
            p_inflateEnd(&strm);
            return NULL;
        }

        if (strm.total_out >= MAX_DECOMPRESSED_SIZE) {
            fprintf(stderr, "Error: el binario descomprimido excede el límite de %zu bytes\n",
                    (size_t)MAX_DECOMPRESSED_SIZE);
            free(out);
            p_inflateEnd(&strm);
            return NULL;
        }

        size_t used = strm.next_out - out;
        size_t new_size = buf_size * 2;
        if (new_size > MAX_DECOMPRESSED_SIZE)
            new_size = MAX_DECOMPRESSED_SIZE;
        buf_size = new_size;

        unsigned char *tmp = realloc(out, buf_size);
        if (!tmp) { free(out); p_inflateEnd(&strm); return NULL; }
        out = tmp;
        strm.next_out = out + used;
        strm.avail_out = buf_size - used;
    }
    *out_len = strm.next_out - out;
    p_inflateEnd(&strm);
    return out;
}

/* --- Extracción del binario embebido (con soporte de compresión) --- */
static char *prepare_embedded_binary(const char *cmd_name) {
    const unsigned char *data = NULL;
    size_t size = 0;
    int compressed = 0;
    if (!embedded_find(cmd_name, &data, &size, &compressed))
        return NULL;

    const unsigned char *raw_data = data;
    size_t raw_size = size;
    unsigned char *decompressed = NULL;

    if (compressed) {
        decompressed = gunzip_data(data, size, &raw_size);
        if (!decompressed) {
            fprintf(stderr, "Error: no se pudo descomprimir el binario embebido '%s'\n", cmd_name);
            return NULL;
        }
        raw_data = decompressed;
    }

    char tmp_path[PATH_MAX];
    int fd = -1;

    const char *base_dir = embedded_tmp_dir ? embedded_tmp_dir : ".";
    if (access(base_dir, W_OK) == 0) {
        char work_dir[PATH_MAX];
        int dir_len = snprintf(work_dir, sizeof(work_dir), "%s/.infernal_tmp", base_dir);
        if (dir_len > 0 && (size_t)dir_len < sizeof(work_dir) &&
            (mkdir(work_dir, 0700) == 0 || errno == EEXIST)) {
            int path_len = snprintf(tmp_path, sizeof(tmp_path), "%s/infernal_XXXXXX", work_dir);
        if (path_len > 0 && (size_t)path_len < sizeof(tmp_path)) {
            fd = mkstemp(tmp_path);
        }
            }
    }

    if (fd == -1) {
        const char *tmpdir = getenv("TMPDIR");
        if (tmpdir && tmpdir[0]) {
            snprintf(tmp_path, sizeof(tmp_path), "%s/infernal_XXXXXX", tmpdir);
            fd = mkstemp(tmp_path);
        }
    }
    if (fd == -1) {
        snprintf(tmp_path, sizeof(tmp_path), "/tmp/infernal_XXXXXX");
        fd = mkstemp(tmp_path);
    }

    if (fd == -1) {
        perror("mkstemp");
        free(decompressed);
        return NULL;
    }

    size_t offset = 0;
    while (offset < raw_size) {
        ssize_t written = write(fd, raw_data + offset, raw_size - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            perror("write");
            close(fd);
            unlink(tmp_path);
            free(decompressed);
            return NULL;
        }
        offset += (size_t)written;
    }

    fdatasync(fd);
    fchmod(fd, 0700);
    close(fd);

    free(decompressed);

    char *abs_path = realpath(tmp_path, NULL);
    if (!abs_path) {
        perror("realpath");
        unlink(tmp_path);
        return NULL;
    }
    return abs_path;
}

/* --- Ejecutar comandos a través del shell configurado de Infernal --- */
static char *shell_quote(const char *text) {
    if (!text) return NULL;

    size_t len = 2; /* comillas simples de apertura y cierre */
    for (const char *p = text; *p; p++)
        len += (*p == '\'') ? 4 : 1;

    char *quoted = malloc(len + 1);
    if (!quoted) return NULL;

    size_t out = 0;
    quoted[out++] = '\'';
    for (const char *p = text; *p; p++) {
        if (*p == '\'') {
            quoted[out++] = '\'';
            quoted[out++] = '\\';
            quoted[out++] = '\'';
            quoted[out++] = '\'';
        } else {
            quoted[out++] = *p;
        }
    }
    quoted[out++] = '\'';
    quoted[out] = '\0';
    return quoted;
}

FILE *popen_infernal_shell(const char *cmd, const char *mode) {
    if (!cmd || !mode) return NULL;

    const char *shell = infernal_shell ? infernal_shell : "/bin/sh";
    char *quoted_shell = shell_quote(shell);
    char *quoted_cmd = shell_quote(cmd);
    if (!quoted_shell || !quoted_cmd) {
        free(quoted_shell);
        free(quoted_cmd);
        return NULL;
    }

    size_t len = strlen(quoted_shell) + strlen(quoted_cmd) + 16;
    char *wrapper = malloc(len);
    if (!wrapper) {
        free(quoted_shell);
        free(quoted_cmd);
        return NULL;
    }

    snprintf(wrapper, len, "exec %s -c %s", quoted_shell, quoted_cmd);
    FILE *fp = popen(wrapper, mode);

    free(wrapper);
    free(quoted_shell);
    free(quoted_cmd);
    return fp;
}

/* --- Ejecutar comando embebido y devolver código de salida --- */
int execute_embedded(const char *full_cmd) {
    if (!full_cmd) return -1;
    char *cmd_copy = infernal_strdup(full_cmd);
    char *saveptr;
    char *cmd_name = strtok_r(cmd_copy, " \t", &saveptr);
    if (!cmd_name) { free(cmd_copy); return -1; }

    char *binary_path = prepare_embedded_binary(cmd_name);
    if (!binary_path) { free(cmd_copy); return -1; }

    char *quoted_binary = shell_quote(binary_path);
    if (!quoted_binary) { unlink(binary_path); free(binary_path); free(cmd_copy); return -1; }
    size_t len = strlen(quoted_binary) + 1;
    char *rest = saveptr;
    if (rest && *rest) len += strlen(rest) + 1;
    char *exec_cmd = malloc(len);
    if (!exec_cmd) { unlink(binary_path); free(quoted_binary); free(binary_path); free(cmd_copy); return -1; }
    int written = snprintf(exec_cmd, len, "%s%s%s", quoted_binary, (rest && *rest) ? " " : "", (rest && *rest) ? rest : "");
    if (written < 0 || (size_t)written >= len) {
        unlink(binary_path);
        free(quoted_binary);
        free(binary_path);
        free(exec_cmd);
        free(cmd_copy);
        return -1;
    }

    /* Evitamos system(): en Android/Termux puede intervenir el preload de ejecución
     * y corromper los punteros etiquetados antes de devolver el control al intérprete. */
    pid_t pid = fork();
    if (pid == 0) {
        const char *shell = infernal_shell ? infernal_shell : "/bin/sh";
        execlp(shell, shell, "-c", exec_cmd, (char *)NULL);
        _exit(127);
    }

    int ret = -1;
    if (pid > 0) {
        pid_t waited;
        do {
            waited = waitpid(pid, &ret, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0) ret = -1;
    }

    unlink(binary_path);
    free(quoted_binary);
    free(binary_path);
    free(exec_cmd);
    free(cmd_copy);

    if (ret == -1) return -1;
    if (WIFEXITED(ret)) return WEXITSTATUS(ret);
    return -1;
}

FILE *popen_embedded_with_path(const char *full_cmd, const char *mode, char **temp_path) {
    if (!full_cmd || !temp_path) return NULL;
    char *cmd_copy = infernal_strdup(full_cmd);
    char *saveptr;
    char *cmd_name = strtok_r(cmd_copy, " \t", &saveptr);
    if (!cmd_name) { free(cmd_copy); return NULL; }
    char *binary_path = prepare_embedded_binary(cmd_name);
    if (!binary_path) { free(cmd_copy); return NULL; }
    char *quoted_binary = shell_quote(binary_path);
    if (!quoted_binary) { unlink(binary_path); free(binary_path); free(cmd_copy); return NULL; }
    size_t len = strlen(quoted_binary) + 1;
    char *rest = saveptr;
    if (rest && *rest) len += strlen(rest) + 1;
    char *exec_cmd = malloc(len);
    if (!exec_cmd) { unlink(binary_path); free(quoted_binary); free(binary_path); free(cmd_copy); return NULL; }
    int written = snprintf(exec_cmd, len, "%s%s%s", quoted_binary, (rest && *rest) ? " " : "", (rest && *rest) ? rest : "");
    if (written < 0 || (size_t)written >= len) {
        unlink(binary_path);
        free(quoted_binary);
        free(binary_path);
        free(exec_cmd);
        free(cmd_copy);
        return NULL;
    }
    FILE *fp = popen_infernal_shell(exec_cmd, mode);
    if (fp) { *temp_path = binary_path; }
    else { unlink(binary_path); free(binary_path); }
    free(quoted_binary);
    free(exec_cmd);
    free(cmd_copy);
    return fp;
}

void cleanup_embedded_temp_dir(void) {
    const char *base_dir = embedded_tmp_dir ? embedded_tmp_dir : ".";
    char work_dir[PATH_MAX];
    int dir_len = snprintf(work_dir, sizeof(work_dir), "%s/.infernal_tmp", base_dir);
    if (dir_len <= 0 || (size_t)dir_len >= sizeof(work_dir)) return;
    DIR *d = opendir(work_dir);
    if (!d) return;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (strncmp(entry->d_name, "infernal_", 9) != 0) continue;
        char full_path[PATH_MAX];
        int path_len = snprintf(full_path, sizeof(full_path), "%s/%s", work_dir, entry->d_name);
        if (path_len <= 0 || (size_t)path_len >= sizeof(full_path)) continue;
        unlink(full_path);
    }
    closedir(d);
    rmdir(work_dir);
}

/* --- Ejecutar comando shell con el shell configurado --- */
int run_shell_command(const char *cmd) {
    if (!infernal_shell) {
        DEBUG_WARN("infernal_shell no configurado, usando /bin/sh fallback");
    }

    const char *shell = infernal_shell ? infernal_shell : "/bin/sh";
    DEBUG_OP("Ejecutando shell: %s -c \"%s\"", shell, cmd);

    pid_t pid = fork();
    if (pid == 0) {
        execlp(shell, shell, "-c", cmd, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int status;
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) return -1;
        }
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        return -1;
    } else {
        return -1;
    }
}

int run_command_get_exit_code(const char *cmd) {
    if (!cmd) return -1;
    char *expanded = expand_command(cmd);
    if (!expanded) return -1;

    // Construir comando con stderr redirigido a /dev/null
    char *silent_cmd = malloc(strlen(expanded) + 20);
    if (!silent_cmd) {
        free(expanded);
        return -1;
    }
    sprintf(silent_cmd, "%s 2>/dev/null", expanded);
    free(expanded);

    FILE *fp = popen_infernal_shell(silent_cmd, "r");
    free(silent_cmd);
    if (!fp) return -1;

    // Leer y descartar toda la salida (stdout)
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp) != NULL) {}

    int status = pclose(fp);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}
