#include "aius_json.h"

#include <string.h>

/* FLASHMEM is Teensyduino's __attribute__((section(".flashmem"))), defined in
 * <avr/pgmspace.h> -- pull in the real definition when building for the target
 * (TEENSYDUINO is a compiler -D flag set project-wide, independent of any
 * #include, so checking it here is reliable). This file otherwise never
 * includes any Teensy/Arduino header, so plain gcc for the native suites falls
 * through to the no-op fallback (mirroring lib/aius_dsp_config's shim).
 *
 * Every JSON parse in this firmware is cold -- an HTTP request body, or an SD
 * config (re)load -- and never runs on the audio path, so every definition
 * below is FLASHMEM (FLASH-resident code, not the tight shared FlexRAM/ITCM
 * pool the audio pipeline needs). GCC does not merge the `section` attribute
 * from a declaration onto a later definition that omits it, so FLASHMEM is
 * applied directly on each definition here rather than in the header. */
#if defined(TEENSYDUINO)
#include <avr/pgmspace.h>
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif

FLASHMEM int aius_json_at_end(const AiusJsonCursor *cursor)
{
	/* >= rather than ==: no function here can move pos past len, but a cursor
	 * is a caller-built struct and an out-of-range pos must not read. */
	return cursor->pos >= cursor->len;
}

FLASHMEM uint8_t aius_json_peek(const AiusJsonCursor *cursor)
{
	if (aius_json_at_end(cursor))
		return 0u;
	return cursor->data[cursor->pos];
}

/* RFC 8259 section 2 insignificant whitespace, and nothing else. */
static FLASHMEM int is_whitespace(uint8_t ch)
{
	return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

FLASHMEM void aius_json_skip_whitespace(AiusJsonCursor *cursor)
{
	while (!aius_json_at_end(cursor)) {
		if (!is_whitespace(cursor->data[cursor->pos]))
			return;
		cursor->pos++;
	}
}

FLASHMEM int aius_json_consume(AiusJsonCursor *cursor, uint8_t expected)
{
	if (aius_json_peek(cursor) != expected)
		return 0;
	cursor->pos++;
	return 1;
}

static FLASHMEM int hex_digit(uint8_t ch, uint16_t *value)
{
	if (ch >= '0' && ch <= '9') {
		*value = (uint16_t)(ch - '0');
		return 1;
	}
	if (ch >= 'a' && ch <= 'f') {
		*value = (uint16_t)(ch - 'a' + 10u);
		return 1;
	}
	if (ch >= 'A' && ch <= 'F') {
		*value = (uint16_t)(ch - 'A' + 10u);
		return 1;
	}
	return 0;
}

FLASHMEM int aius_json_take_string(AiusJsonCursor *cursor, uint8_t *dst,
	                                  size_t dst_cap, size_t *length,
	                                  int *overflow)
{
	size_t written = 0;
	size_t decoded_len = 0;
	int too_long = 0;

	if (!aius_json_consume(cursor, '"'))
		return 0;
	while (!aius_json_at_end(cursor)) {
		uint8_t ch = cursor->data[cursor->pos++];
		if (ch == '"') {
			if (length != NULL)
				*length = decoded_len;
			if (overflow != NULL)
				*overflow = too_long;
			if (dst != NULL && dst_cap != 0u && written < dst_cap)
				dst[written] = '\0';
			return 1;
		}
		if (ch < 0x20u)
			return 0;
		if (ch != '\\') {
			decoded_len++;
			if (dst != NULL) {
				if (written + 1u < dst_cap)
					dst[written++] = ch;
				else
					too_long = 1;
			}
			continue;
		}

		if (aius_json_at_end(cursor))
			return 0;
		ch = cursor->data[cursor->pos++];
		if (ch == 'u') {
			uint16_t codepoint = 0;
			for (unsigned int i = 0; i < 4u; i++) {
				uint16_t digit;
				if (aius_json_at_end(cursor) ||
				    !hex_digit(cursor->data[cursor->pos++], &digit))
					return 0;
				codepoint = (uint16_t)((codepoint << 4) | digit);
			}
			decoded_len++;
			/* Only ASCII schema names can match. Preserve non-ASCII as a
			 * single impossible-to-match byte without widening the buffer. */
			uint8_t decoded = codepoint <= 0x7fu ? (uint8_t)codepoint : 0xffu;
			if (dst != NULL) {
				if (written + 1u < dst_cap)
					dst[written++] = decoded;
				else
					too_long = 1;
			}
			continue;
		}
		if (ch != '"' && ch != '\\' && ch != '/' && ch != 'b' &&
		    ch != 'f' && ch != 'n' && ch != 'r' && ch != 't')
			return 0;
		decoded_len++;
		if (dst != NULL) {
			uint8_t decoded = ch;
			switch (ch) {
			case 'b': decoded = '\b'; break;
			case 'f': decoded = '\f'; break;
			case 'n': decoded = '\n'; break;
			case 'r': decoded = '\r'; break;
			case 't': decoded = '\t'; break;
			default: break;
			}
			if (written + 1u < dst_cap)
				dst[written++] = decoded;
			else
				too_long = 1;
		}
	}
	return 0;
}

FLASHMEM int aius_json_key_is(const uint8_t *key, size_t len, int overflow,
	                             const char *expected)
{
	return !overflow && strlen(expected) == len &&
	       memcmp(key, expected, len) == 0;
}

/* The only three bytes that may follow a complete JSON value: RFC 8259's
 * structural characters that end one (`,` between members or elements, `}`, and
 * `]`). `:` and `{`/`[` open a value rather than closing one, so neither can
 * follow one. */
static FLASHMEM int structural_boundary(uint8_t ch)
{
	return ch == ',' || ch == '}' || ch == ']';
}

/* Whether a value whose last byte ends at `pos` is properly terminated:
 * insignificant whitespace may follow it, but only a structural character or
 * the end of input may follow that whitespace. Whitespace alone is NOT a
 * terminator -- `1 2` is two values with no separator between them, and no
 * caller's grammar has anywhere to put the second one, so admitting it here
 * would push the rejection into whichever caller happened to look next (and
 * out of aius_json_skip_value(), which has no next).
 *
 * The scan reads ahead without moving the cursor: `4 ` is a complete number
 * whose cursor must stop on the space, leaving the caller's own
 * aius_json_skip_whitespace() the one thing that consumes it. */
static FLASHMEM int value_terminated(const AiusJsonCursor *cursor, size_t pos)
{
	while (pos < cursor->len && is_whitespace(cursor->data[pos]))
		pos++;
	return pos >= cursor->len || structural_boundary(cursor->data[pos]);
}

/* Byte compare at the cursor without advancing, so both public forms can decide
 * to reject before any position changes. */
static FLASHMEM int matches_at(const AiusJsonCursor *cursor, const char *text,
	                              size_t text_len)
{
	return cursor->pos + text_len <= cursor->len &&
	       memcmp(cursor->data + cursor->pos, text, text_len) == 0;
}

FLASHMEM int aius_json_match(AiusJsonCursor *cursor, const char *text)
{
	size_t text_len = strlen(text);
	if (!matches_at(cursor, text, text_len))
		return 0;
	cursor->pos += text_len;
	return 1;
}

FLASHMEM int aius_json_literal(AiusJsonCursor *cursor, const char *text)
{
	size_t text_len = strlen(text);
	if (!matches_at(cursor, text, text_len))
		return 0;
	if (!value_terminated(cursor, cursor->pos + text_len))
		return 0;
	cursor->pos += text_len;
	return 1;
}

FLASHMEM int aius_json_scan_number(AiusJsonCursor *cursor, size_t *start,
	                                  size_t *end)
{
	*start = cursor->pos;
	if (aius_json_peek(cursor) == '-') {
		cursor->pos++;
		if (aius_json_at_end(cursor))
			return 0;
	}
	if (aius_json_peek(cursor) == '0') {
		cursor->pos++;
		if (!aius_json_at_end(cursor) && cursor->data[cursor->pos] >= '0' &&
		    cursor->data[cursor->pos] <= '9')
			return 0;
	} else {
		if (aius_json_peek(cursor) < '1' || aius_json_peek(cursor) > '9')
			return 0;
		do {
			cursor->pos++;
		} while (!aius_json_at_end(cursor) && cursor->data[cursor->pos] >= '0' &&
		         cursor->data[cursor->pos] <= '9');
	}
	if (aius_json_peek(cursor) == '.') {
		cursor->pos++;
		if (aius_json_at_end(cursor) || aius_json_peek(cursor) < '0' ||
		    aius_json_peek(cursor) > '9')
			return 0;
		do {
			cursor->pos++;
		} while (!aius_json_at_end(cursor) && cursor->data[cursor->pos] >= '0' &&
		         cursor->data[cursor->pos] <= '9');
	}
	if (aius_json_peek(cursor) == 'e' || aius_json_peek(cursor) == 'E') {
		cursor->pos++;
		if (aius_json_peek(cursor) == '+' || aius_json_peek(cursor) == '-')
			cursor->pos++;
		if (aius_json_at_end(cursor) || aius_json_peek(cursor) < '0' ||
		    aius_json_peek(cursor) > '9')
			return 0;
		do {
			cursor->pos++;
		} while (!aius_json_at_end(cursor) && cursor->data[cursor->pos] >= '0' &&
		         cursor->data[cursor->pos] <= '9');
	}
	*end = cursor->pos;
	return value_terminated(cursor, cursor->pos);
}

/* ---- Skip-unknown trio (validates but discards) -------------------------- */

static FLASHMEM int skip_object(AiusJsonCursor *cursor, unsigned int depth,
	                               unsigned int max_depth)
{
	if (depth > max_depth || !aius_json_consume(cursor, '{'))
		return 0;
	aius_json_skip_whitespace(cursor);
	if (aius_json_consume(cursor, '}'))
		return 1;
	for (;;) {
		if (!aius_json_take_string(cursor, NULL, 0u, NULL, NULL))
			return 0;
		aius_json_skip_whitespace(cursor);
		if (!aius_json_consume(cursor, ':'))
			return 0;
		aius_json_skip_whitespace(cursor);
		if (!aius_json_skip_value(cursor, depth + 1u, max_depth))
			return 0;
		aius_json_skip_whitespace(cursor);
		if (aius_json_consume(cursor, '}'))
			return 1;
		if (!aius_json_consume(cursor, ','))
			return 0;
		aius_json_skip_whitespace(cursor);
		if (aius_json_peek(cursor) == '}')
			return 0;
	}
}

static FLASHMEM int skip_array(AiusJsonCursor *cursor, unsigned int depth,
	                              unsigned int max_depth)
{
	if (depth > max_depth || !aius_json_consume(cursor, '['))
		return 0;
	aius_json_skip_whitespace(cursor);
	if (aius_json_consume(cursor, ']'))
		return 1;
	for (;;) {
		if (!aius_json_skip_value(cursor, depth + 1u, max_depth))
			return 0;
		aius_json_skip_whitespace(cursor);
		if (aius_json_consume(cursor, ']'))
			return 1;
		if (!aius_json_consume(cursor, ','))
			return 0;
		aius_json_skip_whitespace(cursor);
		if (aius_json_peek(cursor) == ']')
			return 0;
	}
}

FLASHMEM int aius_json_skip_value(AiusJsonCursor *cursor, unsigned int depth,
	                                 unsigned int max_depth)
{
	switch (aius_json_peek(cursor)) {
	case '"':
		return aius_json_take_string(cursor, NULL, 0u, NULL, NULL);
	case '{':
		return skip_object(cursor, depth, max_depth);
	case '[':
		return skip_array(cursor, depth, max_depth);
	case 'n':
		return aius_json_literal(cursor, "null");
	case 't':
		return aius_json_literal(cursor, "true");
	case 'f':
		return aius_json_literal(cursor, "false");
	case '-':
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9': {
		size_t start, end;
		return aius_json_scan_number(cursor, &start, &end);
	}
	default:
		return 0;
	}
}
