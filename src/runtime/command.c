/*
 * Infernal: el lenguaje de programación. Copyright (C) 2026, GPL v3+ License, Lynds Corp., Aros Legendarios, David Baña Szymaniak.
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

#include "command.h"
#include "runtime/scope.h"
#include "runtime/globals.h"
#include "stdlib/embedded.h"
#include "vm/vm.h"
#include "developer/debug.h"

/* ─── Límite de seguridad para descompresión ─── */
#define MAX_DECOMPRESSED_SIZE (500 * 1024 * 1024)  /* 500 MiB */

static char *embedded_tmp_dir = NULL;

void set_embedded_tmp_dir(const char *dir) {
    if (embedded_tmp_dir) free(embedded_tmp_dir);
    embedded_tmp_dir = dir ? strdup(dir) : NULL;
}

/* ─── Convertir un Value a string para uso en comandos ──────────── */
static char *value_to_command_string(Value v) {
    if (v.type == VAL_LIST) {
        // Si la lista está vacía, devolver cadena vacía
        if (v.data.list.count == 0) return strdup("");

        char *result = strdup("");
        for (int i = 0; i < v.data.list.count; i++) {
            Value elem = v.data.list.items[i];
            char *elem_str = value_to_command_string(elem);  // recursivo para elementos anidados

            // Si el elemento contiene espacios o caracteres especiales, envolver entre comillas dobles
            if (strchr(elem_str, ' ') || strchr(elem_str, '\t') || strchr(elem_str, '\'')) {
                char *tmp = malloc(strlen(elem_str) + 3);
                sprintf(tmp, "\"%s\"", elem_str);
                free(elem_str);
                elem_str = tmp;
            }

            if (i > 0) {
                result = realloc(result, strlen(result) + 1 + strlen(elem_str) + 1);
                strcat(result, " ");
            } else {
                result = realloc(result, strlen(result) + strlen(elem_str) + 1);
            }
            strcat(result, elem_str);
            free(elem_str);
        }
        return result;
    } else {
        char buf[256];
        switch (v.type) {
            case VAL_INT:    snprintf(buf, sizeof(buf), "%d", v.data.ival); break;
            case VAL_FLOAT:  snprintf(buf, sizeof(buf), "%g", v.data.fval); break;
            case VAL_BOOL:   return strdup(v.data.bval ? "true" : "false");
            case VAL_STRING: return strdup(v.data.sval);
            default:         return strdup("");
        }
        return strdup(buf);
    }
}

/* ─── Obtener representación string de una variable para comandos ── */
char *get_var_string(const char *name) {
    VarEntry *e = scope_find(current_scope, name);
    if (!e) return NULL;
    return value_to_command_string(e->value);
}

/* ─── Función auxiliar para añadir '$' + nombre al buffer ─── */
static void append_dollar_name(char **result, size_t *len, size_t *cap, const char *name) {
    size_t nlen = strlen(name);
    if (*len + 1 + nlen >= *cap) {
        *cap = (*len + 1 + nlen) * 2;
        *result = realloc(*result, *cap);
    }
    (*result)[(*len)++] = '$';
    memcpy(*result + *len, name, nlen);
    *len += nlen;
}

/* ─── Expansión de comandos (versión con scopes) ─────────────── */
char *expand_command(const char *cmd) {
    if (!cmd) return NULL;
    size_t cap = strlen(cmd) * 2 + 64;
    char *result = malloc(cap);
    size_t len = 0;
    const char *p = cmd;

    while (*p) {
        if ((*p == '$' || *p == '?') && (isalpha(*(p+1)) || *(p+1) == '_')) {
            const char *start = p + 1;
            while (isalnum(*start) || *start == '_') start++;
            size_t nlen = start - (p + 1);
            if (nlen > 127) nlen = 127;
            char name[128];
            memcpy(name, p + 1, nlen);
            name[nlen] = '\0';

            if (*p == '?') {
                // ?VAR → $VAR literal
                append_dollar_name(&result, &len, &cap, name);
                p = start;
                continue;
            } else {
                // $VAR → expandir con valor de Infernal
                char *val = get_var_string(name);
                if (val) {
                    size_t vlen = strlen(val);
                    if (len + vlen >= cap) {
                        cap = (len + vlen) * 2;
                        result = realloc(result, cap);
                    }
                    memcpy(result + len, val, vlen);
                    len += vlen;
                    free(val);
                    p = start;
                    continue;
                }
                // Si no existe, copiar $VAR tal cual
                if (len + 1 + nlen >= cap) {
                    cap = (len + 1 + nlen) * 2;
                    result = realloc(result, cap);
                }
                result[len++] = '$';
                memcpy(result + len, name, nlen);
                len += nlen;
                p = start;
                continue;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            result = realloc(result, cap);
        }
        result[len++] = *p++;
    }
    result[len] = '\0';
    return realloc(result, len + 1);
}

/* ─── Expansión de comandos usando arrays de locales (para la VM) ─── */
char *expand_command_with_locals(const char *cmd, char **names, Value *values, int count) {
    if (!cmd) return NULL;
    size_t cap = strlen(cmd) * 2 + 64;
    char *result = malloc(cap);
    size_t len = 0;
    const char *p = cmd;

    while (*p) {
        if ((*p == '$' || *p == '?') && (isalpha(*(p+1)) || *(p+1) == '_')) {
            const char *start = p + 1;
            while (isalnum(*start) || *start == '_') start++;
            size_t nlen = start - (p + 1);
            if (nlen > 127) nlen = 127;
            char name[128];
            memcpy(name, p + 1, nlen);
            name[nlen] = '\0';

            if (*p == '?') {
                // ?VAR → $VAR literal
                append_dollar_name(&result, &len, &cap, name);
                p = start;
                continue;
            } else {
                char *val = NULL;

                // 1) Locales de la VM
                for (int i = 0; i < count; i++) {
                    if (names[i] && strcmp(names[i], name) == 0) {
                        val = value_to_command_string(values[i]);
                        break;
                    }
                }

                // 2) Scopes de Infernal (current_scope)
                if (!val) {
                    VarEntry *e = scope_find(current_scope, name);
                    if (e) {
                        val = value_to_command_string(e->value);
                    }
                }

                // 3) Globales de la VM
                if (!val) {
                    int gidx = vm_find_global_index(name);
                    if (gidx >= 0) {
                        val = value_to_command_string(vm_globals[gidx]);
                    }
                }

                if (val) {
                    size_t vlen = strlen(val);
                    if (len + vlen >= cap) {
                        cap = (len + vlen) * 2;
                        result = realloc(result, cap);
                    }
                    memcpy(result + len, val, vlen);
                    len += vlen;
                    free(val);
                    p = start;
                    continue;
                }

                // Si no se encontró, copiar $VAR literal
                if (len + 1 + nlen >= cap) {
                    cap = (len + 1 + nlen) * 2;
                    result = realloc(result, cap);
                }
                result[len++] = '$';
                memcpy(result + len, name, nlen);
                len += nlen;
                p = start;
                continue;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            result = realloc(result, cap);
        }
        result[len++] = *p++;
    }
    result[len] = '\0';
    return realloc(result, len + 1);
}

/* ─── Descompresión usando libz cargada dinámicamente ──────────── */
static unsigned char *gunzip_data(const unsigned char *compressed, size_t compressed_len, size_t *out_len) {
    static void *zlib_handle = NULL;
    static int zlib_available = -1;  // -1 = no verificado, 0 = no, 1 = sí
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
    #define Z_FINISH        4
    #define MAX_WBITS       15

    z_stream strm = {0};
    if (p_inflateInit2(&strm, 16 + MAX_WBITS, zlib_version_str, sizeof(strm)) != Z_OK) {
        fprintf(stderr, "Error: no se pudo inicializar la descompresión zlib (versión %s)\n", zlib_version_str);
        return NULL;
    }

    size_t buf_size = compressed_len * 4 + 1024;
    if (buf_size > MAX_DECOMPRESSED_SIZE) buf_size = MAX_DECOMPRESSED_SIZE;
    unsigned char *out = malloc(buf_size);
    if (!out) { p_inflateEnd(&strm); return NULL; }

    strm.next_in  = (unsigned char *)compressed;
    strm.avail_in = compressed_len;
    strm.next_out = out;
    strm.avail_out = buf_size;

    int ret;
    while ((ret = p_inflate(&strm, Z_FINISH)) != Z_STREAM_END) {
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

/* ─── Extracción del binario embebido (con soporte de compresión) ─── */
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

/* ─── Ejecutar comando embebido y devolver código de salida ─── */
int execute_embedded(const char *full_cmd) {
    if (!full_cmd) return -1;
    char *cmd_copy = strdup(full_cmd);
    char *saveptr;
    char *cmd_name = strtok_r(cmd_copy, " \t", &saveptr);
    if (!cmd_name) { free(cmd_copy); return -1; }

    char *binary_path = prepare_embedded_binary(cmd_name);
    if (!binary_path) { free(cmd_copy); return -1; }

    size_t len = strlen(binary_path) + 1;
    char *rest = saveptr;
    if (rest && *rest) len += strlen(rest) + 1;
    char *exec_cmd = malloc(len);
    if (!exec_cmd) { unlink(binary_path); free(binary_path); free(cmd_copy); return -1; }
    strcpy(exec_cmd, binary_path);
    if (rest && *rest) { strcat(exec_cmd, " "); strcat(exec_cmd, rest); }

    int ret = system(exec_cmd);
    unlink(binary_path);
    free(binary_path);
    free(exec_cmd);
    free(cmd_copy);

    if (ret == -1) return -1;
    if (WIFEXITED(ret)) return WEXITSTATUS(ret);
    return -1;   // terminación anormal
}

FILE *popen_embedded_with_path(const char *full_cmd, const char *mode, char **temp_path) {
    if (!full_cmd || !temp_path) return NULL;
    char *cmd_copy = strdup(full_cmd);
    char *saveptr;
    char *cmd_name = strtok_r(cmd_copy, " \t", &saveptr);
    if (!cmd_name) { free(cmd_copy); return NULL; }
    char *binary_path = prepare_embedded_binary(cmd_name);
    if (!binary_path) { free(cmd_copy); return NULL; }
    size_t len = strlen(binary_path) + 1;
    char *rest = saveptr;
    if (rest && *rest) len += strlen(rest) + 1;
    char *exec_cmd = malloc(len);
    if (!exec_cmd) { unlink(binary_path); free(binary_path); free(cmd_copy); return NULL; }
    strcpy(exec_cmd, binary_path);
    if (rest && *rest) { strcat(exec_cmd, " "); strcat(exec_cmd, rest); }
    FILE *fp = popen(exec_cmd, mode);
    if (fp) { *temp_path = binary_path; }
    else { unlink(binary_path); free(binary_path); }
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
        char full_path[PATH_MAX];
        int path_len = snprintf(full_path, sizeof(full_path), "%s/%s", work_dir, entry->d_name);
        if (path_len <= 0 || (size_t)path_len >= sizeof(full_path)) continue;
        unlink(full_path);
    }
    closedir(d);
    rmdir(work_dir);
}

/* ─── Ejecutar comando shell con el shell configurado ──────── */
int run_shell_command(const char *cmd) {
    if (!infernal_shell) {
        DEBUG_WARN("infernal_shell no configurado, usando system() fallback");
        return system(cmd);
    }

    DEBUG_OP("Ejecutando shell: %s -c \"%s\"", infernal_shell, cmd);

    pid_t pid = fork();
    if (pid == 0) {
        execlp(infernal_shell, infernal_shell, "-c", cmd, (char *)NULL);
        execlp("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
        exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        return -1;
    } else {
        return -1;
    }
}

/* ─── Nueva función unificada: ejecuta y devuelve el código de salida ── */
int run_command_get_exit_code(const char *cmd) {
    if (!cmd) return -1;
    char *expanded = expand_command(cmd);
    if (!expanded) return -1;

    int code;
    if (expanded[0] == '!' && expanded[strlen(expanded)-1] == '!') {
        char *trimmed = strdup(expanded + 1);
        trimmed[strlen(trimmed)-1] = '\0';
        code = execute_embedded(trimmed);
        free(trimmed);
    } else {
        code = run_shell_command(expanded);
    }
    free(expanded);
    return code;
}
