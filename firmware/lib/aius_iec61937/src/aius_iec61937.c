/* AiusIec61937Parser: streaming IEC 61937 data-burst de-framer.
 *
 * Implements aius_iec61937.h as a small word-at-a-time state machine over the
 * S/PDIF subframe payload words. It locks onto the Pa/Pb preamble (auto-detecting
 * wire byte order), reads the Pc/Pd burst header, accumulates the payload words
 * into canonical big-endian bytes (16- or 14-bit packing), and emits an AiusBurst
 * on completion. Corrupt bursts are rejected with a typed status and the machine
 * falls back to searching for the next preamble, so a garbage run re-locks at the
 * next valid burst without a caller-side reset. See lib/aius_iec61937/include.
 */

#include "aius_iec61937.h"

#include <stdlib.h>
#include <string.h>

/* IEC 61937 preamble sync words (16-bit, big-endian wire order). */
#define IEC_PA 0xF872u
#define IEC_PB 0x4E1Fu
/* Pa with its two bytes swapped: the little-endian wire representation. */
#define IEC_PA_SWAPPED 0x72F8u

/* Pc (burst_info) field layout. */
#define PC_DATA_TYPE_MASK 0x001Fu /* bits 0-4: data type */
#define PC_ERROR_FLAG     0x0080u /* bit 7: upstream error flag */

/* Parser lifecycle states. */
enum {
	ST_SEARCH_PA = 0, /* scanning for Pa (also the between-bursts idle state) */
	ST_EXPECT_PB,     /* Pa seen, next word must be Pb */
	ST_EXPECT_PC,     /* reading burst_info */
	ST_EXPECT_PD,     /* reading length-code */
	ST_PAYLOAD        /* accumulating payload words */
};

struct AiusIec61937Parser {
	AiusIec61937Packing packing;

	int state;
	int swap; /* byte order locked at Pa: 1 => swap each word's bytes */

	/* Current-burst header, valid from ST_EXPECT_PD onward. */
	uint16_t data_type;
	int      error_flag;
	int      in_scope;
	uint32_t payload_bytes; /* target de-framed length */
	uint32_t words_needed;  /* payload wire words for this burst */
	uint32_t words_read;    /* payload words consumed so far */

	/* 14-bit unpacking accumulator. */
	uint32_t bitacc;
	int      bitcnt;
	uint32_t bytes_out;

	/* Completed-burst descriptor. */
	int      have_burst;
	uint16_t burst_type;
	uint32_t burst_len;

	uint8_t payload[AIUS_IEC61937_MAX_PAYLOAD];
};

static uint16_t normalize(const AiusIec61937Parser *p, uint16_t w)
{
	return p->swap ? (uint16_t)((w >> 8) | (w << 8)) : w;
}

static void begin_search(AiusIec61937Parser *p)
{
	p->state = ST_SEARCH_PA;
	p->words_read = 0;
	p->bitacc = 0;
	p->bitcnt = 0;
	p->bytes_out = 0;
}

AiusStatus aius_iec61937_parser_create(AiusIec61937Parser **out,
                                       const AiusIec61937Config *cfg)
{
	if (!out)
		return AIUS_INVALID_ARG;
	*out = NULL;

	if (cfg && cfg->packing != AIUS_IEC61937_PACK_16 &&
	    cfg->packing != AIUS_IEC61937_PACK_14)
		return AIUS_INVALID_ARG;

	AiusIec61937Parser *p = calloc(1, sizeof(*p));
	if (!p)
		return AIUS_NO_SPACE;

	p->packing = cfg ? cfg->packing : AIUS_IEC61937_PACK_16;
	begin_search(p);
	p->have_burst = 0;

	*out = p;
	return AIUS_OK;
}

void aius_iec61937_parser_reset(AiusIec61937Parser *p)
{
	if (!p)
		return;
	begin_search(p);
	p->have_burst = 0;
}

void aius_iec61937_parser_destroy(AiusIec61937Parser *p)
{
	free(p);
}

/* Finalize the payload buffer and publish the completed-burst descriptor.
 * Returns AIUS_OK for in-scope (DTS Core) types, AIUS_UNSUPPORTED otherwise. */
static AiusStatus finalize_burst(AiusIec61937Parser *p)
{
	/* Flush a trailing sub-byte group (14-bit packing, non-byte-aligned tail). */
	if (p->packing == AIUS_IEC61937_PACK_14 && p->bytes_out < p->payload_bytes &&
	    p->bitcnt > 0) {
		p->payload[p->bytes_out++] =
			(uint8_t)((p->bitacc << (8 - p->bitcnt)) & 0xFFu);
		p->bitcnt = 0;
	}

	p->burst_type = p->data_type;
	p->burst_len = p->payload_bytes;
	p->have_burst = 1;

	begin_search(p);
	return p->in_scope ? AIUS_OK : AIUS_UNSUPPORTED;
}

AiusStatus aius_iec61937_push(AiusIec61937Parser *p, const uint16_t *words,
                              size_t count, size_t *consumed)
{
	if (!p || !consumed || (!words && count > 0))
		return AIUS_INVALID_ARG;

	size_t i = 0;
	while (i < count) {
		switch (p->state) {
		case ST_SEARCH_PA: {
			uint16_t w = words[i];
			if (w == IEC_PA) {
				p->swap = 0;
				p->state = ST_EXPECT_PB;
			} else if (w == IEC_PA_SWAPPED) {
				p->swap = 1;
				p->state = ST_EXPECT_PB;
			}
			i++; /* a non-Pa word is stuffing/pause/garbage: skip it */
			break;
		}
		case ST_EXPECT_PB: {
			uint16_t w = normalize(p, words[i]);
			if (w == IEC_PB) {
				p->state = ST_EXPECT_PC;
				i++;
			} else {
				/* False sync: re-examine this word as a Pa candidate
				 * without consuming it. i is unchanged, so the next
				 * iteration re-reads words[i] in ST_SEARCH_PA. */
				p->state = ST_SEARCH_PA;
			}
			break;
		}
		case ST_EXPECT_PC: {
			uint16_t pc = normalize(p, words[i]);
			i++;
			p->data_type = (uint16_t)(pc & PC_DATA_TYPE_MASK);
			p->error_flag = (pc & PC_ERROR_FLAG) != 0;
			p->state = ST_EXPECT_PD;
			break;
		}
		case ST_EXPECT_PD: {
			uint16_t pd = normalize(p, words[i]);
			i++;
			uint32_t length_bits = pd;

			if (p->error_flag) {
				begin_search(p);
				*consumed = i;
				return AIUS_MALFORMED;
			}

			p->in_scope = (p->data_type == 11 || p->data_type == 12 ||
			               p->data_type == 13);
			if (p->in_scope && length_bits == 0) {
				begin_search(p);
				*consumed = i;
				return AIUS_MALFORMED;
			}

			p->payload_bytes = (length_bits + 7u) / 8u;
			if (p->payload_bytes > AIUS_IEC61937_MAX_PAYLOAD) {
				begin_search(p);
				*consumed = i;
				return AIUS_NO_SPACE;
			}

			p->words_needed = (p->packing == AIUS_IEC61937_PACK_14)
			                      ? (length_bits + 13u) / 14u
			                      : (length_bits + 15u) / 16u;
			p->words_read = 0;
			p->bytes_out = 0;
			p->bitacc = 0;
			p->bitcnt = 0;
			memset(p->payload, 0, p->payload_bytes);
			p->state = ST_PAYLOAD;

			if (p->words_needed == 0) {
				AiusStatus st = finalize_burst(p);
				*consumed = i;
				return st;
			}
			break;
		}
		case ST_PAYLOAD: {
			uint16_t w = normalize(p, words[i]);
			i++;
			if (p->packing == AIUS_IEC61937_PACK_16) {
				if (p->bytes_out < p->payload_bytes)
					p->payload[p->bytes_out++] = (uint8_t)(w >> 8);
				if (p->bytes_out < p->payload_bytes)
					p->payload[p->bytes_out++] = (uint8_t)(w & 0xFFu);
			} else {
				p->bitacc = (p->bitacc << 14) | (uint32_t)(w & 0x3FFFu);
				p->bitcnt += 14;
				while (p->bitcnt >= 8 && p->bytes_out < p->payload_bytes) {
					p->bitcnt -= 8;
					p->payload[p->bytes_out++] =
						(uint8_t)((p->bitacc >> p->bitcnt) & 0xFFu);
				}
			}
			p->words_read++;
			if (p->words_read >= p->words_needed) {
				AiusStatus st = finalize_burst(p);
				*consumed = i;
				return st;
			}
			break;
		}
		default:
			begin_search(p);
			break;
		}
	}

	*consumed = count;
	return AIUS_NEED_MORE;
}

AiusStatus aius_iec61937_finish(AiusIec61937Parser *p)
{
	if (!p)
		return AIUS_INVALID_ARG;
	int mid_burst = (p->state != ST_SEARCH_PA);
	begin_search(p);
	return mid_burst ? AIUS_TRUNCATED : AIUS_OK;
}

AiusStatus aius_iec61937_current_burst(const AiusIec61937Parser *p, AiusBurst *out)
{
	if (!p || !out || !p->have_burst)
		return AIUS_INVALID_ARG;
	out->data_type = p->burst_type;
	out->payload = p->payload;
	out->length = p->burst_len;
	return AIUS_OK;
}
