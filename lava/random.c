/*
 * Librería Lava: random
 * Módulo de números aleatorios al estilo del módulo `random` de Python.
 *
 * Uso desde Infernal:
 *
 *   import random
 *
 *   print random.random()                  # float en [0.0, 1.0)
 *   print random.randint(1, 6)             # int en [1, 6]
 *   print random.randrange(0, 100, 5)      # int en {0, 5, ..., 95}
 *   print random.uniform(0.0, 1.0)         # float
 *   print random.choice([10, 20, 30])      # un elemento
 *   print random.shuffle([1, 2, 3, 4, 5])  # nueva lista barajada
 *   print random.sample([1, 2, 3, 4, 5], 3)# 3 elementos únicos
 *   random.seed(42)                        # fija la semilla
*/

#include "lava.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------
 * Estado del generador
 * ------------------------------------------------------------------ */

static int g_seeded = 0;

static void ensure_seeded(void) {
    if (!g_seeded) {
        srand((unsigned)time(NULL) ^ ((unsigned)rand() << 16));
        g_seeded = 1;
    }
}

/* ------------------------------------------------------------------
 * seed(n) — fija la semilla
 * ------------------------------------------------------------------ */

LAVA_EXPORT void lava_seed(int n) {
    srand((unsigned)n);
    g_seeded = 1;
    /* Devolvemos null (bool true para que sea verificable). */
    infernal_return_type("bool");
    infernal_return_value("%d", 1);
}

/* ------------------------------------------------------------------
 * random() — float en [0.0, 1.0)
 * ------------------------------------------------------------------ */

LAVA_EXPORT void lava_random(void) {
    ensure_seeded();
    double x = (double)rand() / ((double)RAND_MAX + 1.0);
    infernal_return_type("float");
    infernal_return_value("%f", x);
}

/* ------------------------------------------------------------------
 * randint(a, b) — int en [a, b] ambos incluidos
 * ------------------------------------------------------------------ */

LAVA_EXPORT void lava_randint(int a, int b) {
    ensure_seeded();
    if (a > b) { int t = a; a = b; b = t; }
    int range = b - a + 1;
    if (range <= 0) {
        infernal_return_type("int");
        infernal_return_value("%d", a);
        return;
    }
    int r = a + rand() % range;
    infernal_return_type("int");
    infernal_return_value("%d", r);
}

/* ------------------------------------------------------------------
 * randrange(start, stop, step)
 *   - start < stop y step > 0  → {start, start+step, ...} < stop
 *   - start > stop y step < 0  → {start, start+step, ...} > stop
 *   - step == 0                → error lógico, devuelve 0
 * ------------------------------------------------------------------ */

LAVA_EXPORT void lava_randrange(int start, int stop, int step) {
    ensure_seeded();
    if (step == 0) {
        infernal_return_type("int");
        infernal_return_value("%d", 0);
        return;
    }

    int count;
    if (step > 0) {
        if (start >= stop) {
            infernal_return_type("int");
            infernal_return_value("%d", 0);
            return;
        }
        count = (stop - start + step - 1) / step;
    } else {
        if (start <= stop) {
            infernal_return_type("int");
            infernal_return_value("%d", 0);
            return;
        }
        int abs_step = -step;
        count = (start - stop + abs_step - 1) / abs_step;
    }

    int i = rand() % count;
    int r = start + i * step;
    infernal_return_type("int");
    infernal_return_value("%d", r);
}

/* ------------------------------------------------------------------
 * uniform(a, b) — float en [a, b)
 * ------------------------------------------------------------------ */

LAVA_EXPORT void lava_uniform(double a, double b) {
    ensure_seeded();
    double x = (double)rand() / ((double)RAND_MAX + 1.0);
    infernal_return_type("float");
    infernal_return_value("%f", a + (b - a) * x);
}

/* ------------------------------------------------------------------
 * choice(list) — un elemento aleatorio
 *
 * Si la lista está vacía, devuelve null (int 0). Igual que Python
 * lanzaría IndexError; aquí es más simple devolver un valor nulo.
 * ------------------------------------------------------------------ */

LAVA_EXPORT void lava_choice(lava_list *l) {
    ensure_seeded();
    int n = lava_list_len(l);
    if (n <= 0) {
        infernal_return_type("int");
        infernal_return_value("%d", 0);
        return;
    }
    int i = (rand() % n) + 1;
    const char *t = lava_list_element_type(l, i);
    if (!t) {
        infernal_return_type("int");
        infernal_return_value("%d", 0);
    } else if (strcmp(t, "int") == 0) {
        infernal_return_type("int");
        infernal_return_value("%d", lava_list_int(l, i));
    } else if (strcmp(t, "float") == 0) {
        infernal_return_type("float");
        infernal_return_value("%f", lava_list_float(l, i));
    } else if (strcmp(t, "string") == 0) {
        infernal_return_type("string");
        infernal_return_value("%s", lava_list_string(l, i));
    } else if (strcmp(t, "bool") == 0) {
        infernal_return_type("bool");
        infernal_return_value("%d", lava_list_bool(l, i));
    } else {
        /* Listas/mapas anidados: no soportado por la API de choice. */
        infernal_return_type("int");
        infernal_return_value("%d", 0);
    }
}

/* ------------------------------------------------------------------
 * shuffle(list) — nueva lista con los elementos barajados
 * ------------------------------------------------------------------ */

typedef struct {
    int kind;      /* 0 = otros, 1 = int, 2 = float, 3 = string, 4 = bool */
    int i;
    double f;
    char *s;
    int b;
} tmp_elem;

LAVA_EXPORT void lava_shuffle(lava_list *l) {
    ensure_seeded();
    int n = lava_list_len(l);

    tmp_elem *buf = NULL;
    if (n > 0) {
        buf = malloc((size_t)n * sizeof(tmp_elem));
        if (!buf) {
            infernal_return_list(NULL);
            return;
        }
    }

    for (int i = 0; i < n; i++) {
        buf[i].kind = 0;
        buf[i].s = NULL;
        const char *t = lava_list_element_type(l, i + 1);
        if (!t) continue;
        if (strcmp(t, "int") == 0) {
            buf[i].kind = 1;
            buf[i].i = lava_list_int(l, i + 1);
        } else if (strcmp(t, "float") == 0) {
            buf[i].kind = 2;
            buf[i].f = lava_list_float(l, i + 1);
        } else if (strcmp(t, "string") == 0) {
            buf[i].kind = 3;
            const char *s = lava_list_string(l, i + 1);
            buf[i].s = strdup(s ? s : "");
        } else if (strcmp(t, "bool") == 0) {
            buf[i].kind = 4;
            buf[i].b = lava_list_bool(l, i + 1);
        }
    }

    /* Fisher-Yates */
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        tmp_elem tmp = buf[i];
        buf[i] = buf[j];
        buf[j] = tmp;
    }

    lava_list *out = lava_list_create();
    if (!out) {
        for (int i = 0; i < n; i++) free(buf[i].s);
        free(buf);
        infernal_return_list(NULL);
        return;
    }

    for (int i = 0; i < n; i++) {
        switch (buf[i].kind) {
            case 1: lava_list_add_int(out, buf[i].i); break;
            case 2: lava_list_add_float(out, buf[i].f); break;
            case 3: lava_list_add_string(out, buf[i].s); free(buf[i].s); break;
            case 4: lava_list_add_bool(out, buf[i].b); break;
            default: lava_list_add_int(out, 0); break;
        }
    }
    free(buf);
    infernal_return_list(out);
}

/* ------------------------------------------------------------------
 * sample(list, k) — k elementos únicos aleatorios de la lista
 * ------------------------------------------------------------------ */

LAVA_EXPORT void lava_sample(lava_list *l, int k) {
    ensure_seeded();
    int n = lava_list_len(l);
    if (k < 0) k = 0;
    if (k > n) k = n;

    if (k == 0) {
        lava_list *out = lava_list_create();
        infernal_return_list(out);
        return;
    }

    int *idx = malloc((size_t)n * sizeof(int));
    if (!idx) {
        infernal_return_list(NULL);
        return;
    }
    for (int i = 0; i < n; i++) idx[i] = i + 1;

    /* Fisher-Yates parcial: solo necesitamos los k primeros. */
    for (int i = 0; i < k; i++) {
        int j = i + rand() % (n - i);
        int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }

    lava_list *out = lava_list_create();
    if (!out) {
        free(idx);
        infernal_return_list(NULL);
        return;
    }

    for (int i = 0; i < k; i++) {
        int pos = idx[i];
        const char *t = lava_list_element_type(l, pos);
        if (!t) {
            lava_list_add_int(out, 0);
        } else if (strcmp(t, "int") == 0) {
            lava_list_add_int(out, lava_list_int(l, pos));
        } else if (strcmp(t, "float") == 0) {
            lava_list_add_float(out, lava_list_float(l, pos));
        } else if (strcmp(t, "string") == 0) {
            lava_list_add_string(out, lava_list_string(l, pos));
        } else if (strcmp(t, "bool") == 0) {
            lava_list_add_bool(out, lava_list_bool(l, pos));
        } else {
            lava_list_add_int(out, 0);
        }
    }

    free(idx);
    infernal_return_list(out);
}

/* ------------------------------------------------------------------
 * Registro
 * ------------------------------------------------------------------ */

LAVA_MODULE {
    LAVA_REGISTER("seed",      lava_seed,      "i");
    LAVA_REGISTER("random",    lava_random,    "");
    LAVA_REGISTER("randint",   lava_randint,   "ii");
    LAVA_REGISTER("randrange", lava_randrange, "iii");
    LAVA_REGISTER("uniform",   lava_uniform,   "ff");
    LAVA_REGISTER("choice",    lava_choice,    "l");
    LAVA_REGISTER("shuffle",   lava_shuffle,   "l");
    LAVA_REGISTER("sample",    lava_sample,    "li");
}
