#ifndef AIUS_JSON_H
#define AIUS_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One JSON read cursor for every schema parser in this firmware.
 *
 * Three hand-rolled copies of these primitives existed before this library:
 * lib/aius_config, lib/aius_dsp_config (whose own comment read "Cursor
 * primitives (mirror aius_config.c)") and src/aius_optical_app.cpp's
 * replay-body reader. This is that seam, hoisted once.
 *
 * Properties every caller depends on and none may lose:
 *   - Allocation-free. The cursor is three words over a caller-owned buffer;
 *     nothing here allocates, and nothing retains the input past the call.
 *   - Bounded by `len`, never by a terminator. The HTTP transport buffer is not
 *     NUL-terminated, so no function here calls strlen() on the document. (The
 *     `text`/`expected` arguments are compile-time schema literals; strlen on
 *     those is fine and constant-folded.)
 *   - Recursion only in aius_json_skip_value(), and only to the caller's
 *     max_depth. Every other function is a flat left-to-right walk.
 *   - Cold code. Every definition carries FLASHMEM, so this library never
 *     competes with the audio pipeline for ITCM.
 *
 * This is a primitive kit, not a parser. Schema, key sets, required-key masks,
 * numeric bounds and error vocabularies stay with each caller: they are what
 * the callers differ in, and each library's error enum is its own.
 */
typedef struct {
	const uint8_t *data;
	size_t len;
	size_t pos;
} AiusJsonCursor;

/* Positioned at (or past) the last byte. */
int aius_json_at_end(const AiusJsonCursor *cursor);

/* The byte under the cursor, or 0 at end of input. 0 is never a legal
 * unescaped JSON byte, so callers can compare it without a length check. */
uint8_t aius_json_peek(const AiusJsonCursor *cursor);

/* RFC 8259 section 2 insignificant whitespace, and nothing else: space, tab,
 * LF, CR. A form feed, a vertical tab or an embedded NUL is not JSON
 * whitespace and stays a parse failure. */
void aius_json_skip_whitespace(AiusJsonCursor *cursor);

/* Consume one expected byte. Does NOT skip whitespace: whether whitespace is
 * admitted before a given structural character is the caller's grammar, not
 * this cursor's. (aius_config / aius_dsp_config skip explicitly at each
 * structural point; the replay body reader wraps this to skip first.) */
int aius_json_consume(AiusJsonCursor *cursor, uint8_t expected);

/* Parse a JSON string, decoding escapes into a bounded buffer.
 *
 * `dst` may be NULL with `dst_cap` 0 to validate and discard. *length receives
 * the decoded length; *overflow is set when the decoded string did not fit
 * `dst`, in which case the string is still fully consumed but can never match
 * a schema key. A \uXXXX escape above U+007F decodes to a single 0xFF byte:
 * only ASCII schema names can match, and this keeps a non-ASCII codepoint
 * unmatchable without widening any buffer. Both `length` and `overflow` may be
 * NULL. Returns 0 for an unterminated string, a raw control byte, or a bad
 * escape, having consumed an unspecified amount of input. */
int aius_json_take_string(AiusJsonCursor *cursor, uint8_t *dst, size_t dst_cap,
	                         size_t *length, int *overflow);

/* Compare a parsed string against a schema literal. An overflowed string never
 * matches. */
int aius_json_key_is(const uint8_t *key, size_t len, int overflow,
	                    const char *expected);

/* Match `text` byte-for-byte at the cursor and advance past it, with no regard
 * for what follows. Use when the caller's grammar decides the delimiter --
 * `{"loop":truex}` is refused by the object grammar that demands `,` or `}`
 * next, not by this call. */
int aius_json_match(AiusJsonCursor *cursor, const char *text);

/* Match `text` as a complete JSON value: as aius_json_match, but the match must
 * be followed by a value terminator (below). `truex` and `true x` both fail
 * here. Neither form advances the cursor on failure. */
int aius_json_literal(AiusJsonCursor *cursor, const char *text);

/* Validate a JSON number's grammar and record its [*start, *end) byte range so
 * the caller can convert the text itself. Both outputs are required. Returns 0
 * on a grammar violation or a token not followed by a value terminator.
 *
 * A value terminator, for both this and aius_json_literal, is the end of input
 * or one of `,`, `}`, `]` -- optionally preceded by insignificant whitespace,
 * which the cursor reads past but does not consume. So `4 ` and `4 ,` are
 * complete numbers, both leaving the cursor on the space, while `1 2` is not a
 * number at all: nothing in JSON can follow a value except a separator that
 * closes it, so the rejection belongs here rather than in whichever caller
 * happens to look next. */
int aius_json_scan_number(AiusJsonCursor *cursor, size_t *start, size_t *end);

/* Validate and discard one JSON value of any type, so a caller can consume
 * what it does not model (a wrong-typed value it is about to reject, an
 * unknown key's value).
 *
 * Exactly one value: the cursor stops on the byte after it and the caller's
 * grammar judges that byte. `[1],` skips `[1]` and leaves the cursor on the
 * comma, because every caller invokes this mid-object, where a comma is the
 * legitimate continuation -- consuming or refusing it here would break
 * {"unknown":[1],"known":2}.
 *
 * `depth` is the caller's current nesting level and
 * `max_depth` its cap: this is the only recursive function here, and the cap
 * is what bounds it over untrusted input. Callers set their own cap because
 * their schemas nest to different depths. */
int aius_json_skip_value(AiusJsonCursor *cursor, unsigned int depth,
	                        unsigned int max_depth);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_JSON_H */
