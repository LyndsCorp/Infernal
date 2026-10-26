#include "bigint.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

InfernalBigInt bigint_zero(void) {
    InfernalBigInt value = {{0}, false};
    return value;
}

bool bigint_is_zero(InfernalBigInt value) {
    for (size_t i = 0; i < INFERNAL_INT_MAX_BYTES; ++i)
        if (value.bytes[i]) return false;
    return true;
}

static void normalize(InfernalBigInt *value) {
    if (bigint_is_zero(*value)) value->negative = false;
}

InfernalBigInt bigint_from_u64(uint64_t number) {
    InfernalBigInt value = bigint_zero();
    for (size_t i = 0; i < sizeof(number); ++i) {
        value.bytes[i] = (uint8_t)number;
        number >>= 8;
    }
    return value;
}

InfernalBigInt bigint_from_i64(int64_t number) {
    if (number >= 0) return bigint_from_u64((uint64_t)number);
    uint64_t magnitude = (uint64_t)(-(number + 1)) + 1;
    InfernalBigInt value = bigint_from_u64(magnitude);
    value.negative = true;
    return value;
}

static bool add_magnitude(InfernalBigInt a, InfernalBigInt b,
                          InfernalBigInt *out) {
    unsigned carry = 0;
    *out = bigint_zero();
    for (size_t i = 0; i < INFERNAL_INT_MAX_BYTES; ++i) {
        unsigned sum = (unsigned)a.bytes[i] + b.bytes[i] + carry;
        out->bytes[i] = (uint8_t)sum;
        carry = sum >> 8;
    }
    return carry == 0;
}

static InfernalBigInt subtract_magnitude(InfernalBigInt a, InfernalBigInt b) {
    InfernalBigInt out = bigint_zero();
    unsigned borrow = 0;
    for (size_t i = 0; i < INFERNAL_INT_MAX_BYTES; ++i) {
        int difference = (int)a.bytes[i] - (int)b.bytes[i] - (int)borrow;
        borrow = difference < 0;
        if (borrow) difference += 256;
        out.bytes[i] = (uint8_t)difference;
    }
    return out;
}

int bigint_compare_magnitude(InfernalBigInt a, InfernalBigInt b) {
    for (size_t i = INFERNAL_INT_MAX_BYTES; i-- > 0;) {
        if (a.bytes[i] < b.bytes[i]) return -1;
        if (a.bytes[i] > b.bytes[i]) return 1;
    }
    return 0;
}

int bigint_compare(InfernalBigInt a, InfernalBigInt b) {
    normalize(&a);
    normalize(&b);
    if (a.negative != b.negative) return a.negative ? -1 : 1;
    int magnitude = bigint_compare_magnitude(a, b);
    return a.negative ? -magnitude : magnitude;
}

bool bigint_add(InfernalBigInt a, InfernalBigInt b, InfernalBigInt *out) {
    if (a.negative == b.negative) {
        if (!add_magnitude(a, b, out)) return false;
        out->negative = a.negative;
    } else {
        int order = bigint_compare_magnitude(a, b);
        if (order >= 0) {
            *out = subtract_magnitude(a, b);
            out->negative = a.negative;
        } else {
            *out = subtract_magnitude(b, a);
            out->negative = b.negative;
        }
    }
    normalize(out);
    return true;
}

bool bigint_sub(InfernalBigInt a, InfernalBigInt b, InfernalBigInt *out) {
    if (!bigint_is_zero(b)) b.negative = !b.negative;
    return bigint_add(a, b, out);
}

bool bigint_mul(InfernalBigInt a, InfernalBigInt b, InfernalBigInt *out) {
    *out = bigint_zero();
    bool overflow = false;
    for (size_t i = 0; i < INFERNAL_INT_MAX_BYTES; ++i) {
        unsigned carry = 0;
        if (!a.bytes[i]) continue;
        for (size_t j = 0; j < INFERNAL_INT_MAX_BYTES; ++j) {
            if (i + j >= INFERNAL_INT_MAX_BYTES) {
                if (b.bytes[j]) overflow = true;
                continue;
            }
            unsigned product = (unsigned)a.bytes[i] * b.bytes[j] +
                              out->bytes[i + j] + carry;
            out->bytes[i + j] = (uint8_t)product;
            carry = product >> 8;
        }
        if (carry) overflow = true;
    }
    out->negative = a.negative != b.negative;
    normalize(out);
    return !overflow;
}

static bool shift_left_one(InfernalBigInt *value, bool low_bit) {
    unsigned carry = low_bit ? 1u : 0u;
    for (size_t i = 0; i < INFERNAL_INT_MAX_BYTES; ++i) {
        unsigned next = value->bytes[i] >> 7;
        value->bytes[i] = (uint8_t)((value->bytes[i] << 1) | carry);
        carry = next;
    }
    return carry != 0;
}

static bool get_bit(InfernalBigInt value, size_t bit) {
    return (value.bytes[bit / 8] >> (bit % 8)) & 1u;
}

static void set_bit(InfernalBigInt *value, size_t bit) {
    value->bytes[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

bool bigint_divmod(InfernalBigInt a, InfernalBigInt b,
                   InfernalBigInt *quotient, InfernalBigInt *remainder) {
    if (bigint_is_zero(b)) return false;
    bool quotient_negative = a.negative != b.negative;
    bool remainder_negative = a.negative;
    a.negative = false;
    b.negative = false;
    *quotient = bigint_zero();
    *remainder = bigint_zero();
    for (size_t bit = INFERNAL_INT_MAX_BYTES * 8; bit-- > 0;) {
        bool overflow = shift_left_one(remainder, get_bit(a, bit));
        if (overflow || bigint_compare_magnitude(*remainder, b) >= 0) {
            *remainder = subtract_magnitude(*remainder, b);
            set_bit(quotient, bit);
        }
    }
    quotient->negative = quotient_negative && !bigint_is_zero(*quotient);
    remainder->negative = remainder_negative && !bigint_is_zero(*remainder);
    return true;
}

bool bigint_pow(InfernalBigInt base, uint32_t exponent, InfernalBigInt *out) {
    InfernalBigInt result = bigint_from_u64(1);
    while (exponent) {
        if (exponent & 1u) {
            if (!bigint_mul(result, base, &result)) return false;
        }
        exponent >>= 1;
        if (exponent && !bigint_mul(base, base, &base)) return false;
    }
    *out = result;
    return true;
}

bool bigint_parse(const char *text, InfernalBigInt *out) {
    if (!text || !*text || !out) return false;
    bool negative = false;
    if (*text == '-' || *text == '+') {
        negative = *text == '-';
        ++text;
    }
    if (!*text) return false;
    *out = bigint_zero();
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        unsigned carry = (unsigned)(*p - '0');
        for (size_t i = 0; i < INFERNAL_INT_MAX_BYTES; ++i) {
            unsigned value = (unsigned)out->bytes[i] * 10u + carry;
            out->bytes[i] = (uint8_t)value;
            carry = value >> 8;
        }
        if (carry) return false;
    }
    out->negative = negative;
    normalize(out);
    return true;
}

static size_t magnitude_bits(InfernalBigInt value) {
    for (size_t i = INFERNAL_INT_MAX_BYTES; i-- > 0;) {
        if (!value.bytes[i]) continue;
        uint8_t byte = value.bytes[i];
        size_t bits = 0;
        while (byte) { ++bits; byte >>= 1; }
        return i * 8 + bits;
    }
    return 0;
}

bool bigint_fits_width(InfernalBigInt value, size_t width, bool is_signed) {
    if (width > INFERNAL_INT_MAX_BYTES || width < 4) return false;
    if (!is_signed && value.negative) return false;
    size_t bits = magnitude_bits(value);
    if (!is_signed) return bits <= width * 8;
    if (!value.negative) return bits < width * 8;
    if (bits < width * 8) return true;
    if (bits > width * 8) return false;
    size_t sign_bit = width * 8 - 1;
    if (sign_bit / 8 >= INFERNAL_INT_MAX_BYTES) return false;
    uint8_t limit[INFERNAL_INT_MAX_BYTES] = {0};
    limit[sign_bit / 8] = (uint8_t)(1u << (sign_bit % 8));
    for (size_t i = 0; i < INFERNAL_INT_MAX_BYTES; ++i)
        if (value.bytes[i] != limit[i]) return false;
    return true;
}

size_t bigint_min_width(InfernalBigInt value, bool is_signed) {
    for (size_t width = 4; width <= INFERNAL_INT_MAX_BYTES; width *= 2)
        if (bigint_fits_width(value, width, is_signed)) return width;
    return 0;
}

bool bigint_to_u64(InfernalBigInt value, uint64_t *out) {
    if (value.negative) return false;
    for (size_t i = 8; i < INFERNAL_INT_MAX_BYTES; ++i)
        if (value.bytes[i]) return false;
    uint64_t result = 0;
    for (size_t i = 0; i < 8; ++i) result |= (uint64_t)value.bytes[i] << (i * 8);
    if (out) *out = result;
    return true;
}

bool bigint_to_i64(InfernalBigInt value, int64_t *out) {
    bool negative = value.negative;
    value.negative = false;
    uint64_t magnitude;
    if (!bigint_to_u64(value, &magnitude)) return false;
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    if (magnitude > limit) return false;
    if (out) *out = negative ? (magnitude == limit ? INT64_MIN : -(int64_t)magnitude)
                             : (int64_t)magnitude;
    return true;
}

double bigint_to_double(InfernalBigInt value) {
    double result = 0.0;
    for (size_t i = INFERNAL_INT_MAX_BYTES; i-- > 0;)
        result = result * 256.0 + value.bytes[i];
    return value.negative ? -result : result;
}

static uint8_t div_small(InfernalBigInt *value, uint8_t divisor) {
    unsigned remainder = 0;
    for (size_t i = INFERNAL_INT_MAX_BYTES; i-- > 0;) {
        unsigned current = (remainder << 8) | value->bytes[i];
        value->bytes[i] = (uint8_t)(current / divisor);
        remainder = current % divisor;
    }
    return (uint8_t)remainder;
}

char *bigint_to_string(InfernalBigInt value) {
    char reverse[INFERNAL_INT_MAX_BYTES * 3 + 2];
    size_t count = 0;
    bool negative = value.negative;
    value.negative = false;
    do {
        reverse[count++] = (char)('0' + div_small(&value, 10));
    } while (!bigint_is_zero(value));
    size_t total = count + (negative ? 1 : 0);
    char *result = malloc(total + 1);
    if (!result) return NULL;
    size_t pos = 0;
    if (negative) result[pos++] = '-';
    while (count) result[pos++] = reverse[--count];
    result[pos] = '\0';
    return result;
}
