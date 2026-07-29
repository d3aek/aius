/* Gate probe: is the tables.h negative-shift rewrite value-identical? */
#include <stdio.h>
#include <string.h>

typedef float sample_t;

static const sample_t old_v[] = {
    (sample_t)((-2 << 15) / 3.0),  (sample_t)((2 << 15) / 3.0),
    (sample_t)((-4 << 15) / 5.0),  (sample_t)((-2 << 15) / 5.0),
    (sample_t)((2 << 15) / 5.0),   (sample_t)((4 << 15) / 5.0),
    (sample_t)((-6 << 15) / 7.0),  (sample_t)((-4 << 15) / 7.0),
    (sample_t)((-2 << 15) / 7.0),  (sample_t)((2 << 15) / 7.0),
    (sample_t)((4 << 15) / 7.0),   (sample_t)((6 << 15) / 7.0),
    (sample_t)((-10 << 15) / 11.0), (sample_t)((-8 << 15) / 11.0),
    (sample_t)((-6 << 15) / 11.0),  (sample_t)((-4 << 15) / 11.0),
    (sample_t)((-2 << 15) / 11.0),  (sample_t)((2 << 15) / 11.0),
    (sample_t)((4 << 15) / 11.0),   (sample_t)((6 << 15) / 11.0),
    (sample_t)((8 << 15) / 11.0),   (sample_t)((10 << 15) / 11.0),
    (sample_t)((-14 << 15) / 15.0), (sample_t)((-12 << 15) / 15.0),
    (sample_t)((-10 << 15) / 15.0), (sample_t)((-8 << 15) / 15.0),
    (sample_t)((-6 << 15) / 15.0),  (sample_t)((-4 << 15) / 15.0),
    (sample_t)((-2 << 15) / 15.0),  (sample_t)((2 << 15) / 15.0),
    (sample_t)((4 << 15) / 15.0),   (sample_t)((6 << 15) / 15.0),
    (sample_t)((8 << 15) / 15.0),   (sample_t)((10 << 15) / 15.0),
    (sample_t)((12 << 15) / 15.0),  (sample_t)((14 << 15) / 15.0),
};

#define S(n) ((n) * 32768)

static const sample_t new_v[] = {
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
    int n = (int)(sizeof(old_v) / sizeof(old_v[0]));
    int bad = 0;
    for (int i = 0; i < n; i++)
        if (memcmp(&old_v[i], &new_v[i], sizeof(sample_t)) != 0) {
            printf("DIFF at %d: %.9g vs %.9g\n", i, (double)old_v[i], (double)new_v[i]);
            bad++;
        }
    printf("%d constants compared, %d bit-level differences\n", n, bad);
    return bad != 0;
}
