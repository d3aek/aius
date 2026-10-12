#include "aius_dsp_config.h"

#include "aius_json.h"

#include <math.h>
#include <string.h>

/* FLASHMEM is Teensyduino's __attribute__((section(".flashmem"))), defined in
 * <avr/pgmspace.h> -- pull in the real definition when building for the
 * target (TEENSYDUINO is a compiler -D flag set project-wide, independent of
 * any #include, so checking it here is reliable). This file otherwise never
 * includes any Teensy/Arduino header, so plain gcc for the native suite falls
 * through to the no-op fallback (mirroring scripts/build_config.py's
 * generated-header shim). */
#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif
/* PROGMEM keeps the decoder's const tables in flash: plain .rodata is copied
 * into DTCM .data by the linker script, and slot-a _ebss is at its ceiling. */
#ifndef PROGMEM
#define PROGMEM
#endif

/* Longest schema key is "surround_gain_linear" (20). 32 leaves headroom and
 * matches the aius_config sibling. A "source" value longer than the buffer is
 * flagged overflow and can never equal "sd". */
#define AIUS_DSP_CONFIG_KEY_BUFFER_MAX 32u
/* Legitimate config numbers are short; a longer token is rejected defensively. */
#define AIUS_DSP_CONFIG_NUMBER_BUFFER_MAX 64u
/* Only the skip-unknown path recurses on attacker structure; cap its depth. */
#define AIUS_DSP_CONFIG_MAX_NESTING 8u

/* EQ bounds (mirror build_config.py). */
#define EQ_FREQ_MIN_HZ 10.0
#define EQ_FREQ_MAX_HZ 24000.0
#define EQ_Q_MIN 0.1
#define EQ_Q_MAX 20.0
#define EQ_GAIN_MIN_DB -24.0
#define EQ_GAIN_MAX_DB 24.0
#define INPUT_TRIM_MIN_DB -60.0
#define INPUT_TRIM_MAX_DB 12.0
#define MATRIX_GAIN_MAX_LINEAR 4.0
/* LFE fold-in crossover corner, mirroring build_config.py's LFE_CROSSOVER_*_HZ
 * and aius_dsp.h's AIUS_DSP_LFE_CROSSOVER_{MIN,MAX}_HZ. 0 is deliberately outside
 * the range: see the AiusDspModelMatrix comment. */
#define MATRIX_LFE_CROSSOVER_MIN_HZ 20.0
#define MATRIX_LFE_CROSSOVER_MAX_HZ 500.0
/* Surround decorrelation delay, mirroring aius_dsp.h's
 * AIUS_DSP_SURROUND_DELAY_MAX_MS (the DTCM ring bound). 0 is inside the range
 * on purpose: it IS the shipped default, the pre-feature undelayed fold. */
#define MATRIX_SURROUND_DELAY_MAX_MS 12.0
#define LIMITER_THRESHOLD_MIN_DBFS -24.0
#define LIMITER_THRESHOLD_MAX_DBFS 0.0
#define LIMITER_ATTACK_MIN_MS 0.1
#define LIMITER_ATTACK_MAX_MS 100.0
#define LIMITER_RELEASE_MIN_MS 1.0
#define LIMITER_RELEASE_MAX_MS 1000.0
#define LIMITER_LOOKAHEAD_MIN_MS 0.0
#define LIMITER_LOOKAHEAD_MAX_MS 5.0

/* This parser only ever runs cold (an HTTP PUT or an SD-config (re)load), never
 * on the audio path, so every function in this file is FLASHMEM (FLASH-resident
 * code, not the tight shared FlexRAM/ITCM pool the audio pipeline needs) --
 * same "Cold code -> FLASH" policy the firmware integration applies to its own
 * new config/HTTP code. GCC does not merge the `section` attribute from a
 * forward declaration onto a later definition that omits it, so FLASHMEM is
 * applied directly on each definition below, not via prototypes. */

FLASHMEM const char *aius_dsp_config_error_message(AiusDspConfigErrorCode code)
{
	switch (code) {
	case AIUS_DSP_CONFIG_OK:
		return "ok";
	case AIUS_DSP_CONFIG_ERR_NULL_ARGUMENT:
		return "null argument";
	case AIUS_DSP_CONFIG_ERR_EMPTY:
		return "empty input";
	case AIUS_DSP_CONFIG_ERR_TOO_LARGE:
		return "input too large";
	case AIUS_DSP_CONFIG_ERR_MALFORMED_JSON:
		return "malformed JSON";
	case AIUS_DSP_CONFIG_ERR_UNKNOWN_KEY:
		return "unknown key";
	case AIUS_DSP_CONFIG_ERR_MISSING_KEY:
		return "missing key";
	case AIUS_DSP_CONFIG_ERR_DUPLICATE_KEY:
		return "duplicate key";
	case AIUS_DSP_CONFIG_ERR_WRONG_TYPE:
		return "wrong type";
	case AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE:
		return "value out of range";
	case AIUS_DSP_CONFIG_ERR_TOO_MANY_EQ:
		return "too many EQ sections";
	case AIUS_DSP_CONFIG_ERR_INVALID_SOURCE:
		return "invalid source";
	case AIUS_DSP_CONFIG_ERR_INVALID_EQ_TYPE:
		return "invalid EQ type";
	default:
		return "invalid error code";
	}
}

FLASHMEM void aius_dsp_model_set_defaults(AiusDspModel *model)
{
	if (model == NULL)
		return;
	memset(model, 0, sizeof(*model));
}

/* The JSON cursor primitives -- at-end, peek, skip-whitespace, consume, string,
 * literal, number, skip-value -- live in lib/aius_json and are shared with
 * lib/aius_config and the measure body reader in src/aius_optical_app.cpp. What
 * stays here is this schema: its key sets, required-key masks, numeric bounds,
 * the double conversion, and this library's error vocabulary.
 *
 * Only the skip-unknown path recurses on attacker structure, and only to this
 * schema's own cap, so every call goes through here rather than repeating it. */
static FLASHMEM int skip_value(AiusJsonCursor *parser)
{
	return aius_json_skip_value(parser, 0u, AIUS_DSP_CONFIG_MAX_NESTING);
}

/* ---- Typed value extraction --------------------------------------------- */

static FLASHMEM int is_number_start(uint8_t ch)
{
	return ch == '-' || (ch >= '0' && ch <= '9');
}

/* Correctly rounded (round-to-nearest-even) decimal -> double, so the GET's own
 * %.17g text decodes bit-identical and any typed value lands on the nearest
 * double. Deliberately NOT strtod(): newlib's strtod/dtoa machinery costs
 * several KB of ITCM this audio-critical firmware cannot spare. Instead:
 *   - fast path (Clinger): mantissa <= 2^53 and |power of ten| <= 22 are both
 *     exact doubles, so one multiply or divide is the single correct rounding;
 *   - slow path (after Go's strconv decimal): shift an exact decimal digit
 *     array by powers of two until it is a 53-bit integer, then round. It lives
 *     in FLASHMEM and uses a stack array, no heap and no libm. */
#define DECODE_DIGITS_MAX 800
#define DECODE_SHIFT_PAD 9 /* a left shift by <= 27 adds at most 9 digits */

typedef struct {
	uint8_t d[DECODE_DIGITS_MAX + DECODE_SHIFT_PAD];
	int nd;
	int dp; /* value = 0.d[0]d[1]... * 10^dp */
	int trunc; /* nonzero digits were dropped beyond DECODE_DIGITS_MAX */
} DecodeDecimal;

static FLASHMEM void decode_trim(DecodeDecimal *a)
{
	while (a->nd > 0 && a->d[a->nd - 1] == 0)
		a->nd--;
	if (a->nd == 0)
		a->dp = 0;
}

/* Multiply by 2^k (k <= 27). */
static FLASHMEM void decode_left_shift(DecodeDecimal *a, unsigned k)
{
	int total = a->nd + DECODE_SHIFT_PAD;
	uint64_t carry = 0;
	for (int p = total - 1; p >= 0; p--) {
		uint64_t n = carry;
		if (p >= DECODE_SHIFT_PAD)
			n += (uint64_t)a->d[p - DECODE_SHIFT_PAD] << k;
		a->d[p] = (uint8_t)(n % 10u);
		carry = n / 10u;
	}
	int s = 0;
	while (s < total && a->d[s] == 0)
		s++;
	memmove(a->d, a->d + s, (size_t)(total - s));
	a->nd = total - s;
	a->dp += DECODE_SHIFT_PAD - s;
	if (a->nd > DECODE_DIGITS_MAX) {
		for (int i = DECODE_DIGITS_MAX; i < a->nd; i++)
			if (a->d[i] != 0)
				a->trunc = 1;
		a->nd = DECODE_DIGITS_MAX;
	}
	decode_trim(a);
}

/* Divide by 2^k (k <= 27). */
static FLASHMEM void decode_right_shift(DecodeDecimal *a, unsigned k)
{
	int r = 0, w = 0;
	uint64_t n = 0;
	const uint64_t mask = ((uint64_t)1 << k) - 1u;
	while ((n >> k) == 0) {
		if (r >= a->nd) {
			if (n == 0) {
				a->nd = 0;
				a->dp = 0;
				return;
			}
			while ((n >> k) == 0) {
				n *= 10u;
				r++;
			}
			break;
		}
		n = n * 10u + a->d[r];
		r++;
	}
	a->dp -= r - 1;
	for (; r < a->nd; r++) {
		uint8_t digit = (uint8_t)(n >> k);
		n &= mask;
		a->d[w++] = digit;
		n = n * 10u + a->d[r];
	}
	while (n > 0) {
		uint8_t digit = (uint8_t)(n >> k);
		n &= mask;
		if (w < DECODE_DIGITS_MAX)
			a->d[w++] = digit;
		else if (digit != 0)
			a->trunc = 1;
		n *= 10u;
	}
	a->nd = w;
	decode_trim(a);
}

static FLASHMEM void decode_shift(DecodeDecimal *a, int k)
{
	while (k > 27) {
		decode_left_shift(a, 27u);
		k -= 27;
	}
	while (k < -27) {
		decode_right_shift(a, 27u);
		k += 27;
	}
	if (k > 0)
		decode_left_shift(a, (unsigned)k);
	else if (k < 0)
		decode_right_shift(a, (unsigned)-k);
}

static FLASHMEM double decode_pack(int negative, uint64_t bits)
{
	bits |= (uint64_t)(negative != 0) << 63;
	double value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static FLASHMEM double decode_slow(DecodeDecimal *a, int negative)
{
	static const uint8_t POW_TAB[] PROGMEM = { 1, 3, 6, 9, 13, 16, 19, 23, 26 };
	const int pow_count = (int)(sizeof(POW_TAB) / sizeof(POW_TAB[0]));
	if (a->nd == 0)
		return decode_pack(negative, 0u);
	if (a->dp > 310)
		return decode_pack(negative, 0x7ff0000000000000ull); /* inf */
	if (a->dp < -330)
		return decode_pack(negative, 0u);

	int exp2 = 0;
	while (a->dp > 0) {
		int n = a->dp >= pow_count ? 27 : POW_TAB[a->dp];
		decode_shift(a, -n);
		exp2 += n;
	}
	while (a->dp < 0 || (a->dp == 0 && a->nd > 0 && a->d[0] < 5)) {
		int n = -a->dp >= pow_count ? 27 : POW_TAB[-a->dp];
		decode_shift(a, n);
		exp2 -= n;
	}
	/* Now 0.5 <= a < 1; make it 1 <= x < 2. */
	exp2--;
	if (exp2 < -1022) {
		int n = -1022 - exp2;
		decode_shift(a, -n);
		exp2 += n;
	}
	if (exp2 + 1023 >= 2047)
		return decode_pack(negative, 0x7ff0000000000000ull);

	decode_shift(a, 53);
	uint64_t mant = 0;
	int i = 0;
	for (; i < a->dp && i < a->nd; i++)
		mant = mant * 10u + a->d[i];
	for (; i < a->dp; i++)
		mant *= 10u;
	int round_up = 0;
	if (a->dp >= 0 && a->dp < a->nd) {
		if (a->d[a->dp] == 5 && a->dp + 1 == a->nd)
			round_up = a->trunc || (a->dp > 0 && (a->d[a->dp - 1] & 1u));
		else
			round_up = a->d[a->dp] >= 5;
	}
	if (round_up)
		mant++;
	if (mant == ((uint64_t)1 << 53)) {
		mant >>= 1;
		exp2++;
		if (exp2 + 1023 >= 2047)
			return decode_pack(negative, 0x7ff0000000000000ull);
	}
	if ((mant & ((uint64_t)1 << 52)) == 0)
		exp2 = -1023; /* denormal (or zero): biased exponent 0 */
	uint64_t bits = (mant & (((uint64_t)1 << 52) - 1u)) |
	                ((uint64_t)(exp2 + 1023) << 52);
	return decode_pack(negative, bits);
}

/* Convert an already grammar-validated JSON number's [start, end) byte range
 * (per aius_json_scan_number, so at most NUMBER_BUFFER_MAX - 1 bytes) into the
 * nearest double. A zero mantissa with an explicit exponent above 308 yields
 * NaN, as the old digit-scaling decoder did (0 * inf), so that input stays
 * refused as out of range. */
static FLASHMEM double decode_scanned_number(const AiusJsonCursor *parser,
	                                            size_t start, size_t end)
{
	static const double POW10[] PROGMEM = {
		1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
		1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
	};
	DecodeDecimal dec;
	size_t i = start;
	int negative = 0;
	if (parser->data[i] == '-') {
		negative = 1;
		i++;
	}
	dec.nd = 0;
	dec.dp = 0;
	dec.trunc = 0;
	int saw_dot = 0;
	int total_digits = 0; /* digits kept, including interior and trailing zeros */
	uint64_t fast_mant = 0;
	for (; i < end; i++) {
		uint8_t c = parser->data[i];
		if (c == '.') {
			saw_dot = 1;
			dec.dp = dec.nd;
			continue;
		}
		if (c < '0' || c > '9')
			break;
		if (c == '0' && dec.nd == 0) {
			dec.dp--; /* leading zero */
			continue;
		}
		if (dec.nd < DECODE_DIGITS_MAX)
			dec.d[dec.nd++] = (uint8_t)(c - '0');
		else if (c != '0')
			dec.trunc = 1;
		if (total_digits < 19)
			fast_mant = fast_mant * 10u + (uint64_t)(c - '0');
		total_digits++;
	}
	if (!saw_dot)
		dec.dp = dec.nd;

	int exponent = 0;
	if (i < end && (parser->data[i] == 'e' || parser->data[i] == 'E')) {
		int exponent_negative = 0;
		i++;
		if (i < end && (parser->data[i] == '+' || parser->data[i] == '-')) {
			exponent_negative = parser->data[i] == '-';
			i++;
		}
		while (i < end && parser->data[i] >= '0' && parser->data[i] <= '9') {
			if (exponent < 10000)
				exponent = exponent * 10 + (parser->data[i] - '0');
			i++;
		}
		if (exponent_negative)
			exponent = -exponent;
	}

	if (dec.nd == 0) {
		if (exponent > 308)
			return (double)NAN;
		return decode_pack(negative, 0u);
	}
	dec.dp += exponent;

	/* Clinger fast path; fast_mant holds every digit only when <= 19 kept. */
	int e10 = dec.dp - total_digits;
	if (total_digits <= 19 && fast_mant <= ((uint64_t)1 << 53) &&
	    e10 >= -22 && e10 <= 22) {
		double value = (double)fast_mant;
		value = e10 < 0 ? value / POW10[-e10] : value * POW10[e10];
		return negative ? -value : value;
	}
	decode_trim(&dec);
	return decode_slow(&dec, negative);
}

/* Extract a finite JSON number as a double. A well-formed number whose text is
 * longer than the defensive cap, or that overflows to non-finite, is out of
 * range. A non-number JSON value is wrong type; garbage is malformed. */
static FLASHMEM AiusDspConfigErrorCode take_number(AiusJsonCursor *parser,
	                                          double *out)
{
	if (is_number_start(aius_json_peek(parser))) {
		size_t start, end;
		if (!aius_json_scan_number(parser, &start, &end))
			return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
		if (end - start >= AIUS_DSP_CONFIG_NUMBER_BUFFER_MAX)
			return AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE;
		double value = decode_scanned_number(parser, start, end);
		if (!isfinite(value))
			return AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE;
		*out = value;
		return AIUS_DSP_CONFIG_OK;
	}
	if (!skip_value(parser))
		return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
	return AIUS_DSP_CONFIG_ERR_WRONG_TYPE;
}

static FLASHMEM AiusDspConfigErrorCode take_bounded(AiusJsonCursor *parser,
	                                           double low, double high,
	                                           double *out)
{
	double value = 0.0;
	AiusDspConfigErrorCode error = take_number(parser, &value);
	if (error != AIUS_DSP_CONFIG_OK)
		return error;
	if (value < low || value > high)
		return AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE;
	*out = value;
	return AIUS_DSP_CONFIG_OK;
}

static FLASHMEM AiusDspConfigErrorCode take_bool(AiusJsonCursor *parser,
	                                        uint8_t *out)
{
	if (aius_json_peek(parser) == 'f') {
		if (!aius_json_literal(parser, "false"))
			return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
		*out = 0u;
		return AIUS_DSP_CONFIG_OK;
	}
	if (aius_json_peek(parser) == 't') {
		if (!aius_json_literal(parser, "true"))
			return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
		*out = 1u;
		return AIUS_DSP_CONFIG_OK;
	}
	if (!skip_value(parser))
		return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
	return AIUS_DSP_CONFIG_ERR_WRONG_TYPE;
}

/* A JSON number that is a non-negative integer (integral value, >= 0). */
static FLASHMEM AiusDspConfigErrorCode take_non_negative_integer(AiusJsonCursor *parser,
	                                                        double *out)
{
	double value = 0.0;
	AiusDspConfigErrorCode error = take_number(parser, &value);
	if (error != AIUS_DSP_CONFIG_OK)
		return error;
	if (value < 0.0 || floor(value) != value)
		return AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE;
	*out = value;
	return AIUS_DSP_CONFIG_OK;
}

/* ---- Object dispatch loop helper ---------------------------------------- */

/* Parse one `"key" :` pair prologue: the key string, the delimiter, and leave
 * the cursor at the value. On success returns OK and fills key/len/overflow. */
static FLASHMEM AiusDspConfigErrorCode take_key(AiusJsonCursor *parser,
	                                       uint8_t *key, size_t cap,
	                                       size_t *key_len, int *overflow)
{
	if (!aius_json_take_string(parser, key, cap, key_len, overflow))
		return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
	aius_json_skip_whitespace(parser);
	return AIUS_DSP_CONFIG_OK;
}

static FLASHMEM AiusDspConfigErrorCode consume_colon(AiusJsonCursor *parser)
{
	if (!aius_json_consume(parser, ':'))
		return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
	aius_json_skip_whitespace(parser);
	return AIUS_DSP_CONFIG_OK;
}

/* Advance past a value separator. Returns 1 to continue the loop, 0 to stop on
 * '}', and sets *error on malformed separators. */
static FLASHMEM int object_next(AiusJsonCursor *parser, AiusDspConfigErrorCode *error)
{
	aius_json_skip_whitespace(parser);
	if (aius_json_consume(parser, '}'))
		return 0;
	if (!aius_json_consume(parser, ',')) {
		*error = AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
		return 0;
	}
	aius_json_skip_whitespace(parser);
	if (aius_json_peek(parser) == '}') {
		*error = AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
		return 0;
	}
	return 1;
}

/* ---- Schema object parsers ---------------------------------------------- */

static FLASHMEM AiusDspConfigErrorCode parse_eq_section(AiusJsonCursor *parser,
	                                               AiusDspModelEq *out)
{
	if (aius_json_peek(parser) != '{') {
		if (!skip_value(parser))
			return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
		return AIUS_DSP_CONFIG_ERR_WRONG_TYPE;
	}
	parser->pos++; /* '{' */
	aius_json_skip_whitespace(parser);

	uint8_t seen = 0u;
	uint8_t type = 0u;
	int have_type = 0;
	double freq = 0.0, q = 0.0, gain = 0.0;
	int have_freq = 0, have_q = 0, have_gain = 0;

	if (!aius_json_consume(parser, '}')) {
		for (;;) {
			uint8_t key[AIUS_DSP_CONFIG_KEY_BUFFER_MAX];
			size_t key_len = 0;
			int overflow = 0;
			AiusDspConfigErrorCode error =
				take_key(parser, key, sizeof(key), &key_len, &overflow);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			unsigned int field;
			if (aius_json_key_is(key, key_len, overflow, "type"))
				field = 0u;
			else if (aius_json_key_is(key, key_len, overflow, "frequency_hz"))
				field = 1u;
			else if (aius_json_key_is(key, key_len, overflow, "q"))
				field = 2u;
			else if (aius_json_key_is(key, key_len, overflow, "gain_db"))
				field = 3u;
			else
				return AIUS_DSP_CONFIG_ERR_UNKNOWN_KEY;

			uint8_t bit = (uint8_t)(1u << field);
			if ((seen & bit) != 0u)
				return AIUS_DSP_CONFIG_ERR_DUPLICATE_KEY;
			seen |= bit;

			error = consume_colon(parser);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			if (field == 0u) {
				uint8_t type_str[AIUS_DSP_CONFIG_KEY_BUFFER_MAX];
				size_t type_len = 0;
				int type_overflow = 0;
				if (aius_json_peek(parser) != '"') {
					if (!skip_value(parser))
						return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
					return AIUS_DSP_CONFIG_ERR_WRONG_TYPE;
				}
				if (!aius_json_take_string(parser, type_str,
				                           sizeof(type_str), &type_len,
				                           &type_overflow))
					return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
				if (aius_json_key_is(type_str, type_len, type_overflow, "bypass"))
					type = 0u;
				else if (aius_json_key_is(type_str, type_len, type_overflow, "highpass"))
					type = 1u;
				else if (aius_json_key_is(type_str, type_len, type_overflow, "peaking"))
					type = 2u;
				else if (aius_json_key_is(type_str, type_len, type_overflow, "lowpass"))
					type = 3u;
				else
					return AIUS_DSP_CONFIG_ERR_INVALID_EQ_TYPE;
				have_type = 1;
			} else {
				double value = 0.0;
				error = take_number(parser, &value);
				if (error != AIUS_DSP_CONFIG_OK)
					return error;
				if (field == 1u) {
					freq = value;
					have_freq = 1;
				} else if (field == 2u) {
					q = value;
					have_q = 1;
				} else {
					gain = value;
					have_gain = 1;
				}
			}

			AiusDspConfigErrorCode sep = AIUS_DSP_CONFIG_OK;
			if (!object_next(parser, &sep)) {
				if (sep != AIUS_DSP_CONFIG_OK)
					return sep;
				break;
			}
		}
	}

	/* build_config.py treats a missing/unknown type as "unsupported EQ type". */
	if (!have_type)
		return AIUS_DSP_CONFIG_ERR_INVALID_EQ_TYPE;
	/* Non-bypass sections bounds-check freq/q/gain (absent -> 0, mirroring
	 * build_config.py's section.get(..., 0)). */
	if (type != 0u) {
		double f = have_freq ? freq : 0.0;
		double qq = have_q ? q : 0.0;
		double g = have_gain ? gain : 0.0;
		if (f < EQ_FREQ_MIN_HZ || f > EQ_FREQ_MAX_HZ)
			return AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE;
		if (qq < EQ_Q_MIN || qq > EQ_Q_MAX)
			return AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE;
		if (g < EQ_GAIN_MIN_DB || g > EQ_GAIN_MAX_DB)
			return AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE;
	}

	out->type = type;
	out->frequency_hz = have_freq ? freq : 0.0;
	out->q = have_q ? q : 0.0;
	out->gain_db = have_gain ? gain : 0.0;
	return AIUS_DSP_CONFIG_OK;
}

static FLASHMEM AiusDspConfigErrorCode parse_eq_array(AiusJsonCursor *parser,
	                                             AiusDspModelEq *eq,
	                                             uint8_t *count)
{
	*count = 0u;
	if (aius_json_peek(parser) != '[') {
		if (!skip_value(parser))
			return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
		return AIUS_DSP_CONFIG_ERR_WRONG_TYPE;
	}
	parser->pos++; /* '[' */
	aius_json_skip_whitespace(parser);
	if (aius_json_consume(parser, ']'))
		return AIUS_DSP_CONFIG_OK;
	for (;;) {
		if (*count >= AIUS_DSP_MODEL_EQ_MAX)
			return AIUS_DSP_CONFIG_ERR_TOO_MANY_EQ;
		AiusDspConfigErrorCode error = parse_eq_section(parser, &eq[*count]);
		if (error != AIUS_DSP_CONFIG_OK)
			return error;
		(*count)++;
		aius_json_skip_whitespace(parser);
		if (aius_json_consume(parser, ']'))
			return AIUS_DSP_CONFIG_OK;
		if (!aius_json_consume(parser, ','))
			return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
		aius_json_skip_whitespace(parser);
		if (aius_json_peek(parser) == ']')
			return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
	}
}

static FLASHMEM AiusDspConfigErrorCode parse_input_channel(AiusJsonCursor *parser,
	                                                  AiusDspModelInputChannel *out)
{
	if (aius_json_peek(parser) != '{') {
		if (!skip_value(parser))
			return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
		return AIUS_DSP_CONFIG_ERR_WRONG_TYPE;
	}
	parser->pos++;
	aius_json_skip_whitespace(parser);

	uint8_t seen = 0u;
	double gain = 0.0;
	uint8_t eq_count = 0u;
	AiusDspModelEq eq[AIUS_DSP_MODEL_EQ_MAX];
	memset(eq, 0, sizeof(eq));

	if (!aius_json_consume(parser, '}')) {
		for (;;) {
			uint8_t key[AIUS_DSP_CONFIG_KEY_BUFFER_MAX];
			size_t key_len = 0;
			int overflow = 0;
			AiusDspConfigErrorCode error =
				take_key(parser, key, sizeof(key), &key_len, &overflow);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			unsigned int field;
			if (aius_json_key_is(key, key_len, overflow, "gain_db"))
				field = 0u;
			else if (aius_json_key_is(key, key_len, overflow, "eq"))
				field = 1u;
			else
				return AIUS_DSP_CONFIG_ERR_UNKNOWN_KEY;

			uint8_t bit = (uint8_t)(1u << field);
			if ((seen & bit) != 0u)
				return AIUS_DSP_CONFIG_ERR_DUPLICATE_KEY;
			seen |= bit;

			error = consume_colon(parser);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			if (field == 0u)
				error = take_bounded(parser, INPUT_TRIM_MIN_DB,
				                     INPUT_TRIM_MAX_DB, &gain);
			else
				error = parse_eq_array(parser, eq, &eq_count);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			AiusDspConfigErrorCode sep = AIUS_DSP_CONFIG_OK;
			if (!object_next(parser, &sep)) {
				if (sep != AIUS_DSP_CONFIG_OK)
					return sep;
				break;
			}
		}
	}

	if (seen != 0x03u)
		return AIUS_DSP_CONFIG_ERR_MISSING_KEY;
	out->gain_db = gain;
	out->eq_count = eq_count;
	memcpy(out->eq, eq, sizeof(out->eq));
	return AIUS_DSP_CONFIG_OK;
}

static FLASHMEM AiusDspConfigErrorCode parse_input_channels(AiusJsonCursor *parser,
	                                                   AiusDspModel *model)
{
	static const char *const names[AIUS_DSP_MODEL_INPUT_CHANNELS] = {
		"fl", "fr", "fc", "lfe", "sl", "sr"
	};
	if (aius_json_peek(parser) != '{') {
		if (!skip_value(parser))
			return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
		return AIUS_DSP_CONFIG_ERR_WRONG_TYPE;
	}
	parser->pos++;
	aius_json_skip_whitespace(parser);

	uint8_t seen = 0u;
	if (!aius_json_consume(parser, '}')) {
		for (;;) {
			uint8_t key[AIUS_DSP_CONFIG_KEY_BUFFER_MAX];
			size_t key_len = 0;
			int overflow = 0;
			AiusDspConfigErrorCode error =
				take_key(parser, key, sizeof(key), &key_len, &overflow);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			unsigned int index = AIUS_DSP_MODEL_INPUT_CHANNELS;
			for (unsigned int i = 0; i < AIUS_DSP_MODEL_INPUT_CHANNELS; i++) {
				if (aius_json_key_is(key, key_len, overflow, names[i])) {
					index = i;
					break;
				}
			}
			if (index == AIUS_DSP_MODEL_INPUT_CHANNELS)
				return AIUS_DSP_CONFIG_ERR_UNKNOWN_KEY;

			uint8_t bit = (uint8_t)(1u << index);
			if ((seen & bit) != 0u)
				return AIUS_DSP_CONFIG_ERR_DUPLICATE_KEY;
			seen |= bit;

			error = consume_colon(parser);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;
			error = parse_input_channel(parser, &model->input_channels[index]);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			AiusDspConfigErrorCode sep = AIUS_DSP_CONFIG_OK;
			if (!object_next(parser, &sep)) {
				if (sep != AIUS_DSP_CONFIG_OK)
					return sep;
				break;
			}
		}
	}

	if (seen != 0x3Fu)
		return AIUS_DSP_CONFIG_ERR_MISSING_KEY;
	return AIUS_DSP_CONFIG_OK;
}

static FLASHMEM AiusDspConfigErrorCode parse_features(AiusJsonCursor *parser,
	                                             AiusDspModelFeatures *out)
{
	if (aius_json_peek(parser) != '{') {
		if (!skip_value(parser))
			return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
		return AIUS_DSP_CONFIG_ERR_WRONG_TYPE;
	}
	parser->pos++;
	aius_json_skip_whitespace(parser);

	uint8_t seen = 0u;
	AiusDspModelFeatures features = {0u, 0u, 0u, 0u, 0u};
	if (!aius_json_consume(parser, '}')) {
		for (;;) {
			uint8_t key[AIUS_DSP_CONFIG_KEY_BUFFER_MAX];
			size_t key_len = 0;
			int overflow = 0;
			AiusDspConfigErrorCode error =
				take_key(parser, key, sizeof(key), &key_len, &overflow);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			unsigned int field;
			if (aius_json_key_is(key, key_len, overflow, "input_eq"))
				field = 0u;
			else if (aius_json_key_is(key, key_len, overflow, "matrix"))
				field = 1u;
			else if (aius_json_key_is(key, key_len, overflow, "output_eq"))
				field = 2u;
			else if (aius_json_key_is(key, key_len, overflow, "limiter"))
				field = 3u;
			else if (aius_json_key_is(key, key_len, overflow, "meters"))
				field = 4u;
			else
				return AIUS_DSP_CONFIG_ERR_UNKNOWN_KEY;

			uint8_t bit = (uint8_t)(1u << field);
			if ((seen & bit) != 0u)
				return AIUS_DSP_CONFIG_ERR_DUPLICATE_KEY;
			seen |= bit;

			error = consume_colon(parser);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;
			uint8_t value = 0u;
			error = take_bool(parser, &value);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;
			switch (field) {
			case 0u: features.input_eq = value; break;
			case 1u: features.matrix = value; break;
			case 2u: features.output_eq = value; break;
			case 3u: features.limiter = value; break;
			default: features.meters = value; break;
			}

			AiusDspConfigErrorCode sep = AIUS_DSP_CONFIG_OK;
			if (!object_next(parser, &sep)) {
				if (sep != AIUS_DSP_CONFIG_OK)
					return sep;
				break;
			}
		}
	}

	if (seen != 0x1Fu)
		return AIUS_DSP_CONFIG_ERR_MISSING_KEY;
	*out = features;
	return AIUS_DSP_CONFIG_OK;
}

static FLASHMEM AiusDspConfigErrorCode parse_matrix(AiusJsonCursor *parser,
	                                           AiusDspModelMatrix *out)
{
	if (aius_json_peek(parser) != '{') {
		if (!skip_value(parser))
			return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
		return AIUS_DSP_CONFIG_ERR_WRONG_TYPE;
	}
	parser->pos++;
	aius_json_skip_whitespace(parser);

	uint8_t seen = 0u;
	/* THE HOME OF THE ABSENT-READS-ON DEFAULT for layout_aware (the trailing 1u).
	 * It cannot live in aius_dsp_model_set_defaults(), which is a memset to zero,
	 * and a body omitting the key must not silently ask for the legacy uniform
	 * pad -- every config written before the field existed omits it.
	 * surround_delay_ms's absent default is 0.0 (the undelayed fold), which a
	 * memset also produces, so it has no such split home. */
	AiusDspModelMatrix matrix = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0u, 1u};
	if (!aius_json_consume(parser, '}')) {
		for (;;) {
			uint8_t key[AIUS_DSP_CONFIG_KEY_BUFFER_MAX];
			size_t key_len = 0;
			int overflow = 0;
			AiusDspConfigErrorCode error =
				take_key(parser, key, sizeof(key), &key_len, &overflow);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			unsigned int field;
			if (aius_json_key_is(key, key_len, overflow, "center_gain_linear"))
				field = 0u;
			else if (aius_json_key_is(key, key_len, overflow, "surround_gain_linear"))
				field = 1u;
			else if (aius_json_key_is(key, key_len, overflow, "lfe_gain_linear"))
				field = 2u;
			else if (aius_json_key_is(key, key_len, overflow, "headroom_linear"))
				field = 3u;
			else if (aius_json_key_is(key, key_len, overflow, "clipfree"))
				field = 4u;
			else if (aius_json_key_is(key, key_len, overflow, "lfe_crossover_hz"))
				field = 5u;
			else if (aius_json_key_is(key, key_len, overflow, "layout_aware"))
				field = 6u;
			else if (aius_json_key_is(key, key_len, overflow, "surround_delay_ms"))
				field = 7u;
			else
				return AIUS_DSP_CONFIG_ERR_UNKNOWN_KEY;

			uint8_t bit = (uint8_t)(1u << field);
			if ((seen & bit) != 0u)
				return AIUS_DSP_CONFIG_ERR_DUPLICATE_KEY;
			seen |= bit;

			error = consume_colon(parser);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			switch (field) {
			case 0u:
				error = take_bounded(parser, 0.0, MATRIX_GAIN_MAX_LINEAR,
				                     &matrix.center_gain_linear);
				break;
			case 1u:
				error = take_bounded(parser, 0.0, MATRIX_GAIN_MAX_LINEAR,
				                     &matrix.surround_gain_linear);
				break;
			case 2u:
				error = take_bounded(parser, 0.0, MATRIX_GAIN_MAX_LINEAR,
				                     &matrix.lfe_gain_linear);
				break;
			case 3u: {
				double headroom = 0.0;
				error = take_number(parser, &headroom);
				if (error == AIUS_DSP_CONFIG_OK &&
				    (headroom <= 0.0 || headroom > 1.0))
					error = AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE;
				if (error == AIUS_DSP_CONFIG_OK)
					matrix.headroom_linear = headroom;
				break;
			}
			case 4u:
				error = take_bool(parser, &matrix.clipfree);
				break;
			case 5u:
				error = take_bounded(parser, MATRIX_LFE_CROSSOVER_MIN_HZ,
				                     MATRIX_LFE_CROSSOVER_MAX_HZ,
				                     &matrix.lfe_crossover_hz);
				break;
			case 6u:
				error = take_bool(parser, &matrix.layout_aware);
				break;
			default:
				error = take_bounded(parser, 0.0,
				                     MATRIX_SURROUND_DELAY_MAX_MS,
				                     &matrix.surround_delay_ms);
				break;
			}
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			AiusDspConfigErrorCode sep = AIUS_DSP_CONFIG_OK;
			if (!object_next(parser, &sep)) {
				if (sep != AIUS_DSP_CONFIG_OK)
					return sep;
				break;
			}
		}
	}

	/* MASK, NOT EQUALITY, and the difference is the whole point: the six legacy
	 * keys (bits 0..5) stay REQUIRED, while layout_aware (bit 6) and
	 * surround_delay_ms (bit 7) are optional -- both landed after configs
	 * existed. The naive full-mask equality would reject every config that does
	 * NOT carry the new fields -- which is every config written before they
	 * existed -- and `seen != 0x3Fu` would reject every config that DOES, which
	 * is inverted from the intent in the more embarrassing direction: it 400s
	 * the editor's own body. */
	if ((seen & 0x3Fu) != 0x3Fu)
		return AIUS_DSP_CONFIG_ERR_MISSING_KEY;
	*out = matrix;
	return AIUS_DSP_CONFIG_OK;
}

static FLASHMEM AiusDspConfigErrorCode parse_limiter(AiusJsonCursor *parser,
	                                            AiusDspModelLimiter *out)
{
	if (aius_json_peek(parser) != '{') {
		if (!skip_value(parser))
			return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
		return AIUS_DSP_CONFIG_ERR_WRONG_TYPE;
	}
	parser->pos++;
	aius_json_skip_whitespace(parser);

	uint8_t seen = 0u;
	AiusDspModelLimiter limiter = {0u, 0.0, 0.0, 0.0, 0.0};
	if (!aius_json_consume(parser, '}')) {
		for (;;) {
			uint8_t key[AIUS_DSP_CONFIG_KEY_BUFFER_MAX];
			size_t key_len = 0;
			int overflow = 0;
			AiusDspConfigErrorCode error =
				take_key(parser, key, sizeof(key), &key_len, &overflow);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			unsigned int field;
			if (aius_json_key_is(key, key_len, overflow, "enabled"))
				field = 0u;
			else if (aius_json_key_is(key, key_len, overflow, "threshold_dbfs"))
				field = 1u;
			else if (aius_json_key_is(key, key_len, overflow, "attack_ms"))
				field = 2u;
			else if (aius_json_key_is(key, key_len, overflow, "release_ms"))
				field = 3u;
			else if (aius_json_key_is(key, key_len, overflow, "lookahead_ms"))
				field = 4u;
			else
				return AIUS_DSP_CONFIG_ERR_UNKNOWN_KEY;

			uint8_t bit = (uint8_t)(1u << field);
			if ((seen & bit) != 0u)
				return AIUS_DSP_CONFIG_ERR_DUPLICATE_KEY;
			seen |= bit;

			error = consume_colon(parser);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			switch (field) {
			case 0u:
				error = take_bool(parser, &limiter.enabled);
				break;
			case 1u:
				error = take_bounded(parser, LIMITER_THRESHOLD_MIN_DBFS,
				                     LIMITER_THRESHOLD_MAX_DBFS,
				                     &limiter.threshold_dbfs);
				break;
			case 2u:
				error = take_bounded(parser, LIMITER_ATTACK_MIN_MS,
				                     LIMITER_ATTACK_MAX_MS, &limiter.attack_ms);
				break;
			case 3u:
				error = take_bounded(parser, LIMITER_RELEASE_MIN_MS,
				                     LIMITER_RELEASE_MAX_MS, &limiter.release_ms);
				break;
			default:
				error = take_bounded(parser, LIMITER_LOOKAHEAD_MIN_MS,
				                     LIMITER_LOOKAHEAD_MAX_MS,
				                     &limiter.lookahead_ms);
				break;
			}
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			AiusDspConfigErrorCode sep = AIUS_DSP_CONFIG_OK;
			if (!object_next(parser, &sep)) {
				if (sep != AIUS_DSP_CONFIG_OK)
					return sep;
				break;
			}
		}
	}

	if (seen != 0x1Fu)
		return AIUS_DSP_CONFIG_ERR_MISSING_KEY;
	*out = limiter;
	return AIUS_DSP_CONFIG_OK;
}

static FLASHMEM AiusDspConfigErrorCode parse_source(AiusJsonCursor *parser)
{
	uint8_t value[AIUS_DSP_CONFIG_KEY_BUFFER_MAX];
	size_t value_len = 0;
	int overflow = 0;
	if (aius_json_peek(parser) != '"') {
		if (!skip_value(parser))
			return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
		return AIUS_DSP_CONFIG_ERR_WRONG_TYPE;
	}
	if (!aius_json_take_string(parser, value, sizeof(value), &value_len, &overflow))
		return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
	/* Both values GET can serve are accepted, and the value is then IGNORED.
	 *
	 * `source` is a read-only status field: it reports which model is active, and
	 * `GET /api/dsp/config` emits `g_dsp_source_sd ? "sd" : "default"`. But it is
	 * also one of the nine REQUIRED top-level keys, and this function used to
	 * accept only "sd" -- so on a board with no SD-hosted config, which serves
	 * "default", the natural GET-then-PUT round-trip was refused 400
	 * `dsp_config_invalid`. That is how B6 gate 5 failed: not a runner problem, a
	 * contract defect on an untouched board.
	 *
	 * Ignoring the value is correct rather than lax. A successful PUT always
	 * writes `/aius/dsp.json`, so the resulting source is always "sd" whatever the
	 * body claimed; honouring the field would mean letting a client assert a fact
	 * the server determines. Rejecting a well-formed round-trip to defend a field
	 * that cannot influence the outcome is the worst of both.
	 *
	 * Still validated as an enum rather than skipped: an unrecognised string is
	 * still INVALID_SOURCE, so a typo or a wrong-shaped body is caught. The SD
	 * editor sends "sd" and is unaffected either way -- it hardcodes the value,
	 * which is exactly why this went unnoticed. */
	if (!aius_json_key_is(value, value_len, overflow, "sd") &&
	    !aius_json_key_is(value, value_len, overflow, "default"))
		return AIUS_DSP_CONFIG_ERR_INVALID_SOURCE;
	return AIUS_DSP_CONFIG_OK;
}

/* ---- Top level ----------------------------------------------------------- */

/* Bit positions for the nine required top-level keys, then the response-only
 * keys GET /api/dsp/config adds (staged_generation, config_pending; the
 * defect-22 staging report). Those two are accepted and ignored so a
 * consumer that round-trips the GET document -- the native latency renderer,
 * the bench's bypass PUT -- is not refused as UNKNOWN_KEY; they are never
 * required, so TOP_ALL_SEEN covers the nine only. */
enum {
	TOP_SOURCE = 0,
	TOP_CONFIG_GENERATION,
	TOP_SAMPLE_RATE,
	TOP_FEATURES,
	TOP_INPUT_CHANNELS,
	TOP_OUTPUT_EQ,
	TOP_MATRIX,
	TOP_LIMITER,
	TOP_BASE_GENERATION,
	TOP_STAGED_GENERATION,
	TOP_CONFIG_PENDING
};
#define TOP_ALL_SEEN 0x1FFu

static FLASHMEM AiusDspConfigErrorCode parse_top_value(AiusJsonCursor *parser,
	                                              unsigned int field,
	                                              AiusDspModel *model)
{
	switch (field) {
	case TOP_SOURCE:
		return parse_source(parser);
	case TOP_CONFIG_GENERATION: {
		double ignored = 0.0;
		return take_non_negative_integer(parser, &ignored);
	}
	case TOP_SAMPLE_RATE: {
		double rate = 0.0;
		AiusDspConfigErrorCode error = take_number(parser, &rate);
		if (error != AIUS_DSP_CONFIG_OK)
			return error;
		/* Bounded before the cast: a double above UINT32_MAX is UB to cast. */
		if (rate <= 0.0 || rate > 4294967295.0 || floor(rate) != rate)
			return AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE;
		model->sample_rate_hz = (uint32_t)rate;
		return AIUS_DSP_CONFIG_OK;
	}
	case TOP_FEATURES:
		return parse_features(parser, &model->features);
	case TOP_INPUT_CHANNELS:
		return parse_input_channels(parser, model);
	case TOP_OUTPUT_EQ:
		return parse_eq_array(parser, model->output_eq, &model->output_eq_count);
	case TOP_MATRIX:
		return parse_matrix(parser, &model->matrix);
	case TOP_LIMITER:
		return parse_limiter(parser, &model->limiter);
	case TOP_STAGED_GENERATION: {
		double ignored = 0.0;
		return take_non_negative_integer(parser, &ignored);
	}
	case TOP_CONFIG_PENDING: {
		uint8_t ignored = 0u;
		return take_bool(parser, &ignored);
	}
	default: {
		double base = 0.0;
		AiusDspConfigErrorCode error =
			take_non_negative_integer(parser, &base);
		if (error != AIUS_DSP_CONFIG_OK)
			return error;
		if (base > 4294967295.0)
			return AIUS_DSP_CONFIG_ERR_OUT_OF_RANGE;
		model->base_generation = (uint32_t)base;
		return AIUS_DSP_CONFIG_OK;
	}
	}
}

FLASHMEM AiusDspConfigErrorCode aius_dsp_config_parse_put(const uint8_t *json, size_t len,
	                                                AiusDspModel *out)
{
	if (out == NULL)
		return AIUS_DSP_CONFIG_ERR_NULL_ARGUMENT;
	aius_dsp_model_set_defaults(out);
	if (json == NULL)
		return AIUS_DSP_CONFIG_ERR_NULL_ARGUMENT;
	if (len == 0u)
		return AIUS_DSP_CONFIG_ERR_EMPTY;
	if (len > AIUS_DSP_MODEL_MAX_JSON_BYTES)
		return AIUS_DSP_CONFIG_ERR_TOO_LARGE;

	AiusDspModel parsed;
	aius_dsp_model_set_defaults(&parsed);

	AiusJsonCursor parser = {json, len, 0u};
	aius_json_skip_whitespace(&parser);
	if (!aius_json_consume(&parser, '{'))
		return skip_value(&parser) ? AIUS_DSP_CONFIG_ERR_WRONG_TYPE
		                           : AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
	aius_json_skip_whitespace(&parser);

	unsigned int seen = 0u;
	if (!aius_json_consume(&parser, '}')) {
		for (;;) {
			uint8_t key[AIUS_DSP_CONFIG_KEY_BUFFER_MAX];
			size_t key_len = 0;
			int overflow = 0;
			AiusDspConfigErrorCode error =
				take_key(&parser, key, sizeof(key), &key_len, &overflow);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			unsigned int field;
			if (aius_json_key_is(key, key_len, overflow, "source"))
				field = TOP_SOURCE;
			else if (aius_json_key_is(key, key_len, overflow, "config_generation"))
				field = TOP_CONFIG_GENERATION;
			else if (aius_json_key_is(key, key_len, overflow, "sample_rate_hz"))
				field = TOP_SAMPLE_RATE;
			else if (aius_json_key_is(key, key_len, overflow, "features"))
				field = TOP_FEATURES;
			else if (aius_json_key_is(key, key_len, overflow, "input_channels"))
				field = TOP_INPUT_CHANNELS;
			else if (aius_json_key_is(key, key_len, overflow, "output_eq"))
				field = TOP_OUTPUT_EQ;
			else if (aius_json_key_is(key, key_len, overflow, "matrix"))
				field = TOP_MATRIX;
			else if (aius_json_key_is(key, key_len, overflow, "limiter"))
				field = TOP_LIMITER;
			else if (aius_json_key_is(key, key_len, overflow, "base_generation"))
				field = TOP_BASE_GENERATION;
			else if (aius_json_key_is(key, key_len, overflow, "staged_generation"))
				field = TOP_STAGED_GENERATION;
			else if (aius_json_key_is(key, key_len, overflow, "config_pending"))
				field = TOP_CONFIG_PENDING;
			else
				return AIUS_DSP_CONFIG_ERR_UNKNOWN_KEY;

			unsigned int bit = 1u << field;
			if ((seen & bit) != 0u)
				return AIUS_DSP_CONFIG_ERR_DUPLICATE_KEY;
			seen |= bit;

			error = consume_colon(&parser);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;
			error = parse_top_value(&parser, field, &parsed);
			if (error != AIUS_DSP_CONFIG_OK)
				return error;

			AiusDspConfigErrorCode sep = AIUS_DSP_CONFIG_OK;
			if (!object_next(&parser, &sep)) {
				if (sep != AIUS_DSP_CONFIG_OK)
					return sep;
				break;
			}
		}
	}

	aius_json_skip_whitespace(&parser);
	if (!aius_json_at_end(&parser))
		return AIUS_DSP_CONFIG_ERR_MALFORMED_JSON;
	if ((seen & TOP_ALL_SEEN) != TOP_ALL_SEEN)
		return AIUS_DSP_CONFIG_ERR_MISSING_KEY;

	*out = parsed;
	return AIUS_DSP_CONFIG_OK;
}
