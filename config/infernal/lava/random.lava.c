/*
 * Librería Lava para Infernal base: random
 *
 * Este software se distribuye bajo la licencia Apache 2.0
 * Copyright (C) 2026, David Baña Szymaniak
 *
 * Uso desde Infernal:
 *
 *   import random
 *
 *   random.seed(42)                           # fija la semilla (opcional)
 *
 *   print(random.random())                    # float en [0.0, 1.0)
 *   print(random.randint(1, 6))               # int en [1, 6]
 *   print(random.randrange(0, 100, 5))        # int en {0, 5, ..., 95}
 *   print(random.uniform(0.0, 1.0))           # float en [0.0, 1.0)
 *   print(random.gauss(0.0, 1.0))             # float normal N(0, 1)
 *   print(random.expovariate(1.5))            # float exponencial
 *   print(random.getrandbits(16))             # int con 16 bits aleatorios
 *   print(random.randbytes(8))                # string hex de 8 bytes
 *
 *   print(random.choice([10, 20, 30]))        # un elemento
 *   print(random.choices([1, 2, 3], 5))       # 5 elementos con reemplazo
 *   print(random.shuffle([1, 2, 3, 4, 5]))    # nueva lista barajada
 *   print(random.sample([1, 2, 3, 4, 5], 3))  # 3 elementos sin repetir
 *
 */

#include "lava.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

/* ==================================================================
 * Dependencias del intérprete (visibles gracias a -rdynamic)
 * ================================================================== */

extern int current_eval_line;
extern void error(int line, const char *fmt, ...) __attribute__((noreturn));

/* ==================================================================
 * PRNG: xoshiro256** + SplitMix64 para la siembra
 *
 * Referencia: https://prng.di.unimi.it/xoshiro256starstar.c
 * Dominio público (CC0). David Blackman y Sebastiano Vigna.
 * ================================================================== */

static uint64_t prng_s[4];
static int      prng_seeded = 0;

static inline uint64_t rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static inline uint64_t prng_next_u64(void) {
    const uint64_t result = rotl64(prng_s[1] * 5, 7) * 9;
    const uint64_t t = prng_s[1] << 17;
    prng_s[2] ^= prng_s[0];
    prng_s[3] ^= prng_s[1];
    prng_s[1] ^= prng_s[2];
    prng_s[0] ^= prng_s[3];
    prng_s[2] ^= t;
    prng_s[3] = rotl64(prng_s[3], 45);
    return result;
}

static inline uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void prng_seed_u64(uint64_t seed) {
    uint64_t x = seed;
    for (int i = 0; i < 4; i++) prng_s[i] = splitmix64(&x);
    prng_seeded = 1;
}

static uint64_t entropy_from_os(void) {
    uint64_t buf = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, &buf, sizeof(buf));
        close(fd);
        if (n == (ssize_t)sizeof(buf)) return buf;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t mix = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    mix ^= ((uint64_t)getpid() << 32);
    mix ^= (uint64_t)(uintptr_t)&buf;
    return mix;
}

static void ensure_seeded(void) {
    if (!prng_seeded) prng_seed_u64(entropy_from_os());
}

/* Entero uniforme en [0, bound) sin sesgo de módulo. */
static uint64_t prng_bounded(uint64_t bound) {
    if (bound <= 1) return 0;
    uint64_t threshold = (uint64_t)(-bound) % bound;
    for (;;) {
        uint64_t r = prng_next_u64();
        if (r >= threshold) return r % bound;
    }
}

/* Float uniforme en [0.0, 1.0) con 53 bits de entropía. */
static double prng_double(void) {
    return (double)(prng_next_u64() >> 11) * (1.0 / 9007199254740992.0);
}

/* Gaussiana estándar (Box-Muller con caché). */
static int    gauss_has_spare = 0;
static double gauss_spare     = 0.0;

static double prng_gauss(void) {
    if (gauss_has_spare) {
        gauss_has_spare = 0;
        return gauss_spare;
    }
    double u1, u2, s;
    do {
        u1 = 2.0 * prng_double() - 1.0;
        u2 = 2.0 * prng_double() - 1.0;
        s = u1 * u1 + u2 * u2;
    } while (s >= 1.0 || s == 0.0);
    double f = sqrt(-2.0 * log(s) / s);
    gauss_spare = u2 * f;
    gauss_has_spare = 1;
    return u1 * f;
}

/* ==================================================================
 * Helpers
 * ================================================================== */

static void raise(const char *fmt, ...) __attribute__((noreturn));

static void raise(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    error(current_eval_line, "%s", buf);
}

/* Copia un elemento (int/float/string/bool/list) de src[idx] a dst.
 * Devuelve 0 si OK, -1 si el tipo no está soportado. */
static int copy_element(lava_list *dst, lava_list *src, int idx) {
    const char *t = lava_list_element_type(src, idx);
    if (!t) return -1;
    if      (strcmp(t, "int")    == 0) lava_list_add_int   (dst, lava_list_int   (src, idx));
    else if (strcmp(t, "float")  == 0) lava_list_add_float (dst, lava_list_float (src, idx));
    else if (strcmp(t, "string") == 0) lava_list_add_string(dst, lava_list_string(src, idx));
    else if (strcmp(t, "bool")   == 0) lava_list_add_bool  (dst, lava_list_bool  (src, idx));
    else if (strcmp(t, "list")   == 0) lava_list_add_list  (dst, lava_list_list  (src, idx));
    else return -1;
    return 0;
}

/* Verifica que todos los elementos sean copiables. Lanza error() si no. */
static void check_all_supported(lava_list *l, const char *fname) {
    int n = lava_list_len(l);
    for (int i = 1; i <= n; i++) {
        const char *t = lava_list_element_type(l, i);
        if (!t) raise("random.%s(): elemento sin tipo en posición %d", fname, i);
        if (strcmp(t, "int") == 0 || strcmp(t, "float") == 0 ||
            strcmp(t, "string") == 0 || strcmp(t, "bool") == 0 ||
            strcmp(t, "list") == 0) continue;
        raise("random.%s(): tipo '%s' no soportado en la posición %d "
        "(solo int, float, string, bool, list)", fname, t, i);
    }
}

/* Baraja in-place los items de un Value que ya sabemos que es lista. */
static void shuffle_value_items(lava_list *l) {
    Value *v = (Value *)l;
    int n = v->data.list.count;
    for (int i = n - 1; i > 0; i--) {
        int j = (int)prng_bounded((uint64_t)(i + 1));
        Value tmp         = v->data.list.items[i];
        v->data.list.items[i] = v->data.list.items[j];
        v->data.list.items[j] = tmp;
    }
}

/* ==================================================================
 * API pública
 * ================================================================== */

/* seed(n) — fija la semilla. Cualquier int vale, incluido 0. */
LAVA_EXPORT void lava_seed(int n) {
    prng_seed_u64((uint64_t)(uint32_t)n);
    infernal_return_type("bool");
    infernal_return_value("%d", 1);
}

/* random() — float uniforme en [0.0, 1.0). */
LAVA_EXPORT void lava_random(void) {
    ensure_seeded();
    infernal_return_type("float");
    infernal_return_value("%f", prng_double());
}

/* randint(a, b) — int uniforme en [a, b], ambos incluidos.
 * Si a > b se intercambian (comportamiento permisivo). */
LAVA_EXPORT void lava_randint(int a, int b) {
    ensure_seeded();
    if (a > b) { int t = a; a = b; b = t; }
    uint64_t range = (uint64_t)((int64_t)b - (int64_t)a) + 1ULL;
    infernal_return_type("int");
    infernal_return_value("%d", a + (int)prng_bounded(range));
}

/* randrange(start, stop, step) — como range() pero escoge uno al azar.
 *   step > 0: {start, start+step, ..., < stop}
 *   step < 0: {start, start+step, ..., > stop}
 * Lanza error si el rango está vacío o step == 0. */
LAVA_EXPORT void lava_randrange(int start, int stop, int step) {
    ensure_seeded();

    if (step == 0)
        raise("random.randrange(): step no puede ser 0");

    if (step > 0 && start >= stop)
        raise("random.randrange(): rango vacío (start=%d >= stop=%d, step=%d)",
              start, stop, step);

    if (step < 0 && start <= stop)
        raise("random.randrange(): rango vacío (start=%d <= stop=%d, step=%d)",
              start, stop, step);

    int64_t n;
    if (step > 0) {
        n = ((int64_t)stop - (int64_t)start + step - 1) / step;
    } else {
        int64_t abs_step = -(int64_t)step;
        n = ((int64_t)start - (int64_t)stop + abs_step - 1) / abs_step;
    }

    int64_t idx = (int64_t)prng_bounded((uint64_t)n);
    infernal_return_type("int");
    infernal_return_value("%d", start + (int)(idx * step));
}

/* uniform(a, b) — float uniforme en [a, b). */
LAVA_EXPORT void lava_uniform(double a, double b) {
    ensure_seeded();
    infernal_return_type("float");
    infernal_return_value("%f", a + (b - a) * prng_double());
}

/* gauss(mu, sigma) — float con distribución normal N(mu, sigma).
 * Si sigma <= 0 se usa 1.0. */
LAVA_EXPORT void lava_gauss(double mu, double sigma) {
    ensure_seeded();
    if (sigma <= 0.0) sigma = 1.0;
    infernal_return_type("float");
    infernal_return_value("%f", mu + sigma * prng_gauss());
}

/* expovariate(lambd) — float con distribución exponencial de tasa lambd. */
LAVA_EXPORT void lava_expovariate(double lambd) {
    ensure_seeded();
    if (lambd <= 0.0)
        raise("random.expovariate(): lambd debe ser > 0 (recibido %g)", lambd);
    double u = 1.0 - prng_double();       /* en (0, 1] */
    infernal_return_type("float");
    infernal_return_value("%f", -log(u) / lambd);
}

/* getrandbits(k) — entero no negativo con k bits aleatorios (0 <= k <= 31). */
LAVA_EXPORT void lava_getrandbits(int k) {
    ensure_seeded();
    if (k < 0 || k > 31)
        raise("random.getrandbits(): k debe estar entre 0 y 31 (recibido %d)", k);
    uint32_t v = (k == 0) ? 0u : (uint32_t)(prng_next_u64() >> (64 - k));
    infernal_return_type("int");
    infernal_return_value("%d", (int)v);
}

/* randbytes(n) — string hex de 2*n caracteres con n bytes aleatorios. */
LAVA_EXPORT void lava_randbytes(int n) {
    ensure_seeded();
    if (n < 0)     raise("random.randbytes(): n no puede ser negativo");
    if (n > 4096)  raise("random.randbytes(): n demasiado grande (máx 4096)");

    static const char hex[] = "0123456789abcdef";
    char *buf = malloc((size_t)n * 2 + 1);
    if (!buf) raise("random.randbytes(): memoria insuficiente");

    for (int i = 0; i < n; i++) {
        uint8_t byte = (uint8_t)prng_next_u64();
        buf[2*i]     = hex[byte >> 4];
        buf[2*i + 1] = hex[byte & 0x0F];
    }
    buf[2*n] = '\0';

    infernal_return_type("string");
    infernal_return_value("%s", buf);
    free(buf);
}

/* choice(list) — un elemento aleatorio. Lanza error si está vacía. */
LAVA_EXPORT void lava_choice(lava_list *l) {
    ensure_seeded();
    int n = lava_list_len(l);
    if (n <= 0) raise("random.choice(): la lista está vacía");

    int i = (int)prng_bounded((uint64_t)n) + 1;   /* base 1 */
    const char *t = lava_list_element_type(l, i);
    if (!t) raise("random.choice(): no se pudo determinar el tipo del elemento");

    if      (strcmp(t, "int")    == 0) {
        infernal_return_type("int");
        infernal_return_value("%d", lava_list_int(l, i));
    } else if (strcmp(t, "float")  == 0) {
        infernal_return_type("float");
        infernal_return_value("%f", lava_list_float(l, i));
    } else if (strcmp(t, "string") == 0) {
        infernal_return_type("string");
        infernal_return_value("%s", lava_list_string(l, i));
    } else if (strcmp(t, "bool")   == 0) {
        infernal_return_type("bool");
        infernal_return_value("%d", lava_list_bool(l, i));
    } else {
        raise("random.choice(): tipo '%s' no soportado "
        "(solo int, float, string, bool)", t);
    }
}

/* choices(list, k) — k elementos CON reemplazo. */
LAVA_EXPORT void lava_choices(lava_list *l, int k) {
    ensure_seeded();
    if (k < 0) raise("random.choices(): k no puede ser negativo");
    int n = lava_list_len(l);
    if (n <= 0 && k > 0) raise("random.choices(): la lista está vacía");
    check_all_supported(l, "choices");

    lava_list *out = lava_list_create();
    if (!out) raise("random.choices(): memoria insuficiente");

    for (int i = 0; i < k; i++) {
        int idx = (int)prng_bounded((uint64_t)n) + 1;
        copy_element(out, l, idx);
    }
    infernal_return_list(out);
}

/* shuffle(list) — nueva lista con los elementos barajados. */
LAVA_EXPORT void lava_shuffle(lava_list *l) {
    ensure_seeded();
    check_all_supported(l, "shuffle");

    int n = lava_list_len(l);
    lava_list *out = lava_list_create();
    if (!out) raise("random.shuffle(): memoria insuficiente");

    for (int i = 1; i <= n; i++) copy_element(out, l, i);
    shuffle_value_items(out);

    infernal_return_list(out);
}

/* sample(list, k) — k elementos únicos aleatorios. */
LAVA_EXPORT void lava_sample(lava_list *l, int k) {
    ensure_seeded();
    int n = lava_list_len(l);
    if (k < 0) raise("random.sample(): k no puede ser negativo");
    if (k > n) raise("random.sample(): k=%d > tamaño de la lista (%d)", k, n);
    check_all_supported(l, "sample");

    if (k == 0) {
        lava_list *out = lava_list_create();
        infernal_return_list(out);
        return;
    }

    int *idx = malloc((size_t)n * sizeof(int));
    if (!idx) raise("random.sample(): memoria insuficiente");
    for (int i = 0; i < n; i++) idx[i] = i + 1;

    /* Fisher-Yates parcial: solo los k primeros. */
    for (int i = 0; i < k; i++) {
        int j = i + (int)prng_bounded((uint64_t)(n - i));
        int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }

    lava_list *out = lava_list_create();
    if (!out) { free(idx); raise("random.sample(): memoria insuficiente"); }

    for (int i = 0; i < k; i++) copy_element(out, l, idx[i]);
    free(idx);
    infernal_return_list(out);
}

/* ==================================================================
 * Registro
 * ================================================================== */

LAVA_MODULE {
    LAVA_REGISTER("seed",         lava_seed,         "i");
    LAVA_REGISTER("random",       lava_random,       "");
    LAVA_REGISTER("randint",      lava_randint,      "ii");
    LAVA_REGISTER("randrange",    lava_randrange,    "iii");
    LAVA_REGISTER("uniform",      lava_uniform,      "ff");
    LAVA_REGISTER("gauss",        lava_gauss,        "ff");
    LAVA_REGISTER("expovariate",  lava_expovariate,  "f");
    LAVA_REGISTER("getrandbits",  lava_getrandbits,  "i");
    LAVA_REGISTER("randbytes",    lava_randbytes,    "i");
    LAVA_REGISTER("choice",       lava_choice,       "l");
    LAVA_REGISTER("choices",      lava_choices,      "li");
    LAVA_REGISTER("shuffle",      lava_shuffle,      "l");
    LAVA_REGISTER("sample",       lava_sample,       "li");
}
