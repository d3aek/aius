#ifndef AIUS_A52_CRC_TEST_H
#define AIUS_A52_CRC_TEST_H

/* Test-side AC-3 CRC repair. CRC2 is appended normally; CRC1 precedes the
 * bytes it protects, so repair solves and caches the inverse 16x16 GF(2)
 * influence matrix for each first-segment length. Production only validates
 * residues and does not share this implementation. */

#include <stddef.h>
#include <stdint.h>

#define A52_TEST_CRC16_POLY UINT16_C(0x8005)
#define A52_TEST_MAX_FRAME UINT32_C(3840)

struct a52_test_crc1_solver {
    size_t segment_size;
    uint16_t inverse_rows[16];
};

static inline uint16_t a52_test_crc16_update(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)byte << 8;
    for (int bit = 0; bit < 8; bit++)
        crc = (crc & UINT16_C(0x8000))
            ? (uint16_t)((crc << 1) ^ A52_TEST_CRC16_POLY)
            : (uint16_t)(crc << 1);
    return crc;
}

static inline uint16_t a52_test_crc16(const uint8_t *data, size_t size)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < size; i++)
        crc = a52_test_crc16_update(crc, data[i]);
    return crc;
}

static inline size_t a52_test_crc_split(size_t frame_size)
{
    size_t frame_words = frame_size / 2u;
    return 2u * (frame_words / 2u + frame_words / 8u);
}

static inline uint16_t a52_test_crc1_effect(uint16_t field,
                                            size_t segment_size)
{
    uint16_t crc = 0;
    crc = a52_test_crc16_update(crc, (uint8_t)(field >> 8));
    crc = a52_test_crc16_update(crc, (uint8_t)field);
    for (size_t i = 2; i < segment_size; i++)
        crc = a52_test_crc16_update(crc, 0);
    return crc;
}

static inline const struct a52_test_crc1_solver *
a52_test_crc1_solver_get(size_t segment_size)
{
    static struct a52_test_crc1_solver cache[32];
    static size_t cache_count;

    for (size_t i = 0; i < cache_count; i++)
        if (cache[i].segment_size == segment_size)
            return &cache[i];
    if (cache_count == sizeof(cache) / sizeof(cache[0]))
        return NULL;

    uint16_t coefficients[16] = { 0 };
    uint16_t inverse[16];
    for (int row = 0; row < 16; row++)
        inverse[row] = (uint16_t)(UINT16_C(1) << row);
    for (int column = 0; column < 16; column++) {
        uint16_t effect =
            a52_test_crc1_effect((uint16_t)(UINT16_C(1) << column),
                                 segment_size);
        for (int row = 0; row < 16; row++)
            if (effect & (uint16_t)(UINT16_C(1) << row))
                coefficients[row] |=
                    (uint16_t)(UINT16_C(1) << column);
    }

    for (int column = 0; column < 16; column++) {
        int pivot = column;
        while (pivot < 16 &&
               !(coefficients[pivot] &
                 (uint16_t)(UINT16_C(1) << column)))
            pivot++;
        if (pivot == 16)
            return NULL;
        if (pivot != column) {
            uint16_t swap = coefficients[pivot];
            coefficients[pivot] = coefficients[column];
            coefficients[column] = swap;
            swap = inverse[pivot];
            inverse[pivot] = inverse[column];
            inverse[column] = swap;
        }
        for (int row = 0; row < 16; row++) {
            if (row != column &&
                (coefficients[row] &
                 (uint16_t)(UINT16_C(1) << column))) {
                coefficients[row] ^= coefficients[column];
                inverse[row] ^= inverse[column];
            }
        }
    }

    for (int row = 0; row < 16; row++) {
        if (coefficients[row] !=
            (uint16_t)(UINT16_C(1) << row))
            return NULL;
    }

    struct a52_test_crc1_solver *solver = &cache[cache_count++];
    solver->segment_size = segment_size;
    for (int row = 0; row < 16; row++)
        solver->inverse_rows[row] = inverse[row];
    return solver;
}

static inline unsigned int a52_test_parity16(uint16_t value)
{
    unsigned int parity = 0;
    while (value) {
        parity ^= value & 1u;
        value >>= 1;
    }
    return parity;
}

static inline int a52_test_crc_segments_valid(const uint8_t *frame,
                                               size_t frame_size)
{
    if (!frame || frame_size < 7 || frame_size > A52_TEST_MAX_FRAME)
        return 0;
    size_t split = a52_test_crc_split(frame_size);
    if (split < 4 || split > frame_size - 2)
        return 0;
    return a52_test_crc16(frame + 2, split - 2) == 0 &&
           a52_test_crc16(frame + split, frame_size - split) == 0 &&
           a52_test_crc16(frame + 2, frame_size - 2) == 0;
}

static inline int a52_test_crc_repair(uint8_t *frame, size_t frame_size)
{
    if (!frame || frame_size < 7 || frame_size > A52_TEST_MAX_FRAME)
        return 0;
    size_t split = a52_test_crc_split(frame_size);
    if (split < 4 || split > frame_size - 2)
        return 0;

    size_t first_segment_size = split - 2;
    const struct a52_test_crc1_solver *solver =
        a52_test_crc1_solver_get(first_segment_size);
    if (!solver)
        return 0;

    frame[2] = 0;
    frame[3] = 0;
    uint16_t base = a52_test_crc16(frame + 2, first_segment_size);
    uint16_t field = 0;
    for (int bit = 0; bit < 16; bit++)
        if (a52_test_parity16(
                (uint16_t)(solver->inverse_rows[bit] & base)))
            field |= (uint16_t)(UINT16_C(1) << bit);
    frame[2] = (uint8_t)(field >> 8);
    frame[3] = (uint8_t)field;

    frame[frame_size - 2] = 0;
    frame[frame_size - 1] = 0;
    uint16_t crc2 =
        a52_test_crc16(frame + split, frame_size - split - 2);
    frame[frame_size - 2] = (uint8_t)(crc2 >> 8);
    frame[frame_size - 1] = (uint8_t)crc2;

    return a52_test_crc_segments_valid(frame, frame_size);
}

#endif /* AIUS_A52_CRC_TEST_H */
