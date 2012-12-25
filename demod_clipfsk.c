/*
 *      demod_clipfsk.c -- 1200 baud FSK demodulator
 *
 *      Copyright (C) 2007  
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

#include "multimon.h"
#include "filter.h"
#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------------- */

/*
 * V.23 signaling 1200 baud +/- 1%
 * Mark frequency: 2300 Hz +/- 1.5%
 * Space frequency: 1100 Hz +/- 1.5%
 */

#define FREQ_MARK  1200
#define FREQ_SPACE 2200
#define FREQ_SAMP  22050
#define BAUD       1200
#define SUBSAMP    2

/* ---------------------------------------------------------------------- */

#define CORRLEN ((int)(FREQ_SAMP/BAUD))
#define SPHASEINC (0x10000u*BAUD*SUBSAMP/FREQ_SAMP)

static float corr_mark_i[CORRLEN];
static float corr_mark_q[CORRLEN];
static float corr_space_i[CORRLEN];
static float corr_space_q[CORRLEN];

/* ---------------------------------------------------------------------- */
	
static void clipfsk_init(struct demod_state *s)
{
	float f;
	int i;

	clip_init(s);
	memset(&s->l1.clipfsk, 0, sizeof(s->l1.clipfsk));
	for (f = 0, i = 0; i < CORRLEN; i++) {
		corr_mark_i[i] = cos(f);
		corr_mark_q[i] = sin(f);
		f += 2.0*M_PI*FREQ_MARK/FREQ_SAMP;
	}
	for (f = 0, i = 0; i < CORRLEN; i++) {
		corr_space_i[i] = cos(f);
		corr_space_q[i] = sin(f);
		f += 2.0*M_PI*FREQ_SPACE/FREQ_SAMP;
	}
}

/* ---------------------------------------------------------------------- */

static void clipfsk_demod(struct demod_state *s, float *buffer, int length)
{
	float f;
	unsigned char curbit;

	if (s->l1.clipfsk.subsamp) {
		int numfill = SUBSAMP - s->l1.clipfsk.subsamp;
		if (length < numfill) {
			s->l1.clipfsk.subsamp += length;
			return;
		}
		buffer += numfill;
		length -= numfill;
		s->l1.clipfsk.subsamp = 0;
	}
	for (; length >= SUBSAMP; length -= SUBSAMP, buffer += SUBSAMP) {
		f = 	fsqr(mac(buffer, corr_mark_i, CORRLEN)) +
			fsqr(mac(buffer, corr_mark_q, CORRLEN)) -
			fsqr(mac(buffer, corr_space_i, CORRLEN)) -
			fsqr(mac(buffer, corr_space_q, CORRLEN));
		s->l1.clipfsk.dcd_shreg <<= 1;
		s->l1.clipfsk.dcd_shreg |= (f > 0);
		verbprintf(10, "%c", '0'+(s->l1.clipfsk.dcd_shreg & 1));
		/*
		 * check if transition
		 */
		if ((s->l1.clipfsk.dcd_shreg ^ (s->l1.clipfsk.dcd_shreg >> 1)) & 1) {
			if (s->l1.clipfsk.sphase < (0x8000u-(SPHASEINC/2)))
				s->l1.clipfsk.sphase += SPHASEINC/8;
			else
				s->l1.clipfsk.sphase -= SPHASEINC/8;
		}
		s->l1.clipfsk.sphase += SPHASEINC;
		if (s->l1.clipfsk.sphase >= 0x10000u) {
			s->l1.clipfsk.sphase &= 0xffffu;
			curbit = s->l1.clipfsk.dcd_shreg & 1;
			verbprintf(9, " %c ", '0'+curbit);
			clip_rxbit(s, curbit);
		}
	}
	s->l1.clipfsk.subsamp = length;
}

/* ---------------------------------------------------------------------- */

const struct demod_param demod_clipfsk = {
    "CLIPFSK", FREQ_SAMP, CORRLEN, clipfsk_init, clipfsk_demod, NULL
};

/* ---------------------------------------------------------------------- */
