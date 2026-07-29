/* Gate probe: FNV-1a hash of every decoded plane over the whole corpus.
 * Byte-identity oracle for the F1 identity-only reduction. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a52.h"
#include "aius_a52.h"

static uint8_t *read_file(const char *dir, const char *name, size_t *n)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s.ac3", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f); *n = (size_t)sz; return b;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    static const char *names[] = { "stereo_tones", "stereo_impulse",
                                   "surround51_impulse", "surround51_ident" };
    uint64_t h = 14695981039346656037ull /* canonical FNV-1a 64 basis */;
    for (int v = 0; v < 4; v++) {
        size_t sz = 0;
        uint8_t *d = read_file(argv[1], names[v], &sz);
        if (!d) { fprintf(stderr, "no %s\n", names[v]); return 3; }
        AiusA52Decoder *dec = NULL;
        if (aius_a52_decoder_create(&dec) != AIUS_OK) return 1;
        size_t off = 0;
        uint64_t vh = 14695981039346656037ull /* canonical FNV-1a 64 basis */;
        while (off + 7 <= sz) {
            int fl, sr, br;
            int len = a52_syncinfo(d + off, &fl, &sr, &br);
            if (len <= 0 || off + (size_t)len > sz) break;
            AiusPcmBlock blk;
            memset(&blk, 0, sizeof(blk));
            AiusStatus st = aius_a52_decode_access_unit(dec, d + off, (size_t)len, &blk);
            if (st != AIUS_OK) { fprintf(stderr, "%s: status %d\n", names[v], (int)st); return 1; }
            for (uint16_t c = 0; c < blk.channels; c++)
                for (uint32_t i = 0; i < blk.frames; i++) {
                    uint32_t s = (uint32_t)blk.planes[c][i];
                    for (int k = 0; k < 4; k++) {
                        vh ^= (uint8_t)(s >> (8 * k));
                        vh *= 1099511628211ull;
                    }
                }
            off += (size_t)len;
        }
        printf("%-20s flagsmask=0x%x hash=%016" PRIx64 "\n", names[v], 0, vh);
        h ^= vh; h *= 1099511628211ull;
        aius_a52_decoder_destroy(dec);
        free(d);
    }
    printf("CORPUS %016" PRIx64 "\n", h);
    return 0;
}
