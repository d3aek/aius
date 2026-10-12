/* lib/aius_json contract: the one JSON cursor lib/aius_config,
 * lib/aius_dsp_config and src/aius_optical_app.cpp's measure body reader share.
 *
 * The three callers' own suites prove their schemas. This one drives the
 * primitives directly, because a primitive kit's contract is finer than any one
 * caller exercises -- and because the two literal forms and the two string
 * forms exist precisely so the callers can differ, which is the pair of
 * distinctions most at risk of being "simplified" into one.
 *
 * Allocation-freedom is a contract, not an implementation detail (the measure
 * body reader runs in the epoch loop), so the suite links --wrap over the libc
 * allocators and asserts zero allocations across a full sweep of the kit.
 */

#include "aius_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t nmemb, size_t size);
extern void *__real_realloc(void *ptr, size_t size);
extern void __real_free(void *ptr);

static unsigned int allocation_calls;

void *__wrap_malloc(size_t size)
{
	allocation_calls++;
	return __real_malloc(size);
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
	allocation_calls++;
	return __real_calloc(nmemb, size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
	allocation_calls++;
	return __real_realloc(ptr, size);
}

void __wrap_free(void *ptr)
{
	allocation_calls++;
	__real_free(ptr);
}

static unsigned int failures;

static void check(int condition, const char *format, ...)
{
	if (condition)
		return;
	failures++;
	va_list args;
	va_start(args, format);
	fputs("FAIL: ", stderr);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
}

static AiusJsonCursor over(const char *json)
{
	AiusJsonCursor cursor = { (const uint8_t *)json, strlen(json), 0u };
	return cursor;
}

static AiusJsonCursor over_bytes(const char *json, size_t len)
{
	AiusJsonCursor cursor = { (const uint8_t *)json, len, 0u };
	return cursor;
}

/* ---- cursor basics -------------------------------------------------------- */

static void test_at_end_peek_consume(void)
{
	AiusJsonCursor empty = over_bytes("", 0u);
	check(aius_json_at_end(&empty), "empty input is not at end");
	check(aius_json_peek(&empty) == 0u, "peek at end is not 0");

	AiusJsonCursor cursor = over("{}");
	check(!aius_json_at_end(&cursor), "non-empty input reported at end");
	check(aius_json_peek(&cursor) == '{', "peek did not return the first byte");
	check(aius_json_peek(&cursor) == '{', "peek advanced the cursor");

	check(!aius_json_consume(&cursor, '['), "consume matched the wrong byte");
	check(cursor.pos == 0u, "consume advanced on a failed match");
	check(aius_json_consume(&cursor, '{'), "consume rejected the byte present");
	check(cursor.pos == 1u, "consume did not advance exactly one byte");
	check(aius_json_consume(&cursor, '}'), "consume rejected the second byte");
	check(aius_json_at_end(&cursor), "cursor not at end after the last byte");
	check(!aius_json_consume(&cursor, '}'), "consume matched past the end");

	/* A byte the cursor never reads: len, not a terminator, is the bound. */
	AiusJsonCursor bounded = over_bytes("{}[]", 2u);
	check(aius_json_consume(&bounded, '{') && aius_json_consume(&bounded, '}'),
	      "bounded cursor did not read its two bytes");
	check(aius_json_at_end(&bounded), "cursor read past its declared length");
}

static void test_skip_whitespace(void)
{
	AiusJsonCursor cursor = over(" \t\r\n{");
	aius_json_skip_whitespace(&cursor);
	check(cursor.pos == 4u, "skip_whitespace did not skip all four WS bytes");
	check(aius_json_peek(&cursor) == '{', "skip_whitespace overshot");

	aius_json_skip_whitespace(&cursor);
	check(cursor.pos == 4u, "skip_whitespace advanced over a non-WS byte");

	/* RFC 8259 §2 admits exactly those four. These three are not whitespace. */
	AiusJsonCursor form_feed = over(" \f");
	aius_json_skip_whitespace(&form_feed);
	check(form_feed.pos == 1u, "form feed was treated as JSON whitespace");
	AiusJsonCursor vertical_tab = over(" \v");
	aius_json_skip_whitespace(&vertical_tab);
	check(vertical_tab.pos == 1u, "vertical tab was treated as JSON whitespace");
	AiusJsonCursor embedded_nul = over_bytes(" \0", 2u);
	aius_json_skip_whitespace(&embedded_nul);
	check(embedded_nul.pos == 1u, "NUL was treated as JSON whitespace");

	AiusJsonCursor all_whitespace = over("   ");
	aius_json_skip_whitespace(&all_whitespace);
	check(aius_json_at_end(&all_whitespace),
	      "skip_whitespace stopped short of the end");
}

/* ---- strings ------------------------------------------------------------- */

static void expect_string(const char *json, const char *decoded,
	                          size_t decoded_len, const char *label)
{
	uint8_t buffer[16];
	size_t length = 0;
	int overflow = -1;
	AiusJsonCursor cursor = over(json);
	memset(buffer, 0xa5, sizeof(buffer));
	check(aius_json_take_string(&cursor, buffer, sizeof(buffer), &length,
	                           &overflow) != 0,
	      "%s: rejected a well-formed string", label);
	check(length == decoded_len, "%s: decoded length %zu, expected %zu", label,
	      length, decoded_len);
	check(overflow == 0, "%s: overflow reported for a string that fits", label);
	check(memcmp(buffer, decoded, decoded_len) == 0,
	      "%s: decoded bytes differ", label);
	check(buffer[decoded_len] == '\0', "%s: decoded buffer not terminated",
	      label);
	check(aius_json_at_end(&cursor), "%s: cursor not past the closing quote",
	      label);
}

static void expect_string_rejected(const char *json, size_t len,
	                                   const char *label)
{
	uint8_t buffer[16];
	size_t length = 0;
	int overflow = 0;
	AiusJsonCursor cursor = over_bytes(json, len);
	check(aius_json_take_string(&cursor, buffer, sizeof(buffer), &length,
	                           &overflow) == 0,
	      "%s: accepted a malformed string", label);
}

static void test_take_string(void)
{
	expect_string("\"\"", "", 0u, "empty string");
	expect_string("\"config\"", "config", 6u, "plain key");
	expect_string("\"a b\"", "a b", 3u, "embedded space");
	expect_string("\"\\\"\\\\\\/\"", "\"\\/", 3u, "quote, backslash, solidus");
	expect_string("\"\\b\\f\\n\\r\\t\"", "\b\f\n\r\t", 5u, "control escapes");
	expect_string("\"\\u0041\"", "A", 1u, "ASCII \\u escape");
	/* Above U+007F decodes to one unmatchable byte rather than widening any
	 * buffer -- only ASCII schema names can ever match. */
	expect_string("\"\\u00e9\"", "\xff", 1u, "non-ASCII \\u escape");
	expect_string("\"\\u0000\"", "\x00", 1u, "\\u0000 escape");

	expect_string_rejected("\"abc", 4u, "unterminated string");
	expect_string_rejected("abc\"", 4u, "no opening quote");
	expect_string_rejected("\"a\nb\"", 5u, "raw newline in string");
	expect_string_rejected("\"a\x01\"", 4u, "raw control byte in string");
	expect_string_rejected("\"a\\x\"", 5u, "unknown escape");
	expect_string_rejected("\"a\\", 3u, "escape at end of input");
	expect_string_rejected("\"\\u00\"", 6u, "truncated \\u escape");
	expect_string_rejected("\"\\u00g1\"", 8u, "non-hex \\u escape");

	/* Hex digits in all three cases the decoder accepts. */
	uint8_t buffer[16];
	size_t length = 0;
	int overflow = 0;
	AiusJsonCursor mixed_hex = over("\"\\u004a\\u004B\"");
	check(aius_json_take_string(&mixed_hex, buffer, sizeof(buffer), &length,
	                           &overflow) != 0,
	      "mixed-case hex escape rejected");
	check(length == 2u && buffer[0] == 'J' && buffer[1] == 'K',
	      "mixed-case hex escape decoded wrongly");

	/* Validate and discard: no buffer at all. */
	AiusJsonCursor discard = over("\"anything at all\", rest");
	check(aius_json_take_string(&discard, NULL, 0u, NULL, NULL) != 0,
	      "validate-and-discard rejected a well-formed string");
	check(discard.pos == 17u, "validate-and-discard left the wrong position");

	/* Overflow: fully consumed, reported, and never equal to a schema key. */
	uint8_t small[4];
	AiusJsonCursor long_key = over("\"abcdefgh\":1");
	length = 0;
	overflow = 0;
	memset(small, 0xa5, sizeof(small));
	check(aius_json_take_string(&long_key, small, sizeof(small), &length,
	                           &overflow) != 0,
	      "an over-long string was rejected rather than flagged");
	check(overflow == 1, "an over-long string did not set overflow");
	check(length == 8u, "an over-long string reported the truncated length");
	check(long_key.pos == 10u, "an over-long string was not fully consumed");
	check(!aius_json_key_is(small, length, overflow, "abcdefgh"),
	      "an overflowed key matched its own text");

	/* The largest string that still fits dst_cap keeps its terminator. */
	AiusJsonCursor exact = over("\"abc\"");
	length = 0;
	overflow = 1;
	check(aius_json_take_string(&exact, small, sizeof(small), &length,
	                           &overflow) != 0,
	      "an exactly-fitting string was rejected");
	check(overflow == 0 && length == 3u && strcmp((const char *)small, "abc") == 0,
	      "an exactly-fitting string was flagged or truncated");
}

static void test_key_is(void)
{
	const uint8_t *key = (const uint8_t *)"features";
	check(aius_json_key_is(key, 8u, 0, "features"), "an exact key did not match");
	check(!aius_json_key_is(key, 8u, 0, "feature"),
	      "a shorter expectation matched");
	check(!aius_json_key_is(key, 7u, 0, "features"),
	      "a prefix of the key matched the full name");
	check(!aius_json_key_is(key, 8u, 1, "features"),
	      "an overflowed key matched");
	check(aius_json_key_is((const uint8_t *)"", 0u, 0, ""),
	      "the empty key did not match the empty name");
}

/* ---- literals ------------------------------------------------------------ */

static void test_match_and_literal(void)
{
	/* Both forms accept a bare literal and consume exactly its bytes. */
	AiusJsonCursor matched = over("true");
	check(aius_json_match(&matched, "true") && matched.pos == 4u,
	      "match rejected or mis-advanced over a bare literal");
	AiusJsonCursor literal = over("true");
	check(aius_json_literal(&literal, "true") && literal.pos == 4u,
	      "literal rejected or mis-advanced over a bare literal");

	/* The distinction the two callers need. `truex` is a literal followed by
	 * junk: the config parsers reject it here, on the value boundary; the
	 * measure body reader lets its object grammar reject it at the next
	 * structural character, so its match must succeed. */
	AiusJsonCursor trailing_match = over("truex");
	check(aius_json_match(&trailing_match, "true"),
	      "match refused a literal with a trailing byte");
	check(trailing_match.pos == 4u, "match did not stop after the literal");
	AiusJsonCursor trailing_literal = over("truex");
	check(!aius_json_literal(&trailing_literal, "true"),
	      "literal accepted a literal with a trailing byte");
	check(trailing_literal.pos == 0u, "literal advanced on a failed match");

	/* Every terminator the value grammar admits after a literal: a structural
	 * character, the end of input, or whitespace followed by either. The cursor
	 * stops on the first of them in every case -- it reads the terminator, it
	 * never consumes it. */
	static const char *const boundaries[] = {
		"true", "true,", "true}", "true]",
		"true ", "true\t", "true\r", "true\n",
		"true ,", "true \t}", "true\r\n]"
	};
	for (unsigned int i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
		AiusJsonCursor cursor = over(boundaries[i]);
		check(aius_json_literal(&cursor, "true"),
		      "literal rejected the value terminator in %s", boundaries[i]);
		check(cursor.pos == 4u, "literal consumed the terminator of %s",
		      boundaries[i]);
	}

	/* Whitespace is not itself a terminator: it may only precede one. `true x`
	 * is two values with no separator, which no grammar can place -- the same
	 * rule scan_number applies to `1 2` below. */
	static const char *const unterminated[] = {
		"true x", "true \"a\"", "true true", "true 1", "true :", "true {"
	};
	for (unsigned int i = 0;
	     i < sizeof(unterminated) / sizeof(unterminated[0]); i++) {
		AiusJsonCursor cursor = over(unterminated[i]);
		check(!aius_json_literal(&cursor, "true"),
		      "literal accepted the unterminated value in %s", unterminated[i]);
		check(cursor.pos == 0u, "literal advanced on %s", unterminated[i]);
	}

	/* Neither form advances on a failure, and neither reads past len. */
	AiusJsonCursor mismatch = over("tru");
	check(!aius_json_match(&mismatch, "true") && mismatch.pos == 0u,
	      "match accepted or advanced over a truncated literal");
	check(!aius_json_literal(&mismatch, "true") && mismatch.pos == 0u,
	      "literal accepted or advanced over a truncated literal");
	AiusJsonCursor short_buffer = over_bytes("true", 3u);
	check(!aius_json_match(&short_buffer, "true"),
	      "match read past the declared length");
	check(!aius_json_literal(&short_buffer, "true"),
	      "literal read past the declared length");

	AiusJsonCursor wrong = over("false");
	check(!aius_json_literal(&wrong, "true") && wrong.pos == 0u,
	      "literal matched the wrong text");
	check(aius_json_literal(&wrong, "false") && wrong.pos == 5u,
	      "literal rejected false");
	AiusJsonCursor null_literal = over("null");
	check(aius_json_literal(&null_literal, "null"), "literal rejected null");
}

/* ---- numbers ------------------------------------------------------------- */

static void expect_number(const char *json, size_t expected_end,
	                          const char *label)
{
	size_t start = 99u;
	size_t end = 99u;
	AiusJsonCursor cursor = over(json);
	check(aius_json_scan_number(&cursor, &start, &end) != 0,
	      "%s: rejected a well-formed number", label);
	check(start == 0u, "%s: start is %zu, expected 0", label, start);
	check(end == expected_end, "%s: end is %zu, expected %zu", label, end,
	      expected_end);
	check(cursor.pos == expected_end,
	      "%s: cursor stopped at %zu, expected %zu", label, cursor.pos,
	      expected_end);
}

static void expect_number_rejected(const char *json, const char *label)
{
	size_t start = 0;
	size_t end = 0;
	AiusJsonCursor cursor = over(json);
	check(aius_json_scan_number(&cursor, &start, &end) == 0,
	      "%s: accepted a malformed number", label);
}

static void test_scan_number(void)
{
	expect_number("0", 1u, "zero");
	expect_number("-0", 2u, "negative zero");
	expect_number("48000", 5u, "integer");
	expect_number("-24", 3u, "negative integer");
	expect_number("0.5", 3u, "fraction below one");
	expect_number("-1.25", 5u, "negative fraction");
	expect_number("1e3", 3u, "bare exponent");
	expect_number("1E3", 3u, "upper-case exponent");
	expect_number("1e+3", 4u, "signed positive exponent");
	expect_number("1e-3", 4u, "signed negative exponent");
	expect_number("-0.5e-12", 8u, "every part at once");

	/* The range stops at the value terminator, and so does the cursor -- the
	 * terminator is read, never consumed, so trailing whitespace is left for the
	 * caller's own skip_whitespace(). */
	expect_number("1.5,", 3u, "number before a comma");
	expect_number("2}", 1u, "number before a closing brace");
	expect_number("3]", 1u, "number before a closing bracket");
	expect_number("4 ", 1u, "number before a space");
	expect_number("5 ,", 1u, "number before a spaced comma");
	expect_number("6\r\n}", 1u, "number before a newline and a brace");

	expect_number_rejected("01", "leading zero");
	expect_number_rejected("00", "double zero");
	expect_number_rejected("-01", "negative leading zero");
	expect_number_rejected("1.", "trailing decimal point");
	expect_number_rejected(".5", "no integer part");
	expect_number_rejected("+1", "explicit plus sign");
	expect_number_rejected("-", "lone minus sign");
	expect_number_rejected("1e", "exponent with no digits");
	expect_number_rejected("1e+", "signed exponent with no digits");
	expect_number_rejected("1.e3", "empty fraction before an exponent");
	expect_number_rejected("1x", "number not at a value boundary");
	/* Whitespace may precede a terminator but is not one: a second value after
	 * it has nowhere to go in any caller's grammar. */
	expect_number_rejected("1 2", "two numbers, no separator");
	expect_number_rejected("1 x", "junk after a spaced number");
	expect_number_rejected("1 \"a\"", "a string after a spaced number");
	expect_number_rejected("nan", "not a number at all");
}

/* ---- skip-unknown -------------------------------------------------------- */

static void expect_skipped(const char *json, unsigned int max_depth,
	                           const char *label)
{
	AiusJsonCursor cursor = over(json);
	check(aius_json_skip_value(&cursor, 0u, max_depth) != 0,
	      "%s: refused to skip a well-formed value", label);
	check(aius_json_at_end(&cursor), "%s: skipped only part of the value",
	      label);
}

static void expect_not_skipped(const char *json, unsigned int max_depth,
	                               const char *label)
{
	AiusJsonCursor cursor = over(json);
	check(aius_json_skip_value(&cursor, 0u, max_depth) == 0,
	      "%s: skipped a malformed value", label);
}

/* Skipping consumes exactly one value and stops. What follows is the caller's
 * grammar to judge, not this call's, so the assertion is on where the cursor
 * came to rest rather than on reaching the end of input. */
static void expect_skipped_leaving(const char *json, unsigned int max_depth,
	                                   size_t expected_pos, const char *label)
{
	AiusJsonCursor cursor = over(json);
	check(aius_json_skip_value(&cursor, 0u, max_depth) != 0,
	      "%s: refused to skip a well-formed value", label);
	check(cursor.pos == expected_pos, "%s: stopped at %zu, expected %zu", label,
	      cursor.pos, expected_pos);
}

static void test_skip_value(void)
{
	expect_skipped("null", 4u, "null");
	expect_skipped("true", 4u, "true");
	expect_skipped("false", 4u, "false");
	expect_skipped("-1.5e3", 4u, "number");
	expect_skipped("\"text\"", 4u, "string");
	expect_skipped("{}", 4u, "empty object");
	expect_skipped("[]", 4u, "empty array");
	expect_skipped("{ \"a\" : 1 , \"b\" : [ 1 , 2 ] }", 4u, "spaced object");
	expect_skipped("[{\"a\":[null,{\"b\":true}]}]", 4u, "nested mixture");

	expect_not_skipped("", 4u, "empty input");
	expect_not_skipped("nullx", 4u, "literal with trailing junk");
	expect_not_skipped("undefined", 4u, "unknown literal");
	expect_not_skipped("{", 4u, "unterminated object");
	expect_not_skipped("[", 4u, "unterminated array");
	expect_not_skipped("{\"a\":1,}", 4u, "trailing comma in an object");
	expect_not_skipped("[1,]", 4u, "trailing comma in an array");
	expect_not_skipped("{\"a\"1}", 4u, "missing colon");
	expect_not_skipped("{a:1}", 4u, "unquoted key");
	expect_not_skipped("[1 2]", 4u, "missing comma in an array");
	expect_not_skipped("{\"a\":1 \"b\":2}", 4u, "missing comma in an object");

	/* One value, then stop. A `,` after a value is not junk: it is what every
	 * caller of this function sees, because all three call it for a key whose
	 * value they do not model, in the middle of an object that continues. This
	 * call has no business consuming it and no business refusing it -- and the
	 * whole-object case below is the one that proves that is the right split. */
	expect_skipped_leaving("[1],", 4u, 3u, "array, then the caller's comma");
	expect_skipped_leaving("1,", 4u, 1u, "number, then the caller's comma");
	expect_skipped_leaving("true}", 4u, 4u, "literal, then the caller's brace");
	expect_skipped_leaving("\"s\"]", 4u, 3u, "string, then the caller's bracket");
	expect_skipped_leaving("{} , ", 4u, 2u, "object, then a spaced comma");
	expect_skipped("{\"unknown\":[1],\"known\":2}", 4u,
	               "an unmodelled value mid-object, which is the real caller");

	/* The cap is the caller's, and it bounds the only recursion here. A value
	 * at the cap's own depth is admitted; one level deeper is refused. */
	expect_skipped("{}", 0u, "flat object at depth cap 0");
	expect_not_skipped("{\"a\":{}}", 0u, "nested object beyond depth cap 0");
	expect_skipped("{\"a\":{}}", 1u, "nested object at depth cap 1");
	expect_not_skipped("{\"a\":{\"b\":{}}}", 1u, "two levels beyond depth cap 1");
	expect_skipped("[[]]", 1u, "nested array at depth cap 1");
	expect_not_skipped("[[[]]]", 1u, "two levels of array beyond depth cap 1");
	/* A scalar is never nesting, however deep the cap. */
	expect_skipped("1", 0u, "scalar at depth cap 0");
}

/* ---- allocation-freedom -------------------------------------------------- */

static void test_allocation_free(void)
{
	/* One sweep of the whole kit with no I/O in it, so any allocation the
	 * library makes is the only thing the counter can see. */
	unsigned int before = allocation_calls;
	uint8_t buffer[16];
	size_t length = 0;
	size_t start = 0;
	size_t end = 0;
	int overflow = 0;
	int accepted;
	AiusJsonCursor cursor =
		over(" { \"key\" : \"value\" , \"n\" : -1.5e3 , \"t\" : true , "
		     "\"skip\" : [ { } , null ] } ");

	aius_json_skip_whitespace(&cursor);
	accepted = aius_json_consume(&cursor, '{');
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_take_string(&cursor, buffer, sizeof(buffer), &length,
	                                  &overflow);
	accepted &= aius_json_key_is(buffer, length, overflow, "key");
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_consume(&cursor, ':');
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_take_string(&cursor, NULL, 0u, NULL, NULL);
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_consume(&cursor, ',');
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_take_string(&cursor, buffer, sizeof(buffer), &length,
	                                  &overflow);
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_consume(&cursor, ':');
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_scan_number(&cursor, &start, &end);
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_consume(&cursor, ',');
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_take_string(&cursor, buffer, sizeof(buffer), &length,
	                                  &overflow);
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_consume(&cursor, ':');
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_literal(&cursor, "true");
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_consume(&cursor, ',');
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_take_string(&cursor, buffer, sizeof(buffer), &length,
	                                  &overflow);
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_consume(&cursor, ':');
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_skip_value(&cursor, 0u, 4u);
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_consume(&cursor, '}');
	aius_json_skip_whitespace(&cursor);
	accepted &= aius_json_at_end(&cursor);
	unsigned int after = allocation_calls;

	check(accepted != 0, "the allocation sweep did not parse its own document");
	check(after == before, "the cursor allocated %u time(s)", after - before);
}

int main(void)
{
	test_at_end_peek_consume();
	test_skip_whitespace();
	test_take_string();
	test_key_is();
	test_match_and_literal();
	test_scan_number();
	test_skip_value();
	test_allocation_free();
	if (failures != 0u) {
		fprintf(stderr, "RESULT: %u check(s) failed\n", failures);
		return 1;
	}
	puts("ok aius json cursor (bounded, strict, no heap)");
	return 0;
}
