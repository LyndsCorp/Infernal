/*
 * Librería Lava para Infernal base: math
 *
 * Este software se distribuye bajo la licencia Apache 2.0
 * Copyright (C) 2026, David Baña Szymaniak
 *
 * Uso desde Infernal:
 *
 *   import math
 *
 *   print(math.pi())                          # 3.14159...
 *   print(math.e())                           # 2.71828...
 *   print(math.tau())                         # 6.28318...
 *
 *   print(math.sqrt(2.0))                     # 1.41421...
 *   print(math.cbrt(27.0))                    # 3.0
 *   print(math.pow(2.0, 10.0))                # 1024.0
 *   print(math.exp(1.0))                      # 2.71828...
 *   print(math.log(math.e()))                 # 1.0
 *   print(math.log2(8.0))                     # 3.0
 *   print(math.log10(1000.0))                 # 3.0
 *   print(math.log_base(81.0, 3.0))           # 4.0
 *
 *   print(math.floor(3.7))                    # 3
 *   print(math.ceil(3.2))                     # 4
 *   print(math.trunc(-3.7))                   # -3
 *   print(math.round(2.5))                    # 2  (half-to-even)
 *   print(math.round(3.5))                    # 4
 *   print(math.fabs(-7.5))                    # 7.5
 *
 *   print(math.sin(math.pi() / 2.0))          # 1.0
 *   print(math.cos(0.0))                      # 1.0
 *   print(math.atan2(1.0, 1.0))               # 0.78539...
 *   print(math.degrees(math.pi()))            # 180.0
 *   print(math.radians(180.0))                # 3.14159...
 *
 *   print(math.gcd(48, 18))                   # 6
 *   print(math.lcm(4, 6))                     # 12
 *   print(math.factorial(10))                 # 3628800
 *   print(math.isqrt(50))                     # 7
 *   print(math.comb(5, 2))                    # 10
 *   print(math.perm(5, 2))                    # 20
 *
 *   print(math.hypot(3.0, 4.0))               # 5.0
 *   print(math.dist(0.0, 0.0, 3.0, 4.0))      # 5.0
 *   print(math.fsum([1.0, 2.0, 3.0]))         # 6.0
 *   print(math.prod([1.0, 2.0, 3.0, 4.0]))    # 24.0
 *
 *   print(math.isclose(0.1 + 0.2, 0.3))       # true
 *   print(math.isnan(math.nan()))             # true
 *   print(math.isinf(math.inf()))             # true
 *
 *   print(math.modf(3.75))                    # [0.75, 3.0]
 *   print(math.frexp(8.0))                    # [0.5, 4]
 *   print(math.ldexp(0.5, 4))                 # 8.0
 *
*/

#include "lava.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <errno.h>

/* ==================================================================
 * Dependencias del intérprete (visibles gracias a -rdynamic)
 * ================================================================== */

extern int current_eval_line;
extern void error(int line, const char *fmt, ...) __attribute__((noreturn));

/* ==================================================================
 * Helper de errores
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

/* Límites del int de Infernal (32 bits con signo). */
#define LAVA_INT_MAX  2147483647LL
#define LAVA_INT_MIN (-2147483647LL - 1LL)

static void check_int_range(int64_t v, const char *fname) {
    if (v > LAVA_INT_MAX || v < LAVA_INT_MIN)
        raise("math.%s(): resultado fuera de rango de int (32 bits)", fname);
}

/* ==================================================================
 * Constantes
 * ================================================================== */

/* pi() — 3.141592653589793... */
LAVA_EXPORT void lava_pi(void) {
    infernal_return_type("float");
    infernal_return_value("%f", 3.14159265358979323846);
}

/* e() — 2.718281828459045... */
LAVA_EXPORT void lava_e(void) {
    infernal_return_type("float");
    infernal_return_value("%f", 2.71828182845904523536);
}

/* tau() — 2 * pi = 6.283185307179586... */
LAVA_EXPORT void lava_tau(void) {
    infernal_return_type("float");
    infernal_return_value("%f", 6.28318530717958647692);
}

/* inf() — infinito positivo. */
LAVA_EXPORT void lava_inf(void) {
    infernal_return_type("float");
    infernal_return_value("%f", (double)INFINITY);
}

/* nan() — Not-a-Number. */
LAVA_EXPORT void lava_nan(void) {
    infernal_return_type("float");
    infernal_return_value("%f", (double)NAN);
}

/* ==================================================================
 * Operaciones básicas
 * ================================================================== */

/* fabs(x) — valor absoluto de un float. */
LAVA_EXPORT void lava_fabs(double x) {
    infernal_return_type("float");
    infernal_return_value("%f", fabs(x));
}

/* floor(x) — mayor entero <= x. */
LAVA_EXPORT void lava_floor(double x) {
    double r = floor(x);
    if (r > (double)LAVA_INT_MAX || r < (double)LAVA_INT_MIN || isnan(r))
        raise("math.floor(): resultado fuera de rango de int");
    infernal_return_type("int");
    infernal_return_value("%d", (int)r);
}

/* ceil(x) — menor entero >= x. */
LAVA_EXPORT void lava_ceil(double x) {
    double r = ceil(x);
    if (r > (double)LAVA_INT_MAX || r < (double)LAVA_INT_MIN || isnan(r))
        raise("math.ceil(): resultado fuera de rango de int");
    infernal_return_type("int");
    infernal_return_value("%d", (int)r);
}

/* trunc(x) — parte entera truncada hacia cero. */
LAVA_EXPORT void lava_trunc(double x) {
    double r = trunc(x);
    if (r > (double)LAVA_INT_MAX || r < (double)LAVA_INT_MIN || isnan(r))
        raise("math.trunc(): resultado fuera de rango de int");
    infernal_return_type("int");
    infernal_return_value("%d", (int)r);
}

/* round(x) — redondeo "half to even" (igual que Python). */
LAVA_EXPORT void lava_round(double x) {
    if (isnan(x) || isinf(x))
        raise("math.round(): argumento no finito");
    double r = nearbyint(x);
    if (r > (double)LAVA_INT_MAX || r < (double)LAVA_INT_MIN)
        raise("math.round(): resultado fuera de rango de int");
    infernal_return_type("int");
    infernal_return_value("%d", (int)r);
}

/* copysign(x, y) — magnitud de x con el signo de y. */
LAVA_EXPORT void lava_copysign(double x, double y) {
    infernal_return_type("float");
    infernal_return_value("%f", copysign(x, y));
}

/* ==================================================================
 * Potencias y logaritmos
 * ================================================================== */

/* sqrt(x) — raíz cuadrada. */
LAVA_EXPORT void lava_sqrt(double x) {
    if (x < 0.0) raise("math.sqrt(): dominio inválido (x=%g < 0)", x);
    infernal_return_type("float");
    infernal_return_value("%f", sqrt(x));
}

/* cbrt(x) — raíz cúbica (acepta negativos). */
LAVA_EXPORT void lava_cbrt(double x) {
    infernal_return_type("float");
    infernal_return_value("%f", cbrt(x));
}

/* pow(x, y) — x elevado a y. */
LAVA_EXPORT void lava_pow(double x, double y) {
    errno = 0;
    double r = pow(x, y);
    if (errno == EDOM)   raise("math.pow(): dominio inválido (%g, %g)", x, y);
    if (errno == ERANGE) raise("math.pow(): overflow/underflow (%g, %g)", x, y);
    infernal_return_type("float");
    infernal_return_value("%f", r);
}

/* exp(x) — e^x. */
LAVA_EXPORT void lava_exp(double x) {
    errno = 0;
    double r = exp(x);
    if (errno == ERANGE) raise("math.exp(): overflow (%g)", x);
    infernal_return_type("float");
    infernal_return_value("%f", r);
}

/* expm1(x) — e^x - 1 (preciso cerca de 0). */
LAVA_EXPORT void lava_expm1(double x) {
    infernal_return_type("float");
    infernal_return_value("%f", expm1(x));
}

/* exp2(x) — 2^x. */
LAVA_EXPORT void lava_exp2(double x) {
    infernal_return_type("float");
    infernal_return_value("%f", exp2(x));
}

/* log(x) — logaritmo natural. */
LAVA_EXPORT void lava_log(double x) {
    if (x <= 0.0) raise("math.log(): x debe ser > 0 (recibido %g)", x);
    infernal_return_type("float");
    infernal_return_value("%f", log(x));
}

/* log2(x) — logaritmo base 2. */
LAVA_EXPORT void lava_log2(double x) {
    if (x <= 0.0) raise("math.log2(): x debe ser > 0 (recibido %g)", x);
    infernal_return_type("float");
    infernal_return_value("%f", log2(x));
}

/* log10(x) — logaritmo base 10. */
LAVA_EXPORT void lava_log10(double x) {
    if (x <= 0.0) raise("math.log10(): x debe ser > 0 (recibido %g)", x);
    infernal_return_type("float");
    infernal_return_value("%f", log10(x));
}

/* log1p(x) — log(1 + x) (preciso cerca de 0). */
LAVA_EXPORT void lava_log1p(double x) {
    if (x <= -1.0) raise("math.log1p(): x debe ser > -1 (recibido %g)", x);
    infernal_return_type("float");
    infernal_return_value("%f", log1p(x));
}

/* log_base(x, b) — logaritmo de x en base b. */
LAVA_EXPORT void lava_log_base(double x, double base) {
    if (x    <= 0.0) raise("math.log_base(): x debe ser > 0 (recibido %g)", x);
    if (base <= 0.0) raise("math.log_base(): base debe ser > 0 (recibido %g)", base);
    if (base == 1.0) raise("math.log_base(): base no puede ser 1");
    infernal_return_type("float");
    infernal_return_value("%f", log(x) / log(base));
}

/* ==================================================================
 * Trigonometría
 * ================================================================== */

LAVA_EXPORT void lava_sin(double x) {
    infernal_return_type("float");
    infernal_return_value("%f", sin(x));
}

LAVA_EXPORT void lava_cos(double x) {
    infernal_return_type("float");
    infernal_return_value("%f", cos(x));
}

LAVA_EXPORT void lava_tan(double x) {
    infernal_return_type("float");
    infernal_return_value("%f", tan(x));
}

LAVA_EXPORT void lava_asin(double x) {
    if (x < -1.0 || x > 1.0)
        raise("math.asin(): x fuera de [-1, 1] (recibido %g)", x);
    infernal_return_type("float");
    infernal_return_value("%f", asin(x));
}

LAVA_EXPORT void lava_acos(double x) {
    if (x < -1.0 || x > 1.0)
        raise("math.acos(): x fuera de [-1, 1] (recibido %g)", x);
    infernal_return_type("float");
    infernal_return_value("%f", acos(x));
}

LAVA_EXPORT void lava_atan(double x) {
    infernal_return_type("float");
    infernal_return_value("%f", atan(x));
}

/* atan2(y, x) — ángulo del punto (x, y) respecto al eje +X. */
LAVA_EXPORT void lava_atan2(double y, double x) {
    infernal_return_type("float");
    infernal_return_value("%f", atan2(y, x));
}

/* degrees(r) — radianes a grados. */
LAVA_EXPORT void lava_degrees(double r) {
    infernal_return_type("float");
    infernal_return_value("%f", r * (180.0 / 3.14159265358979323846));
}

/* radians(d) — grados a radianes. */
LAVA_EXPORT void lava_radians(double d) {
    infernal_return_type("float");
    infernal_return_value("%f", d * (3.14159265358979323846 / 180.0));
}

/* ==================================================================
 * Trigonometría hiperbólica
 * ================================================================== */

LAVA_EXPORT void lava_sinh(double x) {
    infernal_return_type("float");
    infernal_return_value("%f", sinh(x));
}

LAVA_EXPORT void lava_cosh(double x) {
    infernal_return_type("float");
    infernal_return_value("%f", cosh(x));
}

LAVA_EXPORT void lava_tanh(double x) {
    infernal_return_type("float");
    infernal_return_value("%f", tanh(x));
}

LAVA_EXPORT void lava_asinh(double x) {
    infernal_return_type("float");
    infernal_return_value("%f", asinh(x));
}

LAVA_EXPORT void lava_acosh(double x) {
    if (x < 1.0) raise("math.acosh(): x debe ser >= 1 (recibido %g)", x);
    infernal_return_type("float");
    infernal_return_value("%f", acosh(x));
}

LAVA_EXPORT void lava_atanh(double x) {
    if (x <= -1.0 || x >= 1.0)
        raise("math.atanh(): x debe estar en (-1, 1) (recibido %g)", x);
    infernal_return_type("float");
    infernal_return_value("%f", atanh(x));
}

/* ==================================================================
 * Teoría de números (enteros)
 * ================================================================== */

static int64_t i64_gcd(int64_t a, int64_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = b; b = a % b; a = t; }
    return a;
}

/* gcd(a, b) — máximo común divisor. */
LAVA_EXPORT void lava_gcd(int a, int b) {
    infernal_return_type("int");
    infernal_return_value("%d", (int)i64_gcd((int64_t)a, (int64_t)b));
}

/* lcm(a, b) — mínimo común múltiplo (0 si alguno es 0). */
LAVA_EXPORT void lava_lcm(int a, int b) {
    if (a == 0 || b == 0) {
        infernal_return_type("int");
        infernal_return_value("%d", 0);
        return;
    }
    int64_t g = i64_gcd((int64_t)a, (int64_t)b);
    int64_t r = ((int64_t)a / g) * (int64_t)b;
    if (r < 0) r = -r;
    check_int_range(r, "lcm");
    infernal_return_type("int");
    infernal_return_value("%d", (int)r);
}

/* factorial(n) — n! para 0 <= n <= 12 (cabe en int32). */
LAVA_EXPORT void lava_factorial(int n) {
    if (n < 0)  raise("math.factorial(): n no puede ser negativo");
    if (n > 12) raise("math.factorial(): n demasiado grande (máx 12 en int32)");
    int64_t r = 1;
    for (int i = 2; i <= n; i++) r *= i;
    infernal_return_type("int");
    infernal_return_value("%d", (int)r);
}

/* isqrt(n) — raíz cuadrada entera (floor de sqrt). */
LAVA_EXPORT void lava_isqrt(int n) {
    if (n < 0) raise("math.isqrt(): n no puede ser negativo");
    int64_t r = (int64_t)sqrt((double)n);
    while (r > 0 && r * r > n) r--;
    while ((r + 1) * (r + 1) <= (int64_t)n) r++;
    infernal_return_type("int");
    infernal_return_value("%d", (int)r);
}

/* comb(n, k) — coeficiente binomial C(n, k). */
LAVA_EXPORT void lava_comb(int n, int k) {
    if (n < 0 || k < 0) raise("math.comb(): n y k deben ser >= 0");
    if (k > n) {
        infernal_return_type("int");
        infernal_return_value("%d", 0);
        return;
    }
    if (k > n - k) k = n - k;
    int64_t r = 1;
    for (int i = 1; i <= k; i++) {
        r = r * (int64_t)(n - k + i) / (int64_t)i;
    }
    check_int_range(r, "comb");
    infernal_return_type("int");
    infernal_return_value("%d", (int)r);
}

/* perm(n, k) — permutaciones P(n, k) = n! / (n - k)!. */
LAVA_EXPORT void lava_perm(int n, int k) {
    if (n < 0 || k < 0) raise("math.perm(): n y k deben ser >= 0");
    if (k > n) {
        infernal_return_type("int");
        infernal_return_value("%d", 0);
        return;
    }
    int64_t r = 1;
    for (int i = 0; i < k; i++) {
        r *= (int64_t)(n - i);
        check_int_range(r, "perm");
    }
    infernal_return_type("int");
    infernal_return_value("%d", (int)r);
}

/* ==================================================================
 * Funciones especiales
 * ================================================================== */

/* erf(x) — función error. */
LAVA_EXPORT void lava_erf(double x) {
    infernal_return_type("float");
    infernal_return_value("%f", erf(x));
}

/* erfc(x) — 1 - erf(x), estable para x grandes. */
LAVA_EXPORT void lava_erfc(double x) {
    infernal_return_type("float");
    infernal_return_value("%f", erfc(x));
}

/* gamma(x) — función gamma Γ(x). */
LAVA_EXPORT void lava_gamma(double x) {
    if (x <= 0.0 && x == floor(x))
        raise("math.gamma(): polo en x=%g", x);
    infernal_return_type("float");
    infernal_return_value("%f", tgamma(x));
}

/* lgamma(x) — logaritmo natural de |Γ(x)|. */
LAVA_EXPORT void lava_lgamma(double x) {
    if (x <= 0.0 && x == floor(x))
        raise("math.lgamma(): polo en x=%g", x);
    infernal_return_type("float");
    infernal_return_value("%f", lgamma(x));
}

/* ==================================================================
 * Punto flotante (fmod, hypot, frexp, modf, ...)
 * ================================================================== */

/* fmod(x, y) — resto con signo de x (estilo C). */
LAVA_EXPORT void lava_fmod(double x, double y) {
    if (y == 0.0) raise("math.fmod(): división por cero");
    infernal_return_type("float");
    infernal_return_value("%f", fmod(x, y));
}

/* remainder(x, y) — resto redondeado (IEEE 754). */
LAVA_EXPORT void lava_remainder(double x, double y) {
    if (y == 0.0) raise("math.remainder(): división por cero");
    infernal_return_type("float");
    infernal_return_value("%f", remainder(x, y));
}

/* hypot(x, y) — sqrt(x² + y²) sin overflow intermedio. */
LAVA_EXPORT void lava_hypot(double x, double y) {
    infernal_return_type("float");
    infernal_return_value("%f", hypot(x, y));
}

/* fmin(x, y) — mínimo (propaga NaN como Python). */
LAVA_EXPORT void lava_fmin(double x, double y) {
    if (isnan(x)) { infernal_return_type("float"); infernal_return_value("%f", y); return; }
    if (isnan(y)) { infernal_return_type("float"); infernal_return_value("%f", x); return; }
    infernal_return_type("float");
    infernal_return_value("%f", x < y ? x : y);
}

/* fmax(x, y) — máximo (propaga NaN como Python). */
LAVA_EXPORT void lava_fmax(double x, double y) {
    if (isnan(x)) { infernal_return_type("float"); infernal_return_value("%f", y); return; }
    if (isnan(y)) { infernal_return_type("float"); infernal_return_value("%f", x); return; }
    infernal_return_type("float");
    infernal_return_value("%f", x > y ? x : y);
}

/* fdim(x, y) — max(x - y, 0). */
LAVA_EXPORT void lava_fdim(double x, double y) {
    infernal_return_type("float");
    infernal_return_value("%f", fdim(x, y));
}

/* ldexp(x, i) — x * 2^i. */
LAVA_EXPORT void lava_ldexp(double x, int i) {
    infernal_return_type("float");
    infernal_return_value("%f", ldexp(x, i));
}

/* frexp(x) — devuelve [mantisa, exponente] tal que x = m * 2^e, m en [0.5, 1). */
LAVA_EXPORT void lava_frexp(double x) {
    int e = 0;
    double m = frexp(x, &e);
    lava_list *out = lava_list_create();
    if (!out) raise("math.frexp(): memoria insuficiente");
    lava_list_add_float(out, m);
    lava_list_add_int(out, e);
    infernal_return_list(out);
}

/* modf(x) — devuelve [parte_fraccionaria, parte_entera] (ambas float). */
LAVA_EXPORT void lava_modf(double x) {
    double ipart = 0.0;
    double fpart = modf(x, &ipart);
    lava_list *out = lava_list_create();
    if (!out) raise("math.modf(): memoria insuficiente");
    lava_list_add_float(out, fpart);
    lava_list_add_float(out, ipart);
    infernal_return_list(out);
}

/* ==================================================================
 * Predicados
 * ================================================================== */

/* isfinite(x) — true si x no es NaN ni ±inf. */
LAVA_EXPORT void lava_isfinite(double x) {
    infernal_return_type("bool");
    infernal_return_value("%d", isfinite(x) ? 1 : 0);
}

/* isinf(x) — true si x es ±inf. */
LAVA_EXPORT void lava_isinf(double x) {
    infernal_return_type("bool");
    infernal_return_value("%d", isinf(x) ? 1 : 0);
}

/* isnan(x) — true si x es NaN. */
LAVA_EXPORT void lava_isnan(double x) {
    infernal_return_type("bool");
    infernal_return_value("%d", isnan(x) ? 1 : 0);
}

/* isclose(a, b) — con rel_tol=1e-9, abs_tol=0.0 (igual que Python). */
LAVA_EXPORT void lava_isclose(double a, double b) {
    if (isnan(a) || isnan(b)) {
        infernal_return_type("bool");
        infernal_return_value("%d", 0);
        return;
    }
    if (isinf(a) || isinf(b)) {
        infernal_return_type("bool");
        infernal_return_value("%d", (a == b) ? 1 : 0);
        return;
    }
    double diff   = fabs(a - b);
    double mx     = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    double rel    = 1e-9 * mx;
    double abs_t  = 0.0;
    int ok = (diff <= rel) || (diff <= abs_t);
    infernal_return_type("bool");
    infernal_return_value("%d", ok);
}

/* isclose_tol(a, b, rel_tol, abs_tol) — versión con tolerancias. */
LAVA_EXPORT void lava_isclose_tol(double a, double b, double rel_tol, double abs_tol) {
    if (isnan(a) || isnan(b)) {
        infernal_return_type("bool");
        infernal_return_value("%d", 0);
        return;
    }
    if (isinf(a) || isinf(b)) {
        infernal_return_type("bool");
        infernal_return_value("%d", (a == b) ? 1 : 0);
        return;
    }
    double diff = fabs(a - b);
    double mx   = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    int ok = (diff <= rel_tol * mx) || (diff <= abs_tol);
    infernal_return_type("bool");
    infernal_return_value("%d", ok);
}

/* ==================================================================
 * Utilidades sobre listas
 * ================================================================== */

/* fsum(list) — suma precisa de una lista de números. */
LAVA_EXPORT void lava_fsum(lava_list *l) {
    int n = lava_list_len(l);
    double total = 0.0;
    for (int i = 1; i <= n; i++) {
        const char *t = lava_list_element_type(l, i);
        if (!t) raise("math.fsum(): elemento sin tipo en posición %d", i);
        if      (strcmp(t, "int")   == 0) total += (double)lava_list_int(l, i);
        else if (strcmp(t, "float") == 0) total += lava_list_float(l, i);
        else if (strcmp(t, "bool")  == 0) total += (double)lava_list_bool(l, i);
        else raise("math.fsum(): tipo '%s' no soportado en posición %d", t, i);
    }
    infernal_return_type("float");
    infernal_return_value("%f", total);
}

/* prod(list) — producto de una lista de números. */
LAVA_EXPORT void lava_prod(lava_list *l) {
    int n = lava_list_len(l);
    double total = 1.0;
    for (int i = 1; i <= n; i++) {
        const char *t = lava_list_element_type(l, i);
        if (!t) raise("math.prod(): elemento sin tipo en posición %d", i);
        if      (strcmp(t, "int")   == 0) total *= (double)lava_list_int(l, i);
        else if (strcmp(t, "float") == 0) total *= lava_list_float(l, i);
        else if (strcmp(t, "bool")  == 0) total *= (double)lava_list_bool(l, i);
        else raise("math.prod(): tipo '%s' no soportado en posición %d", t, i);
    }
    infernal_return_type("float");
    infernal_return_value("%f", total);
}

/* dist(x1, y1, x2, y2) — distancia euclídea entre (x1, y1) y (x2, y2). */
LAVA_EXPORT void lava_dist(double x1, double y1, double x2, double y2) {
    infernal_return_type("float");
    infernal_return_value("%f", hypot(x2 - x1, y2 - y1));
}

/* ==================================================================
 * Registro del módulo
 * ================================================================== */

LAVA_MODULE {
    /* Constantes */
    LAVA_REGISTER("pi",          lava_pi,          "");
    LAVA_REGISTER("e",           lava_e,           "");
    LAVA_REGISTER("tau",         lava_tau,         "");
    LAVA_REGISTER("inf",         lava_inf,         "");
    LAVA_REGISTER("nan",         lava_nan,         "");

    /* Básicas */
    LAVA_REGISTER("fabs",        lava_fabs,        "f");
    LAVA_REGISTER("floor",       lava_floor,       "f");
    LAVA_REGISTER("ceil",        lava_ceil,        "f");
    LAVA_REGISTER("trunc",       lava_trunc,       "f");
    LAVA_REGISTER("round",       lava_round,       "f");
    LAVA_REGISTER("copysign",    lava_copysign,    "ff");

    /* Potencias y logaritmos */
    LAVA_REGISTER("sqrt",        lava_sqrt,        "f");
    LAVA_REGISTER("cbrt",        lava_cbrt,        "f");
    LAVA_REGISTER("pow",         lava_pow,         "ff");
    LAVA_REGISTER("exp",         lava_exp,         "f");
    LAVA_REGISTER("expm1",       lava_expm1,       "f");
    LAVA_REGISTER("exp2",        lava_exp2,        "f");
    LAVA_REGISTER("log",         lava_log,         "f");
    LAVA_REGISTER("log2",        lava_log2,        "f");
    LAVA_REGISTER("log10",       lava_log10,       "f");
    LAVA_REGISTER("log1p",       lava_log1p,       "f");
    LAVA_REGISTER("log_base",    lava_log_base,    "ff");

    /* Trigonometría */
    LAVA_REGISTER("sin",         lava_sin,         "f");
    LAVA_REGISTER("cos",         lava_cos,         "f");
    LAVA_REGISTER("tan",         lava_tan,         "f");
    LAVA_REGISTER("asin",        lava_asin,        "f");
    LAVA_REGISTER("acos",        lava_acos,        "f");
    LAVA_REGISTER("atan",        lava_atan,        "f");
    LAVA_REGISTER("atan2",       lava_atan2,       "ff");
    LAVA_REGISTER("degrees",     lava_degrees,     "f");
    LAVA_REGISTER("radians",     lava_radians,     "f");

    /* Hiperbólicas */
    LAVA_REGISTER("sinh",        lava_sinh,        "f");
    LAVA_REGISTER("cosh",        lava_cosh,        "f");
    LAVA_REGISTER("tanh",        lava_tanh,        "f");
    LAVA_REGISTER("asinh",       lava_asinh,       "f");
    LAVA_REGISTER("acosh",       lava_acosh,       "f");
    LAVA_REGISTER("atanh",       lava_atanh,       "f");

    /* Teoría de números */
    LAVA_REGISTER("gcd",         lava_gcd,         "ii");
    LAVA_REGISTER("lcm",         lava_lcm,         "ii");
    LAVA_REGISTER("factorial",   lava_factorial,   "i");
    LAVA_REGISTER("isqrt",       lava_isqrt,       "i");
    LAVA_REGISTER("comb",        lava_comb,        "ii");
    LAVA_REGISTER("perm",        lava_perm,        "ii");

    /* Especiales */
    LAVA_REGISTER("erf",         lava_erf,         "f");
    LAVA_REGISTER("erfc",        lava_erfc,        "f");
    LAVA_REGISTER("gamma",       lava_gamma,       "f");
    LAVA_REGISTER("lgamma",      lava_lgamma,      "f");

    /* Punto flotante */
    LAVA_REGISTER("fmod",        lava_fmod,        "ff");
    LAVA_REGISTER("remainder",   lava_remainder,   "ff");
    LAVA_REGISTER("hypot",       lava_hypot,       "ff");
    LAVA_REGISTER("fmin",        lava_fmin,        "ff");
    LAVA_REGISTER("fmax",        lava_fmax,        "ff");
    LAVA_REGISTER("fdim",        lava_fdim,        "ff");
    LAVA_REGISTER("ldexp",       lava_ldexp,       "fi");
    LAVA_REGISTER("frexp",       lava_frexp,       "f");
    LAVA_REGISTER("modf",        lava_modf,        "f");

    /* Predicados */
    LAVA_REGISTER("isfinite",    lava_isfinite,    "f");
    LAVA_REGISTER("isinf",       lava_isinf,       "f");
    LAVA_REGISTER("isnan",       lava_isnan,       "f");
    LAVA_REGISTER("isclose",     lava_isclose,     "ff");
    LAVA_REGISTER("isclose_tol", lava_isclose_tol, "ffff");

    /* Listas */
    LAVA_REGISTER("fsum",        lava_fsum,        "l");
    LAVA_REGISTER("prod",        lava_prod,        "l");
    LAVA_REGISTER("dist",        lava_dist,        "ffff");
}
