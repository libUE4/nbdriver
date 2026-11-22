/* SPDX-License-Identifier: GPL-2.0-only */
/* Fixed-point conversion adapted from NBG-driver/include/gyro_math.h. */
#ifndef NBDRIVER_GYRO_MATH_H
#define NBDRIVER_GYRO_MATH_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 nbdriver_gyro_u32;
typedef s64 nbdriver_gyro_s64;
typedef u64 nbdriver_gyro_u64;
#else
#include <stdint.h>
typedef uint32_t nbdriver_gyro_u32;
typedef int64_t nbdriver_gyro_s64;
typedef uint64_t nbdriver_gyro_u64;
#endif

#define NBDRIVER_GYRO_EVENT_SIZE 104U
#define NBDRIVER_GYRO_EVENT_TYPE_OFFSET 8U
#define NBDRIVER_GYRO_EVENT_DATA_OFFSET 24U

static inline int nbdriver_gyro_type_selected(int type, nbdriver_gyro_u32 type_mask)
{
    return (type == 4 && (type_mask & 0x01U)) ||
           (type == 16 && (type_mask & 0x02U));
}

/* Convert finite IEEE-754 binary32 to signed Q32.32 without kernel FP use. */
static inline int nbdriver_gyro_bits_to_q32(nbdriver_gyro_u32 bits,
                                       nbdriver_gyro_s64 *out)
{
    nbdriver_gyro_u32 exponent = (bits >> 23) & 0xffU;
    nbdriver_gyro_u64 significand, magnitude;
    int shift;
    int negative;

    if (!out || exponent == 0xffU)
        return -1;
    negative = (bits >> 31) != 0;
    if (exponent == 0) {
        *out = 0;
        return 0;
    }

    significand = (bits & 0x7fffffU) | 0x800000U;
    shift = (int)exponent - 118;
    if (shift >= 0) {
        if (shift > 39)
            return -1;
        magnitude = significand << shift;
    } else {
        const unsigned int right = (unsigned int)-shift;
        nbdriver_gyro_u64 remainder, halfway;
        if (right >= 64) {
            magnitude = 0;
        } else {
            magnitude = significand >> right;
            remainder = significand & ((((nbdriver_gyro_u64)1) << right) - 1U);
            halfway = ((nbdriver_gyro_u64)1) << (right - 1U);
            if (remainder > halfway ||
                (remainder == halfway && (magnitude & 1U)))
                ++magnitude;
        }
    }

    if (negative) {
        if (magnitude > (((nbdriver_gyro_u64)1) << 63))
            return -1;
        *out = magnitude == (((nbdriver_gyro_u64)1) << 63)
            ? (-0x7fffffffffffffffLL - 1LL)
            : -(nbdriver_gyro_s64)magnitude;
    } else {
        if (magnitude > 0x7fffffffffffffffULL)
            return -1;
        *out = (nbdriver_gyro_s64)magnitude;
    }
    return 0;
}

static inline nbdriver_gyro_u32 nbdriver_gyro_q32_to_bits(nbdriver_gyro_s64 value)
{
    nbdriver_gyro_u64 magnitude, significand;
    nbdriver_gyro_u32 sign = 0, exponent;
    int position, shift;

    if (!value)
        return 0;
    if (value < 0) {
        sign = 0x80000000U;
        magnitude = (nbdriver_gyro_u64)(-(value + 1)) + 1U;
    } else {
        magnitude = (nbdriver_gyro_u64)value;
    }

    position = 63 - __builtin_clzll(magnitude);
    exponent = (nbdriver_gyro_u32)(position + 95);
    if (position > 23) {
        nbdriver_gyro_u64 remainder, halfway;
        shift = position - 23;
        significand = magnitude >> shift;
        remainder = magnitude & ((((nbdriver_gyro_u64)1) << shift) - 1U);
        halfway = ((nbdriver_gyro_u64)1) << (shift - 1);
        if (remainder > halfway ||
            (remainder == halfway && (significand & 1U))) {
            ++significand;
            if (significand == 0x1000000ULL) {
                significand >>= 1;
                ++exponent;
            }
        }
    } else {
        significand = magnitude << (23 - position);
    }
    return sign | (exponent << 23) | ((nbdriver_gyro_u32)significand & 0x7fffffU);
}

static inline int nbdriver_gyro_add_bits(nbdriver_gyro_u32 value_bits,
                                    nbdriver_gyro_s64 delta_q32,
                                    nbdriver_gyro_u32 *out_bits)
{
    nbdriver_gyro_s64 value_q32, sum;
    /* A zero delta must preserve that axis bit-for-bit, including signed zero
     * and subnormal values; Q32.32 round-tripping would erase tiny samples. */
    if (!out_bits || (value_bits & 0x7f800000U) == 0x7f800000U)
        return -1;
    if (!delta_q32) {
        *out_bits = value_bits;
        return 0;
    }


    if (nbdriver_gyro_bits_to_q32(value_bits, &value_q32) != 0 || !out_bits)
        return -1;
    if ((delta_q32 > 0 && value_q32 > 0x7fffffffffffffffLL - delta_q32) ||
        (delta_q32 < 0 && value_q32 < (-0x7fffffffffffffffLL - 1) - delta_q32))
        return -1;
    sum = value_q32 + delta_q32;
    *out_bits = nbdriver_gyro_q32_to_bits(sum);
    return 0;
}

static inline int nbdriver_gyro_add_xy(nbdriver_gyro_u32 x_bits,
                                  nbdriver_gyro_u32 y_bits,
                                  nbdriver_gyro_s64 delta_x_q32,
                                  nbdriver_gyro_s64 delta_y_q32,
                                  nbdriver_gyro_u32 *out_x_bits,
                                  nbdriver_gyro_u32 *out_y_bits)
{
    nbdriver_gyro_u32 x, y;

    if (!out_x_bits || !out_y_bits ||
        nbdriver_gyro_add_bits(x_bits, delta_x_q32, &x) ||
        nbdriver_gyro_add_bits(y_bits, delta_y_q32, &y))
        return -1;
    *out_x_bits = x;
    *out_y_bits = y;
    return 0;
}

/* Operate on a private complete batch; never on SensorService's source buffer.
 * Android sensors_event_t.version is sizeof(sensors_event_t), type is at +8,
 * timestamp at +16 and data at +24. Unknown/malformed records stay untouched.
 */
static inline unsigned int nbdriver_gyro_overlay(unsigned char *batch,
    unsigned int size, nbdriver_gyro_u32 type_mask,
    nbdriver_gyro_s64 x, nbdriver_gyro_s64 y)
{
    unsigned int at, changed = 0;
    if (!batch || (!x && !y) || size % NBDRIVER_GYRO_EVENT_SIZE) return 0;
    for (at = 0; at + NBDRIVER_GYRO_EVENT_SIZE <= size; at += NBDRIVER_GYRO_EVENT_SIZE) {
        nbdriver_gyro_u32 version, type, bx, by, ox, oy;
        __builtin_memcpy(&version, batch + at, 4);
        __builtin_memcpy(&type, batch + at + NBDRIVER_GYRO_EVENT_TYPE_OFFSET, 4);
        if (version != NBDRIVER_GYRO_EVENT_SIZE ||
            !nbdriver_gyro_type_selected((int)type, type_mask)) continue;
        __builtin_memcpy(&bx, batch + at + NBDRIVER_GYRO_EVENT_DATA_OFFSET, 4);
        __builtin_memcpy(&by, batch + at + NBDRIVER_GYRO_EVENT_DATA_OFFSET + 4, 4);
        if (nbdriver_gyro_add_xy(bx, by, x, y, &ox, &oy)) continue;
        __builtin_memcpy(batch + at + NBDRIVER_GYRO_EVENT_DATA_OFFSET, &ox, 4);
        __builtin_memcpy(batch + at + NBDRIVER_GYRO_EVENT_DATA_OFFSET + 4, &oy, 4);
        ++changed;
    }
    return changed;
}
#endif
