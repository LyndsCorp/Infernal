#ifndef CORE_BIGINT_H
#define CORE_BIGINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INFERNAL_INT_MAX_BYTES 64

typedef struct {
    uint8_t bytes[INFERNAL_INT_MAX_BYTES]; /* little endian magnitude */
    bool negative;
    uint8_t used; /* bytes significativos; cero si el entero vale 0 */
} InfernalBigInt;

InfernalBigInt bigint_zero(void);
InfernalBigInt bigint_from_i64(int64_t value);
InfernalBigInt bigint_from_u64(uint64_t value);
bool bigint_parse(const char *text, InfernalBigInt *out);
bool bigint_add(InfernalBigInt a, InfernalBigInt b, InfernalBigInt *out);
bool bigint_sub(InfernalBigInt a, InfernalBigInt b, InfernalBigInt *out);
bool bigint_mul(InfernalBigInt a, InfernalBigInt b, InfernalBigInt *out);
bool bigint_divmod(InfernalBigInt a, InfernalBigInt b,
                   InfernalBigInt *quotient, InfernalBigInt *remainder);
bool bigint_pow(InfernalBigInt base, uint32_t exponent, InfernalBigInt *out);
int bigint_compare(InfernalBigInt a, InfernalBigInt b);
int bigint_compare_ref(const InfernalBigInt *a, const InfernalBigInt *b);
int bigint_compare_magnitude(InfernalBigInt a, InfernalBigInt b);
bool bigint_is_zero(InfernalBigInt value);
bool bigint_is_zero_ref(const InfernalBigInt *value);
bool bigint_fits_width(InfernalBigInt value, size_t width, bool is_signed);
size_t bigint_min_width(InfernalBigInt value, bool is_signed);
bool bigint_to_i64(InfernalBigInt value, int64_t *out);
bool bigint_to_u64(InfernalBigInt value, uint64_t *out);
double bigint_to_double(InfernalBigInt value);
double bigint_to_double_ref(const InfernalBigInt *value);
char *bigint_to_string(InfernalBigInt value);

#endif
