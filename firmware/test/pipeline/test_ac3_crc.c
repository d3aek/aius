/* Host test for the AC-3 / E-AC-3 syncframe CRC check (aius_ac3_crc_check).
 *
 * The check is what stands between a complete-but-corrupt burst -- the S/PDIF
 * DPLL micro-flap that keeps the pipeline class -- and 32 ms of full-scale
 * crackle that neither liba52 nor the vendored E-AC-3 fork would have noticed.
 * So the spans it uses are PINNED BY THE COMMITTED CORPUS here rather than
 * taken from prose:
 *
 *   1. Table identity. Every verdict is cross-checked against a bitwise
 *      CRC-16/0x8005 written out longhand in this file, so a typo in the
 *      shipped 256-entry table is a failure and not a CRC that accepts
 *      everything.
 *   2. Every good frame passes. Every syncframe of every committed AC-3 vector
 *      (tools/ac3_vectors/vectors) and E-AC-3 vector (tools/eac3_vectors) is
 *      accepted -- 48, 44.1 and 32 kHz, frame_size_code 0 through 36.
 *   3. The spans are unique. For one frame of each family, EVERY interior end
 *      offset is walked: the residue is zero at exactly the 5/8 boundary and
 *      the frame end for AC-3, and at the frame end alone for E-AC-3 (which
 *      carries no crc1 -- its bytes 2-3 are strmtyp/frmsiz).
 *   4. Single-bit sensitivity. Every bit of a whole AC-3 frame and a whole
 *      E-AC-3 frame is flipped in turn: each flip is caught, flips inside the
 *      5/8 prefix by crc1 and the rest by crc2, the two CRC fields' own bits
 *      included.
 *   5. The 5/8 boundary is floor-to-even, not the exact fraction. The 44.1 kHz
 *      frame sizes are ODD word counts (a 138-byte frame's exact 5/8 is 86.25
 *      bytes and the encoder's boundary is 84), and vectors at those sizes are
 *      required to be present rather than merely handled if they happen to be.
 *   6. Guards. NULL and a sub-header-length buffer report TOO_SHORT.
 *
 * Also reports the measured cost of one check per access unit, which is the
 * figure the 8 ms per-AU decode budget has to absorb.
 *
 * Usage: test_ac3_crc <ac3_vectors_dir> <eac3_vectors_dir>
 * Exit 0 on all-pass, 1 on any failure.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aius_pipeline.h"

static int g_fail;

#define CHECK(cond, ...)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			fprintf(stderr, "FAIL: ");                                         \
			fprintf(stderr, __VA_ARGS__);                                      \
			fprintf(stderr, "\n");                                             \
			g_fail++;                                                          \
		}                                                                      \
	} while (0)

/* ---- reference CRC ------------------------------------------------------- */

/* The AC-3 CRC written out longhand: poly 0x8005 (x^16 + x^15 + x^2 + 1),
 * MSB-first, init 0, no reflection and no final inversion. Deliberately
 * bit-at-a-time and deliberately not sharing a line of code with the shipped
 * table-driven one -- that independence is the whole point of the
 * cross-check. */
static uint16_t reference_crc16(const uint8_t *data, size_t from, size_t to)
{
	uint16_t crc = 0u;
	for (size_t index = from; index < to; index++) {
		crc = (uint16_t)(crc ^ ((uint16_t)data[index] << 8));
		for (int bit = 0; bit < 8; bit++) {
			if (crc & 0x8000u)
				crc = (uint16_t)(((crc << 1) & 0xFFFFu) ^ 0x8005u);
			else
				crc = (uint16_t)((crc << 1) & 0xFFFFu);
		}
	}
	return crc;
}

/* The 5/8 boundary, in bytes: quarter plus sixteenth of the byte count,
 * doubled. Floor-to-even, which is what the odd-word 44.1 kHz sizes need. */
static size_t reference_crc1_end(size_t size)
{
	return ((size >> 2) + (size >> 4)) << 1;
}

/* The verdict the shipped check MUST produce, derived independently. */
static AiusAc3CrcVerdict reference_verdict(const uint8_t *frame, size_t size)
{
	if (!frame || size < 7u)
		return AIUS_AC3_CRC_TOO_SHORT;
	const unsigned bsid = (unsigned)(frame[5] >> 3);
	if (bsid <= 10u) {
		const size_t end = reference_crc1_end(size);
		if (end > 2u && end <= size && reference_crc16(frame, 2u, end) != 0u)
			return AIUS_AC3_CRC_BAD_CRC1;
	}
	return reference_crc16(frame, 2u, size) == 0u ? AIUS_AC3_CRC_OK
	                                              : AIUS_AC3_CRC_BAD_CRC2;
}

/* ---- corpus walking ------------------------------------------------------ */

static uint8_t *read_file(const char *dir, const char *name, size_t *len)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) {
		fclose(f);
		return NULL;
	}
	uint8_t *buf = malloc((size_t)n);
	size_t got = buf ? fread(buf, 1, (size_t)n, f) : 0u;
	fclose(f);
	if (!buf || got != (size_t)n) {
		free(buf);
		return NULL;
	}
	*len = got;
	return buf;
}

/* A/52 Table 5.18: frame size in 16-bit WORDS per frame_size_code, per rate.
 * The 44.1 kHz column is the odd-word one -- the whole reason the 5/8
 * boundary cannot be the exact fraction. */
static const uint16_t k_frame_words_48000[38] = {
	64, 64, 80, 80, 96, 96, 112, 112, 128, 128, 160, 160, 192, 192, 224, 224,
	256, 256, 320, 320, 384, 384, 448, 448, 512, 512, 640, 640, 768, 768,
	896, 896, 1024, 1024, 1152, 1152, 1280, 1280
};
static const uint16_t k_frame_words_44100[38] = {
	69, 70, 87, 88, 104, 105, 121, 122, 139, 140, 174, 175, 208, 209, 243,
	244, 278, 279, 348, 349, 417, 418, 487, 488, 557, 558, 696, 697, 835,
	836, 975, 976, 1114, 1115, 1253, 1254, 1393, 1394
};
static const uint16_t k_frame_words_32000[38] = {
	96, 96, 120, 120, 144, 144, 168, 168, 192, 192, 240, 240, 288, 288, 336,
	336, 384, 384, 480, 480, 576, 576, 672, 672, 768, 768, 960, 960, 1152,
	1152, 1344, 1344, 1536, 1536, 1728, 1728, 1920, 1920
};
static const uint32_t k_rate_hz[4] = { 48000u, 44100u, 32000u, 0u };

/* Byte length of the AC-3 syncframe at `data`, or 0 if it is not one. */
static size_t ac3_frame_size(const uint8_t *data, size_t available)
{
	if (available < 6u || data[0] != 0x0Bu || data[1] != 0x77u)
		return 0u;
	const unsigned fscod = (unsigned)(data[4] >> 6);
	const unsigned frmsizecod = (unsigned)(data[4] & 0x3Fu);
	if (fscod > 2u || frmsizecod > 37u)
		return 0u;
	const uint16_t *table = fscod == 0u   ? k_frame_words_48000
	                        : fscod == 1u ? k_frame_words_44100
	                                      : k_frame_words_32000;
	const size_t size = (size_t)table[frmsizecod] * 2u;
	return size <= available ? size : 0u;
}

/* Byte length of the E-AC-3 syncframe at `data`: frmsiz is words-minus-one. */
static size_t eac3_frame_size(const uint8_t *data, size_t available)
{
	if (available < 6u || data[0] != 0x0Bu || data[1] != 0x77u)
		return 0u;
	const size_t words =
		(size_t)(((data[2] & 0x07u) << 8) | data[3]) + 1u;
	const size_t size = words * 2u;
	return size <= available ? size : 0u;
}

/* ---- cases --------------------------------------------------------------- */

struct corpus {
	uint8_t *bytes;
	size_t len;
	const char *name;
};

static const char *k_ac3_files[] = {
	"stereo_impulse.ac3",	 "stereo_tones.ac3",
	"stereo_tones_32000.ac3", "stereo_tones_44100.ac3",
	"stereo_tones_44100_32k.ac3", "surround51_ident.ac3",
	"surround51_impulse.ac3"
};
static const char *k_eac3_files[] = {
	"mono_tone.eac3", "stereo_tones.eac3", "stereo_tones_32000.eac3",
	"stereo_tones_44100.eac3", "surround51_ident.eac3",
	"surround51_impulse.eac3"
};

/* Every syncframe of every committed vector is accepted, and every verdict
 * agrees with the longhand reference. Records which rates and frame_size_codes
 * the corpus actually reached so the coverage claim is measured, not assumed. */
static void test_corpus_frames_pass(const char *ac3_dir, const char *eac3_dir)
{
	int fsc_seen[38];
	int rate_seen[3];
	int odd_word_frames = 0;
	int ac3_frames = 0;
	int eac3_frames = 0;
	memset(fsc_seen, 0, sizeof(fsc_seen));
	memset(rate_seen, 0, sizeof(rate_seen));

	for (size_t f = 0; f < sizeof(k_ac3_files) / sizeof(k_ac3_files[0]); f++) {
		size_t len = 0u;
		uint8_t *bytes = read_file(ac3_dir, k_ac3_files[f], &len);
		CHECK(bytes != NULL, "corpus: cannot read %s", k_ac3_files[f]);
		if (!bytes)
			continue;
		size_t off = 0u;
		while (off < len) {
			const size_t size = ac3_frame_size(bytes + off, len - off);
			CHECK(size != 0u, "%s: no syncframe at offset %zu",
			      k_ac3_files[f], off);
			if (size == 0u)
				break;
			const unsigned fscod = (unsigned)(bytes[off + 4] >> 6);
			const unsigned frmsizecod =
				(unsigned)(bytes[off + 4] & 0x3Fu);
			fsc_seen[frmsizecod] = 1;
			rate_seen[fscod] = 1;
			if ((size / 2u) % 2u == 1u)
				odd_word_frames++;
			const AiusAc3CrcVerdict got =
				aius_ac3_crc_check(bytes + off, size);
			CHECK(got == AIUS_AC3_CRC_OK,
			      "%s+%zu (%zu B, %u Hz, fsc %u): good frame rejected %d",
			      k_ac3_files[f], off, size, k_rate_hz[fscod],
			      frmsizecod, (int)got);
			CHECK(got == reference_verdict(bytes + off, size),
			      "%s+%zu: verdict disagrees with the longhand CRC",
			      k_ac3_files[f], off);
			ac3_frames++;
			off += size;
		}
		free(bytes);
	}

	for (size_t f = 0; f < sizeof(k_eac3_files) / sizeof(k_eac3_files[0]);
	     f++) {
		size_t len = 0u;
		uint8_t *bytes = read_file(eac3_dir, k_eac3_files[f], &len);
		CHECK(bytes != NULL, "corpus: cannot read %s", k_eac3_files[f]);
		if (!bytes)
			continue;
		size_t off = 0u;
		while (off < len) {
			const size_t size = eac3_frame_size(bytes + off, len - off);
			CHECK(size != 0u, "%s: no syncframe at offset %zu",
			      k_eac3_files[f], off);
			if (size == 0u)
				break;
			CHECK((unsigned)(bytes[off + 5] >> 3) == 16u,
			      "%s+%zu: bsid %u is not E-AC-3", k_eac3_files[f], off,
			      (unsigned)(bytes[off + 5] >> 3));
			const AiusAc3CrcVerdict got =
				aius_ac3_crc_check(bytes + off, size);
			CHECK(got == AIUS_AC3_CRC_OK,
			      "%s+%zu (%zu B): good E-AC-3 frame rejected %d",
			      k_eac3_files[f], off, size, (int)got);
			CHECK(got == reference_verdict(bytes + off, size),
			      "%s+%zu: E-AC-3 verdict disagrees with the longhand CRC",
			      k_eac3_files[f], off);
			eac3_frames++;
			off += size;
		}
		free(bytes);
	}

	/* Coverage the corpus is REQUIRED to carry, not merely observed to: the
	 * three link rates, both ends of the frame_size_code range, and the odd
	 * word counts that break an exact-fraction 5/8 reading. */
	CHECK(rate_seen[0] && rate_seen[1] && rate_seen[2],
	      "corpus: rates covered 48k=%d 44.1k=%d 32k=%d", rate_seen[0],
	      rate_seen[1], rate_seen[2]);
	CHECK(fsc_seen[0] && fsc_seen[1] && fsc_seen[36],
	      "corpus: frame_size_code boundaries covered 0=%d 1=%d 36=%d",
	      fsc_seen[0], fsc_seen[1], fsc_seen[36]);
	CHECK(odd_word_frames >= 2,
	      "corpus: only %d odd-word (44.1 kHz) frames -- the floor-to-even "
	      "5/8 boundary is then untested", odd_word_frames);
	int fsc_count = 0;
	for (int i = 0; i < 38; i++)
		fsc_count += fsc_seen[i];
	printf("  corpus         %d AC-3 + %d E-AC-3 frames pass; 3 rates, %d "
	       "frame_size_codes, %d odd-word frames\n",
	       ac3_frames, eac3_frames, fsc_count, odd_word_frames);
}

/* The residue is zero at exactly the two documented ends and nowhere else --
 * the evidence that pins the spans. E-AC-3 has one such end, AC-3 two. */
static void test_span_uniqueness(const char *ac3_dir, const char *eac3_dir)
{
	size_t len = 0u;
	uint8_t *bytes = read_file(ac3_dir, "surround51_ident.ac3", &len);
	CHECK(bytes != NULL, "span: cannot read surround51_ident.ac3");
	if (bytes) {
		const size_t size = ac3_frame_size(bytes, len);
		const size_t n58 = reference_crc1_end(size);
		size_t zeros = 0u;
		size_t at_n58 = 0u;
		size_t at_end = 0u;
		for (size_t end = 4u; end <= size; end++) {
			if (reference_crc16(bytes, 2u, end) != 0u)
				continue;
			zeros++;
			at_n58 += end == n58 ? 1u : 0u;
			at_end += end == size ? 1u : 0u;
		}
		CHECK(zeros == 2u && at_n58 == 1u && at_end == 1u,
		      "span (AC-3 %zu B): %zu zero-residue ends, 5/8=%zu hit %zu, "
		      "end hit %zu", size, zeros, n58, at_n58, at_end);
		printf("  span-ac3       %zu B frame: residue zero at 2..%zu (crc1) "
		       "and 2..%zu (crc2), nowhere else\n", size, n58, size);
		free(bytes);
	}

	len = 0u;
	bytes = read_file(eac3_dir, "surround51_ident.eac3", &len);
	CHECK(bytes != NULL, "span: cannot read surround51_ident.eac3");
	if (bytes) {
		const size_t size = eac3_frame_size(bytes, len);
		size_t zeros = 0u;
		size_t at_end = 0u;
		for (size_t end = 4u; end <= size; end++) {
			if (reference_crc16(bytes, 2u, end) != 0u)
				continue;
			zeros++;
			at_end += end == size ? 1u : 0u;
		}
		CHECK(zeros == 1u && at_end == 1u,
		      "span (E-AC-3 %zu B): %zu zero-residue ends, end hit %zu -- "
		      "E-AC-3 must carry no interior crc1", size, zeros, at_end);
		printf("  span-eac3      %zu B frame: residue zero at 2..%zu only "
		       "(no crc1 in E-AC-3)\n", size, size);
		free(bytes);
	}
}

/* Every single-bit flip in a whole frame is caught, and lands in the arm the
 * span says it should: a flip inside the 5/8 prefix (the two crc1 bytes
 * included) fails crc1, everything after it (the two crc2 bytes included)
 * fails crc2. The syncword itself is outside both spans by construction, so
 * its 16 bits are excluded and counted separately.
 *
 * ONE FAMILY OF FLIPS RE-SELECTS THE ARM, and that is correct rather than a
 * carve-out: byte 5's top five bits are bsid, so flipping one of them can make
 * an AC-3 frame read as E-AC-3 (crc1 skipped, error caught by crc2 instead) or
 * the reverse. The expectation is therefore derived from the MUTATED frame's
 * own bsid, and the count of such arm-switching flips is reported so the
 * behaviour is visible rather than absorbed. */
static void test_single_bit_flips(const char *dir, const char *name,
	                              int is_eac3)
{
	size_t len = 0u;
	uint8_t *bytes = read_file(dir, name, &len);
	CHECK(bytes != NULL, "flip: cannot read %s", name);
	if (!bytes)
		return;
	const size_t size = is_eac3 ? eac3_frame_size(bytes, len)
	                            : ac3_frame_size(bytes, len);
	CHECK(size >= 16u, "flip: %s frame too small (%zu)", name, size);
	if (size < 16u) {
		free(bytes);
		return;
	}
	const size_t n58 = reference_crc1_end(size);

	size_t missed = 0u;
	size_t wrong_arm = 0u;
	size_t disagreed = 0u;
	size_t arm_switched = 0u;
	size_t crc1_hits = 0u;
	size_t crc2_hits = 0u;
	size_t syncword_undetected = 0u;
	for (size_t byte = 0u; byte < size; byte++) {
		for (int bit = 0; bit < 8; bit++) {
			bytes[byte] ^= (uint8_t)(1u << bit);
			const AiusAc3CrcVerdict got =
				aius_ac3_crc_check(bytes, size);
			const AiusAc3CrcVerdict want =
				reference_verdict(bytes, size);
			/* bsid AS THE MUTATED FRAME PRESENTS IT: the arm is a
			 * property of the bytes on the wire, not of the file
			 * the bytes came from. */
			const unsigned bsid = (unsigned)(bytes[5] >> 3);
			bytes[byte] ^= (uint8_t)(1u << bit);
			disagreed += got != want ? 1u : 0u;
			if (byte < 2u) {
				/* Outside both spans: the syncword is the
				 * parser's business, not the CRC's. */
				syncword_undetected += got == AIUS_AC3_CRC_OK ? 1u : 0u;
				continue;
			}
			if ((bsid <= 10u) != !is_eac3)
				arm_switched++;
			if (got == AIUS_AC3_CRC_OK) {
				missed++;
				continue;
			}
			const int expect_crc1 = bsid <= 10u && byte < n58;
			if (got == AIUS_AC3_CRC_BAD_CRC1)
				crc1_hits++;
			else
				crc2_hits++;
			if (expect_crc1 != (got == AIUS_AC3_CRC_BAD_CRC1))
				wrong_arm++;
		}
	}
	CHECK(missed == 0u, "flip %s: %zu single-bit flips passed the CRC", name,
	      missed);
	CHECK(wrong_arm == 0u, "flip %s: %zu flips caught by the wrong span",
	      name, wrong_arm);
	CHECK(disagreed == 0u,
	      "flip %s: %zu verdicts disagree with the longhand CRC", name,
	      disagreed);
	CHECK(syncword_undetected == 16u,
	      "flip %s: %zu/16 syncword bits were (wrongly) inside a CRC span",
	      name, syncword_undetected);
	CHECK(arm_switched > 0u && arm_switched <= 5u,
	      "flip %s: %zu bsid flips re-selected the arm, expected 1..5", name,
	      arm_switched);
	if (is_eac3) {
		CHECK(crc1_hits == arm_switched,
		      "flip %s: %zu crc1 verdicts on an E-AC-3 frame, %zu bsid "
		      "flips", name, crc1_hits, arm_switched);
	} else {
		CHECK(crc1_hits == (n58 - 2u) * 8u - arm_switched,
		      "flip %s: %zu crc1 hits, expected %zu (bytes 2..%zu less "
		      "%zu bsid flips)", name, crc1_hits,
		      (n58 - 2u) * 8u - arm_switched, n58, arm_switched);
	}
	printf("  flip-%-9s %zu B: all %zu payload bit flips caught (%zu crc1, "
	       "%zu crc2, %zu re-selected the arm via bsid)\n",
	       is_eac3 ? "eac3" : "ac3", size, (size - 2u) * 8u, crc1_hits,
	       crc2_hits, arm_switched);
	free(bytes);
}

/* The two CRC fields' own bytes, called out explicitly because they are the
 * fields a reader most expects to be excluded from their own check. */
static void test_crc_field_corruption(const char *dir)
{
	size_t len = 0u;
	uint8_t *bytes = read_file(dir, "stereo_tones.ac3", &len);
	CHECK(bytes != NULL, "crc-field: cannot read stereo_tones.ac3");
	if (!bytes)
		return;
	const size_t size = ac3_frame_size(bytes, len);

	bytes[2] ^= 0x01u; /* crc1 high byte */
	CHECK(aius_ac3_crc_check(bytes, size) == AIUS_AC3_CRC_BAD_CRC1,
	      "crc-field: a flipped crc1 bit is not caught by crc1");
	bytes[2] ^= 0x01u;

	bytes[3] ^= 0x80u; /* crc1 low byte */
	CHECK(aius_ac3_crc_check(bytes, size) == AIUS_AC3_CRC_BAD_CRC1,
	      "crc-field: a flipped crc1 bit is not caught by crc1 (low byte)");
	bytes[3] ^= 0x80u;

	bytes[size - 1u] ^= 0x01u; /* crc2 low byte: the LAST byte of the frame */
	CHECK(aius_ac3_crc_check(bytes, size) == AIUS_AC3_CRC_BAD_CRC2,
	      "crc-field: a flipped crc2 bit is not caught by crc2");
	bytes[size - 1u] ^= 0x01u;

	bytes[size - 2u] ^= 0x40u; /* crc2 high byte */
	CHECK(aius_ac3_crc_check(bytes, size) == AIUS_AC3_CRC_BAD_CRC2,
	      "crc-field: a flipped crc2 bit is not caught by crc2 (high byte)");
	bytes[size - 2u] ^= 0x40u;

	CHECK(aius_ac3_crc_check(bytes, size) == AIUS_AC3_CRC_OK,
	      "crc-field: the frame did not restore cleanly");
	printf("  crc-fields     crc1 and crc2 bytes are each inside their own "
	       "span\n");
	free(bytes);
}

/* The 5/8 boundary over the full frame_size_code range, at all three rates:
 * the formula is floor-to-even and disagrees with the exact fraction on every
 * odd-word 44.1 kHz size. Asserts the disagreement exists (so the rounding is
 * proven load-bearing) and that the boundary always sits strictly inside the
 * frame with room for both CRC fields. */
static void test_frame_size_boundaries(void)
{
	int disagreements = 0;
	for (int frmsizecod = 0; frmsizecod < 38; frmsizecod++) {
		const uint16_t words[3] = { k_frame_words_48000[frmsizecod],
		                            k_frame_words_44100[frmsizecod],
		                            k_frame_words_32000[frmsizecod] };
		for (int rate = 0; rate < 3; rate++) {
			const size_t size = (size_t)words[rate] * 2u;
			const size_t n58 = reference_crc1_end(size);
			CHECK(n58 % 2u == 0u,
			      "boundary fsc %d rate %u: 5/8 end %zu is odd",
			      frmsizecod, k_rate_hz[rate], n58);
			CHECK(n58 > 4u && n58 < size - 2u,
			      "boundary fsc %d rate %u: 5/8 end %zu outside the "
			      "%zu B frame's interior", frmsizecod,
			      k_rate_hz[rate], n58, size);
			if (n58 != (size * 5u) / 8u)
				disagreements++;
		}
	}
	CHECK(disagreements > 0,
	      "boundary: floor-to-even never disagrees with the exact 5/8 -- "
	      "the rounding rule is then untested by this table");
	printf("  boundaries     114 (fsc, rate) sizes: 5/8 end even, interior, "
	       "%d disagree with the exact fraction\n", disagreements);
}

static void test_guards(const char *dir)
{
	size_t len = 0u;
	uint8_t *bytes = read_file(dir, "stereo_tones.ac3", &len);
	CHECK(bytes != NULL, "guards: cannot read stereo_tones.ac3");
	if (!bytes)
		return;
	CHECK(aius_ac3_crc_check(NULL, 2560u) == AIUS_AC3_CRC_TOO_SHORT,
	      "guards: NULL frame");
	for (size_t size = 0u; size < 7u; size++)
		CHECK(aius_ac3_crc_check(bytes, size) == AIUS_AC3_CRC_TOO_SHORT,
		      "guards: %zu-byte buffer is not TOO_SHORT", size);
	/* Seven bytes IS enough to reach bsid, so it is judged rather than
	 * refused -- and a seven-byte prefix of a real frame is not a frame. */
	CHECK(aius_ac3_crc_check(bytes, 7u) != AIUS_AC3_CRC_TOO_SHORT,
	      "guards: a 7-byte buffer must be judged, not refused");
	printf("  guards         NULL and sub-header lengths report TOO_SHORT\n");
	free(bytes);
}

/* What the check costs per access unit, against the 8 ms per-AU decode budget
 * the target holds. Reported, never asserted: a host timing is not a target
 * timing, and a threshold here would be a flaky test rather than a guard. */
static void test_cost(const char *dir)
{
	size_t len = 0u;
	uint8_t *bytes = read_file(dir, "surround51_ident.ac3", &len);
	if (!bytes)
		return;
	const size_t size = ac3_frame_size(bytes, len);
	enum { ITERATIONS = 20000 };
	struct timespec begin, end;
	clock_gettime(CLOCK_MONOTONIC, &begin);
	unsigned accumulator = 0u;
	for (int i = 0; i < ITERATIONS; i++)
		accumulator += (unsigned)aius_ac3_crc_check(bytes, size);
	clock_gettime(CLOCK_MONOTONIC, &end);
	CHECK(accumulator == 0u, "cost: the timed frames did not all pass");
	const double nanos =
		((double)(end.tv_sec - begin.tv_sec) * 1e9 +
		 (double)(end.tv_nsec - begin.tv_nsec)) / (double)ITERATIONS;
	/* Under ASan + UBSan at -O1, which is what this suite builds: every
	 * table load carries a shadow check, so the figure is an upper bound
	 * several times the unsanitized cost, let alone the target's. */
	printf("  cost           %zu B access unit: %.0f ns/AU on this host "
	       "(%.2f ns/byte, ASan+UBSan -O1)\n", size, nanos,
	       nanos / (double)size);
	free(bytes);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr,
		        "usage: %s <ac3_vectors_dir> <eac3_vectors_dir>\n",
		        argv[0]);
		return 2;
	}
	const char *ac3_dir = argv[1];
	const char *eac3_dir = argv[2];

	printf("Aius AC-3 / E-AC-3 syncframe CRC test\n");
	test_corpus_frames_pass(ac3_dir, eac3_dir);
	test_span_uniqueness(ac3_dir, eac3_dir);
	test_single_bit_flips(ac3_dir, "stereo_tones.ac3", 0);
	test_single_bit_flips(eac3_dir, "stereo_tones.eac3", 1);
	test_crc_field_corruption(ac3_dir);
	test_frame_size_boundaries();
	test_guards(ac3_dir);
	test_cost(ac3_dir);

	if (g_fail != 0) {
		printf("RESULT: %d check(s) FAILED\n", g_fail);
		return 1;
	}
	printf("RESULT: all checks passed\n");
	return 0;
}
