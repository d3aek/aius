/* Source-coupled proof that tables.h's defined multiplication rewrite retains
 * the 36 distinct quantizer values established before F2. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef float sample_t;

#include "../../lib/aius_a52/a52/tables.h"

#define S(n) ((n) * 32768)

static const sample_t expected_v[] = {
    (sample_t)(S(-2) / 3.0),  (sample_t)(S(2) / 3.0),
    (sample_t)(S(-4) / 5.0),  (sample_t)(S(-2) / 5.0),
    (sample_t)(S(2) / 5.0),   (sample_t)(S(4) / 5.0),
    (sample_t)(S(-6) / 7.0),  (sample_t)(S(-4) / 7.0),
    (sample_t)(S(-2) / 7.0),  (sample_t)(S(2) / 7.0),
    (sample_t)(S(4) / 7.0),   (sample_t)(S(6) / 7.0),
    (sample_t)(S(-10) / 11.0), (sample_t)(S(-8) / 11.0),
    (sample_t)(S(-6) / 11.0),  (sample_t)(S(-4) / 11.0),
    (sample_t)(S(-2) / 11.0),  (sample_t)(S(2) / 11.0),
    (sample_t)(S(4) / 11.0),   (sample_t)(S(6) / 11.0),
    (sample_t)(S(8) / 11.0),   (sample_t)(S(10) / 11.0),
    (sample_t)(S(-14) / 15.0), (sample_t)(S(-12) / 15.0),
    (sample_t)(S(-10) / 15.0), (sample_t)(S(-8) / 15.0),
    (sample_t)(S(-6) / 15.0),  (sample_t)(S(-4) / 15.0),
    (sample_t)(S(-2) / 15.0),  (sample_t)(S(2) / 15.0),
    (sample_t)(S(4) / 15.0),   (sample_t)(S(6) / 15.0),
    (sample_t)(S(8) / 15.0),   (sample_t)(S(10) / 15.0),
    (sample_t)(S(12) / 15.0),  (sample_t)(S(14) / 15.0),
};

int main(void)
{
    /* One occurrence of every distinct rewritten value, read from the real
     * tables rather than restating tables.h's expressions in this probe. */
    const sample_t actual_v[] = {
        q_1_0[0], q_1_0[18],
        q_2_0[0], q_2_0[25], q_2_0[75], q_2_0[100],
        q_3[0], q_3[1], q_3[2], q_3[4], q_3[5], q_3[6],
        q_4_0[0], q_4_0[11], q_4_0[22], q_4_0[33], q_4_0[44],
        q_4_0[66], q_4_0[77], q_4_0[88], q_4_0[99], q_4_0[110],
        q_5[0], q_5[1], q_5[2], q_5[3], q_5[4], q_5[5], q_5[6],
        q_5[8], q_5[9], q_5[10], q_5[11], q_5[12], q_5[13], q_5[14],
    };
    int n = (int)(sizeof(actual_v) / sizeof(actual_v[0]));
    int bad = 0;
    if (sizeof(actual_v) != sizeof(expected_v)) {
        fprintf(stderr, "table probe cardinality mismatch\n");
        return 2;
    }
    for (int i = 0; i < n; i++)
        if (memcmp(&actual_v[i], &expected_v[i], sizeof(sample_t)) != 0) {
            printf("DIFF at %d: %.9g vs %.9g\n", i,
                   (double)actual_v[i], (double)expected_v[i]);
            bad++;
        }
    printf("%d tables.h constants compared, %d bit-level differences\n", n, bad);
    return bad != 0;
}
