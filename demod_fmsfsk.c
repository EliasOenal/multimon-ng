/*
 *      demod_fmsfsk.c -- 1200 baud FMS FSK demodulator
 *
 *      Copyright (C) 2014
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
 * FSM signaling 1200 baud +/- 0.01%
 * "1" frequency: 1200 Hz
 * "0" frequency: 1800 Hz
 */

#define FREQ_1 1200
#define FREQ_0 1800
#define FREQ_SAMP 22050
#define BAUD 1200
#define SUBSAMP 2

/* ---------------------------------------------------------------------- */

#define CORRLEN ((int)(FREQ_SAMP/BAUD))
#define SPHASEINC (0x10000u*BAUD*SUBSAMP/FREQ_SAMP)

static float corr_1_i[CORRLEN];
static float corr_1_q[CORRLEN];
static float corr_0_i[CORRLEN];
static float corr_0_q[CORRLEN];

/* ---------------------------------------------------------------------- */

static void fmsfsk_init(struct demod_state *s)
{
    float f;
    int i;

    fms_init(s);
    memset(&s->l1.fmsfsk, 0, sizeof(s->l1.fmsfsk));
    for (f = 0, i = 0; i < CORRLEN; i++) {
        corr_1_i[i] = cos(f);
        corr_1_q[i] = sin(f);
        f += 2.0*M_PI*FREQ_1/FREQ_SAMP;
    }
    for (f = 0, i = 0; i < CORRLEN; i++) {
        corr_0_i[i] = cos(f);
        corr_0_q[i] = sin(f);
        f += 2.0*M_PI*FREQ_0/FREQ_SAMP;
    }
}

/* ---------------------------------------------------------------------- */

static void fmsfsk_demod(struct demod_state *s, buffer_t buffer, int length)
{
    float f;
    unsigned char curbit;

    if (s->l1.fmsfsk.subsamp) {
        int numfill = SUBSAMP - s->l1.fmsfsk.subsamp;
        if (length < numfill) {
            s->l1.fmsfsk.subsamp += length;
            return;
        }
        buffer.fbuffer += numfill;
        length -= numfill;
        s->l1.fmsfsk.subsamp = 0;
    }
    for (; length >= SUBSAMP; length -= SUBSAMP, buffer.fbuffer += SUBSAMP) {
        f = 	fsqr(mac(buffer.fbuffer, corr_1_i, CORRLEN)) +
            fsqr(mac(buffer.fbuffer, corr_1_q, CORRLEN)) -
            fsqr(mac(buffer.fbuffer, corr_0_i, CORRLEN)) -
            fsqr(mac(buffer.fbuffer, corr_0_q, CORRLEN));
        s->l1.fmsfsk.dcd_shreg <<= 1;
        s->l1.fmsfsk.dcd_shreg |= (f > 0);
        verbprintf(10, "%c", '0'+(s->l1.fmsfsk.dcd_shreg & 1));
        /*
         * check if transition
         */
        if ((s->l1.fmsfsk.dcd_shreg ^ (s->l1.fmsfsk.dcd_shreg >> 1)) & 1) {
            if (s->l1.fmsfsk.sphase < (0x8000u-(SPHASEINC/2)))
                s->l1.fmsfsk.sphase += SPHASEINC/8;
            else
                s->l1.fmsfsk.sphase -= SPHASEINC/8;
        }
        s->l1.fmsfsk.sphase += SPHASEINC;
        if (s->l1.fmsfsk.sphase >= 0x10000u) {
            s->l1.fmsfsk.sphase &= 0xffffu;
            curbit = s->l1.fmsfsk.dcd_shreg & 1;
            verbprintf(9, "FMS %c ", '0'+curbit);
            fms_rxbit(s, curbit);
        }
    }
    s->l1.fmsfsk.subsamp = length;
}

/* ---------------------------------------------------------------------- */

const struct demod_param demod_fmsfsk = {
    "FMSFSK", true, FREQ_SAMP, CORRLEN, fmsfsk_init, fmsfsk_demod, NULL
};

/* ---------------------------------------------------------------------- */
