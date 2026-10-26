#include "bigint.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

InfernalBigInt bigint_zero(void) {
    InfernalBigInt value = {{0}, false, 0};
    return value;
}

bool bigint_is_zero(InfernalBigInt value) {
    return value.used == 0;
}

static void normalize(InfernalBigInt *value) {
    if (!value->used) value->negative = false;
}

InfernalBigInt bigint_from_u64(uint64_t number) {
    InfernalBigInt value = bigint_zero();
    for (size_t i = 0; i < sizeof(number); ++i) {
        value.bytes[i] = (uint8_t)number;
        if (value.bytes[i]) value.used = (uint8_t)(i + 1);
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
    size_t count = a.used > b.used ? a.used : b.used;
    size_t i = 0;
    for (; i < count || carry; ++i) {
        if (i == INFERNAL_INT_MAX_BYTES) return false;
        unsigned sum = (unsigned)a.bytes[i] + b.bytes[i] + carry;
        out->bytes[i] = (uint8_t)sum;
        carry = sum >> 8;
    }
    out->used = (uint8_t)i;
    while (out->used && !out->bytes[out->used - 1]) --out->used;
    return true;
}

static InfernalBigInt subtract_magnitude(InfernalBigInt a, InfernalBigInt b) {
    InfernalBigInt out = bigint_zero();
    unsigned borrow = 0;
    size_t count = a.used > b.used ? a.used : b.used;
    for (size_t i = 0; i < count; ++i) {
        int difference = (int)a.bytes[i] - (int)b.bytes[i] - (int)borrow;
        borrow = difference < 0;
        if (borrow) difference += 256;
        out.bytes[i] = (uint8_t)difference;
    }
    out.used = (uint8_t)count;
    while (out.used && !out.bytes[out.used - 1]) --out.used;
    return out;
}

int bigint_compare_magnitude(InfernalBigInt a, InfernalBigInt b) {
    size_t count = a.used > b.used ? a.used : b.used;
    while (count) {
        size_t i = --count;
        if (a.bytes[i] < b.bytes[i]) return -1;
        if (a.bytes[i] > b.bytes[i]) return 1;
    }
    return 0;
}

int bigint_compare(InfernalBigInt a, InfernalBigInt b) {
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
    for (size_t i = 0; i < a.used; ++i) {
        if (!a.bytes[i]) continue;
        unsigned carry = 0;
        for (size_t j = 0; j < b.used; ++j) {
            if (i + j >= INFERNAL_INT_MAX_BYTES) {
                if (b.bytes[j]) overflow = true;
                continue;
            }
            unsigned product = (unsigned)a.bytes[i] * b.bytes[j] +
                              out->bytes[i + j] + carry;
            out->bytes[i + j] = (uint8_t)product;
            carry = product >> 8;
        }
        size_t k = i + b.used;
        while (carry && k < INFERNAL_INT_MAX_BYTES) {
            unsigned sum = (unsigned)out->bytes[k] + carry;
            out->bytes[k++] = (uint8_t)sum;
            carry = sum >> 8;
        }
        if (carry) overflow = true;
    }
    out->negative = a.negative != b.negative;
    size_t max_used = a.used + b.used;
    if (max_used > INFERNAL_INT_MAX_BYTES) max_used = INFERNAL_INT_MAX_BYTES;
    out->used = (uint8_t)max_used;
    while (out->used && !out->bytes[out->used - 1]) --out->used;
    normalize(out);
    return !overflow;
}

static bool shift_left_one(InfernalBigInt *value, bool low_bit) {
    size_t old_used = value->used;
    unsigned carry = low_bit ? 1u : 0u;
    size_t count = value->used < INFERNAL_INT_MAX_BYTES ? value->used + 1 : value->used;
    for (size_t i = 0; i < count; ++i) {
        unsigned next = value->bytes[i] >> 7;
        value->bytes[i] = (uint8_t)((value->bytes[i] << 1) | carry);
        carry = next;
    }
    if (old_used < INFERNAL_INT_MAX_BYTES && value->bytes[old_used])
        value->used = (uint8_t)(old_used + 1);
    else
        value->used = (uint8_t)old_used;
    return carry != 0;
}

static bool get_bit(InfernalBigInt value, size_t bit) {
    return (value.bytes[bit / 8] >> (bit % 8)) & 1u;
}

static void set_bit(InfernalBigInt *value, size_t bit) {
    value->bytes[bit / 8] |= (uint8_t)(1u << (bit % 8));
    size_t used = bit / 8 + 1;
    if (used > value->used) value->used = (uint8_t)used;
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
    for (size_t bit = (size_t)a.used * 8; bit-- > 0;) {
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
        size_t count = out->used;
        size_t new_used = count;
        for (size_t i = 0; i < count || carry; ++i) {
            if (i == INFERNAL_INT_MAX_BYTES) return false;
            unsigned value = (unsigned)out->bytes[i] * 10u + carry;
            out->bytes[i] = (uint8_t)value;
            carry = value >> 8;
            if (out->bytes[i]) new_used = i + 1;
        }
        out->used = (uint8_t)new_used;
    }
    out->negative = negative;
    normalize(out);
    return true;
}

static size_t magnitude_bits(InfernalBigInt value) {
    if (value.used) {
        size_t i = value.used - 1;
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
    if (value.used > 8) return false;
    uint64_t result = 0;
    for (size_t i = 0; i < value.used; ++i) result |= (uint64_t)value.bytes[i] << (i * 8);
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
    for (size_t i = value.used; i-- > 0;)
        result = result * 256.0 + value.bytes[i];
    return value.negative ? -result : result;
}

static uint8_t div_small(InfernalBigInt *value, uint8_t divisor) {
    unsigned remainder = 0;
    for (size_t i = value->used; i-- > 0;) {
        unsigned current = (remainder << 8) | value->bytes[i];
        value->bytes[i] = (uint8_t)(current / divisor);
        remainder = current % divisor;
    }
    while (value->used && !value->bytes[value->used - 1]) --value->used;
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
