/* Deterministically generate the IEC 61937 parser known-answer corpus.
 *
 * Wraps real DTS Core, AC-3, and E-AC-3 access units (extracted from the
 * committed codec corpora) in IEC 61937 framing, and synthesizes the edge
 * cases the parser must type correctly (unsupported types, corrupt sync,
 * bad/over-long length, truncation, padding, coalesced bursts).
 *
 * Every vector is written as a `.iec` wire file (a sequence of 16-bit words, each
 * serialized big-endian on disk) plus, for vectors that complete a burst, a
 * `.pay` file holding the expected canonical big-endian payload bytes. The wire
 * word values are exactly what the parser receives: little-endian-wire vectors
 * store the byte-swapped preamble/payload words, which the parser auto-detects.
 * A manifest.txt records how each vector is built and its expected parse result.
 *
 * This is the provenance record: re-running reproduces the committed corpus
 * bit-for-bit from the same dts_vectors/ac3_vectors/eac3_vectors inputs. It
 * needs no ffmpeg.
 *
 * Usage: gen_iec_corpus <dts_vectors_dir> <ac3_vectors_dir> <eac3_vectors_dir>
 *                       <out_dir>
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aius_iec61937.h" /* AIUS_IEC61937_MAX_PAYLOAD for the over-long vector */

#define IEC_PA 0xF872u
#define IEC_PB 0x4E1Fu

static uint16_t bswap16(uint16_t w) { return (uint16_t)((w >> 8) | (w << 8)); }

/* ---- growable word buffer ------------------------------------------------ */

struct wb {
	uint16_t *w;
	size_t n, cap;
};

static void wb_push(struct wb *b, uint16_t v)
{
	if (b->n == b->cap) {
		b->cap = b->cap ? b->cap * 2 : 256;
		b->w = realloc(b->w, b->cap * sizeof(*b->w));
		if (!b->w) {
			fprintf(stderr, "oom\n");
			exit(1);
		}
	}
	b->w[b->n++] = v;
}

static void wb_free(struct wb *b) { free(b->w); memset(b, 0, sizeof(*b)); }

/* ---- framing builders ---------------------------------------------------- */

static void add_preamble(struct wb *b, uint16_t data_type, int error,
                         uint32_t pd_value, int swap)
{
	uint16_t pc = (uint16_t)(data_type & 0x1Fu);
	if (error)
		pc |= 0x0080u;
	uint16_t pa = IEC_PA, pb = IEC_PB, pd = (uint16_t)pd_value;
	if (swap) {
		pa = bswap16(pa);
		pb = bswap16(pb);
		pc = bswap16(pc);
		pd = bswap16(pd);
	}
	wb_push(b, pa);
	wb_push(b, pb);
	wb_push(b, pc);
	wb_push(b, pd);
}

static void add_payload_16(struct wb *b, const uint8_t *p, size_t len, int swap)
{
	for (size_t k = 0; k < len; k += 2) {
		uint8_t hi = p[k];
		uint8_t lo = (k + 1 < len) ? p[k + 1] : 0;
		uint16_t w = (uint16_t)((hi << 8) | lo);
		wb_push(b, swap ? bswap16(w) : w);
	}
}

/* 14-bit packing: MSB-first bit stream sliced into 14-bit groups, each placed in
 * the low 14 bits of a word; a trailing partial group is left-justified. */
static void add_payload_14(struct wb *b, const uint8_t *p, size_t len)
{
	uint32_t acc = 0;
	int cnt = 0;
	for (size_t k = 0; k < len; k++) {
		acc = (acc << 8) | p[k];
		cnt += 8;
		while (cnt >= 14) {
			cnt -= 14;
			wb_push(b, (uint16_t)((acc >> cnt) & 0x3FFFu));
		}
	}
	if (cnt > 0)
		wb_push(b, (uint16_t)((acc << (14 - cnt)) & 0x3FFFu));
}

/* ---- file IO ------------------------------------------------------------- */

static uint8_t *read_file(const char *path, size_t *size_out)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "cannot open %s\n", path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc((size_t)n);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "read failed %s\n", path);
		exit(1);
	}
	fclose(f);
	*size_out = (size_t)n;
	return buf;
}

static void write_iec(const char *dir, const char *name, const struct wb *b)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.iec", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "cannot write %s\n", path);
		exit(1);
	}
	for (size_t i = 0; i < b->n; i++) {
		uint8_t be[2] = { (uint8_t)(b->w[i] >> 8), (uint8_t)(b->w[i] & 0xFF) };
		fwrite(be, 1, 2, f);
	}
	fclose(f);
}

static void write_pay(const char *dir, const char *name, const uint8_t *p,
                      size_t len)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.pay", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "cannot write %s\n", path);
		exit(1);
	}
	if (len)
		fwrite(p, 1, len, f);
	fclose(f);
}

/* Extract the first DTS Core access unit (sync 0x7FFE8001 to the next sync). */
static void first_access_unit(const uint8_t *data, size_t size,
                              const uint8_t **au_out, size_t *au_len)
{
	size_t start = SIZE_MAX;
	for (size_t i = 0; i + 4 <= size; i++) {
		if (data[i] == 0x7f && data[i + 1] == 0xfe && data[i + 2] == 0x80 &&
		    data[i + 3] == 0x01) {
			start = i;
			break;
		}
	}
	if (start == SIZE_MAX) {
		fprintf(stderr, "no DTS sync found\n");
		exit(1);
	}
	size_t end = size;
	for (size_t i = start + 4; i + 4 <= size; i++) {
		if (data[i] == 0x7f && data[i + 1] == 0xfe && data[i + 2] == 0x80 &&
		    data[i + 3] == 0x01) {
			end = i;
			break;
		}
	}
	*au_out = data + start;
	*au_len = end - start;
}

/* Extract the first 48 kHz AC-3 access unit. Frame size is derived from
 * fscod/frmsizecod in buf[4], never by scanning for a second sync word. */
static void first_ac3_access_unit(const uint8_t *data, size_t size,
                                  const uint8_t **au_out, size_t *au_len)
{
	static const uint16_t bitrate_kbps[19] = {
		32, 40, 48, 56, 64, 80, 96, 112, 128, 160,
		192, 224, 256, 320, 384, 448, 512, 576, 640
	};
	if (size < 7 || data[0] != 0x0B || data[1] != 0x77) {
		fprintf(stderr, "no AC-3 sync at corpus start\n");
		exit(1);
	}
	uint8_t fscod = (uint8_t)(data[4] >> 6);
	uint8_t frmsizecod = (uint8_t)(data[4] & 0x3Fu);
	if (fscod != 0 || frmsizecod >= 38) {
		fprintf(stderr, "AC-3 corpus is not a valid 48 kHz frame\n");
		exit(1);
	}
	size_t frame_bytes = (size_t)bitrate_kbps[frmsizecod >> 1] * 4u;
	if (frame_bytes > size) {
		fprintf(stderr, "AC-3 frame length %zu exceeds corpus size %zu\n",
		        frame_bytes, size);
		exit(1);
	}
	*au_out = data;
	*au_len = frame_bytes;
}

/* Extract the first E-AC-3 access unit: an independent-substream (strmtyp 0,
 * substreamid 0) syncframe whose length comes from the 11-bit frmsiz field,
 * (frmsiz + 1) 16-bit words, never from scanning for a second sync word. */
static void first_eac3_access_unit(const uint8_t *data, size_t size,
                                   const uint8_t **au_out, size_t *au_len)
{
	if (size < 6 || data[0] != 0x0B || data[1] != 0x77) {
		fprintf(stderr, "no E-AC-3 sync at corpus start\n");
		exit(1);
	}
	uint8_t strmtyp = (uint8_t)(data[2] >> 6);
	uint8_t substreamid = (uint8_t)((data[2] >> 3) & 0x07u);
	uint8_t bsid = (uint8_t)(data[5] >> 3);
	if (strmtyp != 0 || substreamid != 0 || bsid != 16) {
		fprintf(stderr, "E-AC-3 corpus does not start with an independent "
		        "bsid-16 substream-0 frame\n");
		exit(1);
	}
	size_t frmsiz = ((size_t)(data[2] & 0x07u) << 8) | data[3];
	size_t frame_bytes = (frmsiz + 1u) * 2u;
	if (frame_bytes > size) {
		fprintf(stderr, "E-AC-3 frame length %zu exceeds corpus size %zu\n",
		        frame_bytes, size);
		exit(1);
	}
	*au_out = data;
	*au_len = frame_bytes;
}

static void pad_to_word_count(struct wb *b, size_t target)
{
	if (b->n > target) {
		fprintf(stderr, "burst occupies %zu words, exceeds %zu-word period\n",
		        b->n, target);
		exit(1);
	}
	while (b->n < target)
		wb_push(b, 0x0000);
}

/* Deterministic synthetic payload pattern. */
static void synth_fill(uint8_t *p, size_t len)
{
	for (size_t i = 0; i < len; i++)
		p[i] = (uint8_t)(i * 7u + 0x11u);
}

static FILE *g_manifest;

static void manifest(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(g_manifest, fmt, ap);
	va_end(ap);
	fputc('\n', g_manifest);
}

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr,
		        "usage: %s <dts_vectors_dir> <ac3_vectors_dir> "
		        "<eac3_vectors_dir> <out_dir>\n",
		        argv[0]);
		return 2;
	}
	const char *dts_dir = argv[1];
	const char *ac3_dir = argv[2];
	const char *eac3_dir = argv[3];
	const char *out = argv[4];

	char mpath[1024];
	snprintf(mpath, sizeof(mpath), "%s/manifest.txt", out);
	g_manifest = fopen(mpath, "w");
	if (!g_manifest) {
		fprintf(stderr, "cannot write %s\n", mpath);
		return 1;
	}
	manifest("IEC 61937 parser corpus. Columns: name | packing | expect | detail");
	manifest("(regenerate with tools/iec_corpus/generate.sh)");
	manifest("");

	/* Load real DTS Core, AC-3, and E-AC-3 access units. */
	char p1[1024], p2[1024], p3[1024], p4[1024];
	snprintf(p1, sizeof(p1), "%s/stereo_tones.dts", dts_dir);
	snprintf(p2, sizeof(p2), "%s/surround51_tones.dts", dts_dir);
	snprintf(p3, sizeof(p3), "%s/surround51_ident.ac3", ac3_dir);
	snprintf(p4, sizeof(p4), "%s/surround51_ident.eac3", eac3_dir);
	size_t s1 = 0, s2 = 0, s3 = 0, s4 = 0;
	uint8_t *d1 = read_file(p1, &s1);
	uint8_t *d2 = read_file(p2, &s2);
	uint8_t *d3 = read_file(p3, &s3);
	uint8_t *d4 = read_file(p4, &s4);
	const uint8_t *au1, *au2, *ac3_au, *eac3_au;
	size_t au1n, au2n, ac3_aun, eac3_aun;
	first_access_unit(d1, s1, &au1, &au1n);
	first_access_unit(d2, s2, &au2, &au2n);
	first_ac3_access_unit(d3, s3, &ac3_au, &ac3_aun);
	first_eac3_access_unit(d4, s4, &eac3_au, &eac3_aun);
	fprintf(stderr,
	        "stereo DTS AU=%zu bytes, surround DTS AU=%zu bytes, AC-3 AU=%zu "
	        "bytes, E-AC-3 AU=%zu bytes (cap=%u)\n",
	        au1n, au2n, ac3_aun, eac3_aun, AIUS_IEC61937_MAX_PAYLOAD);
	if (au1n > AIUS_IEC61937_MAX_PAYLOAD ||
	    au2n > AIUS_IEC61937_MAX_PAYLOAD ||
	    ac3_aun > AIUS_IEC61937_MAX_PAYLOAD ||
	    eac3_aun > AIUS_IEC61937_MAX_PAYLOAD) {
		fprintf(stderr, "access unit exceeds parser capacity\n");
		return 1;
	}

	struct wb b = { 0 };

	/* 1. dts_be16 — real DTS Core, 16-bit big-endian wire, type 13. */
	add_preamble(&b, 13, 0, (uint32_t)(au1n * 8), 0);
	add_payload_16(&b, au1, au1n, 0);
	write_iec(out, "dts_be16", &b);
	write_pay(out, "dts_be16", au1, au1n);
	manifest("dts_be16 | 16 | OK type=13 | real stereo DTS Core AU (%zu B), BE wire",
	         au1n);
	wb_free(&b);

	/* 2. dts_le16 — same AU, byte-swapped (little-endian) wire. */
	add_preamble(&b, 13, 0, (uint32_t)(au1n * 8), 1);
	add_payload_16(&b, au1, au1n, 1);
	write_iec(out, "dts_le16", &b);
	write_pay(out, "dts_le16", au1, au1n);
	manifest("dts_le16 | 16 | OK type=13 | same AU, LE (byte-swapped) wire -> "
	         "identical payload");
	wb_free(&b);

	/* 3. dts_14bit — same AU, 14-bit packing, BE wire. */
	add_preamble(&b, 13, 0, (uint32_t)(au1n * 8), 0);
	add_payload_14(&b, au1, au1n);
	write_iec(out, "dts_14bit", &b);
	write_pay(out, "dts_14bit", au1, au1n);
	manifest("dts_14bit | 14 | OK type=13 | same AU, 14-bit packing -> identical "
	         "payload");
	wb_free(&b);

	/* 4. dts51_be16 — real 5.1 DTS Core AU, 16-bit BE. */
	add_preamble(&b, 13, 0, (uint32_t)(au2n * 8), 0);
	add_payload_16(&b, au2, au2n, 0);
	write_iec(out, "dts51_be16", &b);
	write_pay(out, "dts51_be16", au2, au2n);
	manifest("dts51_be16 | 16 | OK type=13 | real 5.1 DTS Core AU (%zu B), BE wire",
	         au2n);
	wb_free(&b);

	/* 5. ac3_surround51_ident — two real AC-3 AUs at the 3072-word IEC
	 * repetition period. The second Pa therefore begins at word 3072. */
	add_preamble(&b, 1, 0, (uint32_t)(ac3_aun * 8u), 0);
	add_payload_16(&b, ac3_au, ac3_aun, 0);
	pad_to_word_count(&b, 3072);
	add_preamble(&b, 1, 0, (uint32_t)(ac3_aun * 8u), 0);
	add_payload_16(&b, ac3_au, ac3_aun, 0);
	write_iec(out, "ac3_surround51_ident", &b);
	write_pay(out, "ac3_surround51_ident", ac3_au, ac3_aun);
	manifest("ac3_surround51_ident | 16 | OK,OK type=1 | two real "
	         "surround51_ident AC-3 AUs (%zu B), Pa spacing=3072 words",
	         ac3_aun);
	wb_free(&b);

	/* eac3_surround51_ident — two real E-AC-3 AUs at the 12288-word type-21
	 * repetition period (the E-AC-3 burst spans 1536 samples on a 4x-rate
	 * link). Pc 21 uses a BYTE-count Pd. UNSUPPORTED until the phase-3
	 * dispatch takes type 21 in scope, then OK,OK. */
	add_preamble(&b, 21, 0, (uint32_t)eac3_aun, 0);
	add_payload_16(&b, eac3_au, eac3_aun, 0);
	pad_to_word_count(&b, 12288);
	add_preamble(&b, 21, 0, (uint32_t)eac3_aun, 0);
	add_payload_16(&b, eac3_au, eac3_aun, 0);
	write_iec(out, "eac3_surround51_ident", &b);
	write_pay(out, "eac3_surround51_ident", eac3_au, eac3_aun);
	manifest("eac3_surround51_ident | 16 | UNSUPPORTED,UNSUPPORTED type=21 | "
	         "two real surround51_ident E-AC-3 AUs (%zu B), Pd BYTE count, "
	         "Pa spacing=12288 words",
	         eac3_aun);
	wb_free(&b);

	/* Synthetic payloads for the type / edge vectors. */
	uint8_t syn[128];

	/* 5. type11 — accepted burst type I. */
	synth_fill(syn, 32);
	add_preamble(&b, 11, 0, 32 * 8, 0);
	add_payload_16(&b, syn, 32, 0);
	write_iec(out, "type11", &b);
	write_pay(out, "type11", syn, 32);
	manifest("type11 | 16 | OK type=11 | synthetic 32 B, DTS burst type I");
	wb_free(&b);

	/* 6. type12 — accepted burst type II. */
	synth_fill(syn, 64);
	add_preamble(&b, 12, 0, 64 * 8, 0);
	add_payload_16(&b, syn, 64, 0);
	write_iec(out, "type12", &b);
	write_pay(out, "type12", syn, 64);
	manifest("type12 | 16 | OK type=12 | synthetic 64 B, DTS burst type II");
	wb_free(&b);

	/* 8. unsupported_eac3 — a transport-shaped E-AC-3 frame. Pc 21 uses a
	 * BYTE-count Pd, unlike the bit-count convention for AC-3 and DTS. */
	uint8_t eac3[0x0700];
	synth_fill(eac3, sizeof(eac3));
	eac3[0] = 0x0B;
	eac3[1] = 0x77;
	eac3[2] = 0x03; /* independent stream 0, frmsiz[10:8] = 3 */
	eac3[3] = 0x7F; /* frmsiz = 895 => (895 + 1) * 2 = 1792 B */
	eac3[4] = 0x3F; /* 48 kHz, 6 blocks, 3/2+surround with LFE */
	eac3[5] = 0x80; /* bsid = 16 (E-AC-3) */
	add_preamble(&b, 21, 0, sizeof(eac3), 0);
	add_payload_16(&b, eac3, sizeof(eac3), 0);
	write_iec(out, "unsupported_eac3", &b);
	write_pay(out, "unsupported_eac3", eac3, sizeof(eac3));
	manifest("unsupported_eac3 | 16 | UNSUPPORTED type=21 | E-AC-3-shaped "
	         "1792 B frame, Pd=0x0700 BYTE count");
	wb_free(&b);

	/* 9. null_type — data type 0, zero length => UNSUPPORTED, empty payload. */
	add_preamble(&b, 0, 0, 0, 0);
	write_iec(out, "null_type", &b);
	write_pay(out, "null_type", syn, 0);
	manifest("null_type | 16 | UNSUPPORTED type=0 | null-data burst, 0 B payload");
	wb_free(&b);

	/* 10. two_bursts — two type-13 DTS bursts with pause stuffing between. */
	add_preamble(&b, 13, 0, (uint32_t)(au1n * 8), 0);
	add_payload_16(&b, au1, au1n, 0);
	for (int i = 0; i < 8; i++)
		wb_push(&b, 0x0000);
	add_preamble(&b, 13, 0, (uint32_t)(au1n * 8), 0);
	add_payload_16(&b, au1, au1n, 0);
	write_iec(out, "two_bursts", &b);
	write_pay(out, "two_bursts", au1, au1n); /* each burst payload */
	manifest("two_bursts | 16 | OK,OK type=13 | two DTS bursts, 8-word pause "
	         "between; payload == dts_be16.pay");
	wb_free(&b);

	/* 11. recover — false sync + garbage, then a valid burst re-locks. */
	wb_push(&b, 0x1234);
	wb_push(&b, IEC_PA);   /* false Pa ... */
	wb_push(&b, 0x0000);   /* ... not followed by Pb: bounded re-scan */
	wb_push(&b, 0xABCD);
	wb_push(&b, 0x0000);
	add_preamble(&b, 13, 0, (uint32_t)(au1n * 8), 0);
	add_payload_16(&b, au1, au1n, 0);
	write_iec(out, "recover", &b);
	write_pay(out, "recover", au1, au1n);
	manifest("recover | 16 | OK type=13 | garbage + false Pa, then valid burst; "
	         "payload == dts_be16.pay");
	wb_free(&b);

	/* 12. padded — pause stuffing before and after a single burst. */
	for (int i = 0; i < 6; i++)
		wb_push(&b, 0x0000);
	add_preamble(&b, 13, 0, (uint32_t)(au1n * 8), 0);
	add_payload_16(&b, au1, au1n, 0);
	for (int i = 0; i < 6; i++)
		wb_push(&b, 0x0000);
	write_iec(out, "padded", &b);
	write_pay(out, "padded", au1, au1n);
	manifest("padded | 16 | OK type=13 | pause stuffing around one burst");
	wb_free(&b);

	/* 13. error_flag — Pc error bit set => MALFORMED. */
	add_preamble(&b, 13, 1, 32 * 8, 0);
	synth_fill(syn, 32);
	add_payload_16(&b, syn, 32, 0);
	write_iec(out, "error_flag", &b);
	manifest("error_flag | 16 | MALFORMED type=13 | Pc error flag set");
	wb_free(&b);

	/* 14. zero_len — in-scope type with zero length => MALFORMED. */
	add_preamble(&b, 13, 0, 0, 0);
	write_iec(out, "zero_len", &b);
	manifest("zero_len | 16 | MALFORMED type=13 | length-code 0 on a DTS type");
	wb_free(&b);

	/* 15. zero_len_ac3 — widening Pc 1 makes its zero Pd malformed too. */
	add_preamble(&b, 1, 0, 0, 0);
	write_iec(out, "zero_len_ac3", &b);
	manifest("zero_len_ac3 | 16 | MALFORMED type=1 | Pd=0 on an AC-3 type");
	wb_free(&b);

	/* 16. overlong — length-code beyond the parser buffer => NO_SPACE. */
	add_preamble(&b, 13, 0, 0xFFFFu, 0); /* 65535 bits => 8192 B > capacity */
	write_iec(out, "overlong", &b);
	manifest("overlong | 16 | NO_SPACE type=13 | Pd=0xFFFF (8192 B) > %u cap",
	         AIUS_IEC61937_MAX_PAYLOAD);
	wb_free(&b);

	/* 17. truncated — full header, only half the payload words present. */
	add_preamble(&b, 13, 0, (uint32_t)(au1n * 8), 0);
	add_payload_16(&b, au1, au1n / 2, 0); /* short */
	write_iec(out, "truncated", &b);
	manifest("truncated | 16 | NEED_MORE then finish()->TRUNCATED type=13 | half "
	         "payload only");
	wb_free(&b);

	fclose(g_manifest);
	free(d1);
	free(d2);
	free(d3);
	free(d4);
	fprintf(stderr, "corpus written to %s\n", out);
	return 0;
}
