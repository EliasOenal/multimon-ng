/*
 *      gen_mpt1327.c -- generate MPT 1327 control channel signalling
 *
 *      Copyright (C) 2026 Ryan Drake
 *
 *      Generates a forward control channel transmission (MPT 1327, June 1997
 *      issue, section numbers below): a start-up sequence, then each given
 *      codeword, with a control channel system codeword (CCSC) in front of
 *      every address codeword after the first, then a final CCSC. A filler
 *      data codeword follows an odd number of data codewords.
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* ---------------------------------------------------------------------- */

#include "gen.h"
#include <string.h>

/* control channel codeword synchronisation sequence (3.2.1.1) */
#define MPT1327_SYNC 0xC4D7u

/* x^15 + x^14 + x^13 + x^11 + x^4 + x^2 + 1 without the x^15 term (3.2.3) */
#define MPT1327_GENERATOR 0x6815u

/* FFSK (3.1): 1200 Hz for 1, 1800 Hz for 0, at 1200 bit/s */
#define PHINC_1 (0x10000u*1200/SAMPLE_RATE)
#define PHINC_0 (0x10000u*1800/SAMPLE_RATE)
#define BITPHINC (0x10000u*1200/SAMPLE_RATE)

/* ---------------------------------------------------------------------- */

/* Remainder of info(x) * x^15 divided by the generator, for the 48 information bits */
static unsigned int remainder15(uint64_t info)
{
	unsigned int r = 0;
	int i;

	for (i = 47; i >= 0; i--) {
		unsigned int feedback = ((unsigned int)(info >> i) ^ (r >> 14)) & 1;
		r = (r << 1) & 0x7FFF;
		if (feedback)
			r ^= MPT1327_GENERATOR;
	}
	return r;
}

/* 64-bit codeword: 48 information bits, 15 check bits with the last one inverted,
 * and even parity (3.2.3) */
static uint64_t codeword(uint64_t info)
{
	uint64_t cw = (info << 15) | (remainder15(info) ^ 1);
	uint64_t v = cw;
	unsigned int parity = 0;

	while (v) {
		parity ^= (unsigned int)(v & 1);
		v >>= 1;
	}
	return (cw << 1) | parity;
}

/* Control channel system codeword (5.1.1), completion sequence by the algorithm of appendix 3 */
static uint64_t ccsc(unsigned int sys)
{
	uint64_t rotated = (0xAAAAULL << 32) | (0xC4D4ULL << 16) | ((uint64_t)sys << 1);
	uint64_t cw = codeword(rotated);
	uint64_t ccs;

	if ((cw & 1) == 0)
		cw = codeword(rotated | 1);
	ccs = ((cw >> 1) & 0xFFFF) ^ 1;
	return codeword(((uint64_t)sys << 32) | (ccs << 16) | 0xAAAA);
}

/* MPT 1327 section 5.1.1 forbids SYS identities whose CCSC contains
 * SYNC anywhere inside its 48 information bits. The generator's output is
 * otherwise a valid codeword, but such a system cannot be transmitted. */
int gen_mpt1327_valid_sys(unsigned int sys)
{
	uint64_t info;
	int offset;

	if (sys > 0x7FFFu)
		return 0;
	info = ccsc(sys) >> 16;
	for (offset = 0; offset <= 32; offset++) {
		if (((info >> offset) & 0xFFFFu) == MPT1327_SYNC)
			return 0;
	}
	return 1;
}

/* Appends the n low bits of value, most significant first */
static void put_bits(struct gen_state *s, uint64_t value, int n)
{
	while (n-- > 0) {
		if (s->s.mpt1327.nbits >= 8 * sizeof(s->s.mpt1327.data))
			return;
		if ((value >> n) & 1)
			s->s.mpt1327.data[s->s.mpt1327.nbits / 8] |= 0x80 >> (s->s.mpt1327.nbits % 8);
		s->s.mpt1327.nbits++;
	}
}

/* Appends a codeword with the given number of bit errors, at positions that vary
 * along the transmission */
static void put_codeword(struct gen_state *s, uint64_t cw, int errors)
{
	int j;

	for (j = 0; j < errors; j++)
		cw ^= 1ULL << ((s->s.mpt1327.nbits / 64 * 7 + j * 29 + 3) % 64);
	put_bits(s, cw, 64);
}

static int get_bit(struct gen_state *s, unsigned int idx)
{
	return (s->s.mpt1327.data[idx / 8] >> (7 - idx % 8)) & 1;
}

/* ---------------------------------------------------------------------- */

/* Ends a message: after an odd number of data codewords, a filler data codeword keeps
 * the slot structure (3.3.3.2); then the CCSC of the next slot */
static void end_message(struct gen_params *p, struct gen_state *s, int data_codewords)
{
	if (data_codewords % 2)
		put_codeword(s, codeword(0), p->p.mpt1327.errors);
	put_codeword(s, ccsc(p->p.mpt1327.sys), p->p.mpt1327.errors);
}

void gen_init_mpt1327(struct gen_params *p, struct gen_state *s)
{
	int i, data_codewords = 0;

	memset(s, 0, sizeof(struct gen_state));
	/* start-up sequence (3.3.3.1): preamble of bit reversals ending with 0, then SYNC */
	put_bits(s, 0xAAAAAAAAu, 32);
	put_bits(s, MPT1327_SYNC, 16);
	for (i = 0; i < p->p.mpt1327.count; i++) {
		uint64_t info = p->p.mpt1327.info[i];

		/* an address codeword (bit 1 set) starts a new message in a new slot; data
		 * codewords follow it directly, displacing slots (3.3.3.2) */
		if ((info >> 47) & 1) {
			if (i > 0)
				end_message(p, s, data_codewords);
			data_codewords = 0;
		} else {
			data_codewords++;
		}
		put_codeword(s, codeword(info), p->p.mpt1327.errors);
	}
	end_message(p, s, data_codewords);
	/* trailing bit reversals, so the demodulator sees the end of the last codeword */
	put_bits(s, 0xAAAAu, 16);
	s->s.mpt1327.phinc = get_bit(s, 0) ? PHINC_1 : PHINC_0;
}

int gen_mpt1327(signed short *buf, int buflen, struct gen_params *p, struct gen_state *s)
{
	int num = 0;

	for (; buflen > 0; buflen--, buf++, num++) {
		if (s->s.mpt1327.bit_idx >= s->s.mpt1327.nbits)
			return num;
		*buf += (p->ampl * COS(s->s.mpt1327.ph)) >> 15;
		s->s.mpt1327.ph += s->s.mpt1327.phinc;
		s->s.mpt1327.bitph += BITPHINC;
		if (s->s.mpt1327.bitph >= 0x10000u) {
			s->s.mpt1327.bitph &= 0xffffu;
			s->s.mpt1327.bit_idx++;
			if (s->s.mpt1327.bit_idx < s->s.mpt1327.nbits)
				s->s.mpt1327.phinc = get_bit(s, s->s.mpt1327.bit_idx) ? PHINC_1 : PHINC_0;
		}
	}
	return num;
}
