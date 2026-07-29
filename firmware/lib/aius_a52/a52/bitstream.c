/*
 * bitstream.c
 * Copyright (C) 2000-2002 Michel Lespinasse <walken@zoy.org>
 * Copyright (C) 1999-2000 Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 *
 * This file is part of a52dec, a free ATSC A-52 stream decoder.
 * See http://liba52.sourceforge.net/ for updates.
 *
 * a52dec is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * a52dec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include <inttypes.h>
#include <stddef.h>

#include "a52.h"
#include "a52_internal.h"
#include "bitstream.h"

#define BUFFER_SIZE 4096

void a52_bitstream_set_end (a52_state_t * state, const uint8_t * end)
{
    if (state != NULL)
	state->buffer_end = end;
}

#ifdef AIUS_A52_BOUND_PROBE
void a52_bitstream_probe_expect_end (a52_state_t * state,
				     const uint8_t * frame,
				     uint32_t frame_length)
{
    if (state != NULL)
	state->buffer_expected_end = frame + frame_length;
}

void a52_bitstream_get_probe (const a52_state_t * state,
			      uint32_t * fills,
			      uint32_t * suppressed_fills,
			      uint32_t * end_mismatches)
{
    if (state == NULL)
	return;
    if (fills != NULL)
	*fills = state->bitstream_fills;
    if (suppressed_fills != NULL)
	*suppressed_fills = state->bitstream_suppressed_fills;
    if (end_mismatches != NULL)
	*end_mismatches = state->bitstream_end_mismatches;
}
#endif

void a52_bitstream_set_ptr (a52_state_t * state, uint8_t * buf)
{
    int align;

    align = (long)buf & 3;
    state->buffer_start = (uint32_t *) (buf - align);
    state->bits_left = 0;
    bitstream_get (state, align * 8);
}

static inline void bitstream_fill_current (a52_state_t * state)
{
    uint32_t tmp;

#ifdef AIUS_A52_BOUND_PROBE
    state->bitstream_fills++;
    if (state->buffer_end == NULL ||
	state->buffer_end != state->buffer_expected_end)
	state->bitstream_end_mismatches++;
#endif

    /* The adapter accepts only 48 kHz frames, whose byte length and aligned
     * scratch base make every reachable word start four-byte aligned. The
     * integer comparison also makes an unset NULL end fail closed without an
     * undefined relational comparison between unrelated pointers. */
    if ((uintptr_t)state->buffer_start >= (uintptr_t)state->buffer_end) {
	state->current_word = 0;
#ifdef AIUS_A52_BOUND_PROBE
	state->bitstream_suppressed_fills++;
#endif
	return;
    }

    tmp = *(state->buffer_start++);
    state->current_word = swab32 (tmp);
}

/*
 * The fast paths for _get is in the
 * bitstream.h header file so it can be inlined.
 *
 * The "bottom half" of this routine is suffixed _bh
 *
 * -ah
 */

uint32_t a52_bitstream_get_bh (a52_state_t * state, uint32_t num_bits)
{
    uint32_t result;

    num_bits -= state->bits_left;
    result = state->bits_left
	? (state->current_word << (32 - state->bits_left)) >>
	  (32 - state->bits_left)
	: 0;

    bitstream_fill_current (state);

    if (num_bits != 0)
	result = (result << num_bits) | (state->current_word >> (32 - num_bits));

    state->bits_left = 32 - num_bits;

    return result;
}

int32_t a52_bitstream_get_bh_2 (a52_state_t * state, uint32_t num_bits)
{
    int32_t result;

    num_bits -= state->bits_left;
    result = state->bits_left
	? (int32_t)(state->current_word << (32 - state->bits_left)) >>
	  (32 - state->bits_left)
	: 0;

    bitstream_fill_current(state);

    if (num_bits != 0)
	result = (int32_t)(((uint32_t)result << num_bits) |
			   (state->current_word >> (32 - num_bits)));
	
    state->bits_left = 32 - num_bits;

    return result;
}
