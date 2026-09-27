/*
 *      demod_dualtone.c -- dualtone evaluator (TR-BOS C 4.6)
 *
 *      Copyright (C) 2026 https://github.com/stahmatt
 *
 *      Decodes the dualtone (Doppelton) continuous tone used to trigger
 *      sirens, as defined by the group M continuous tone per DIN 45012.
 *      Only the following two frequency pairs are recognised (tolerances
 *      according to TR-BOS):
 *
 *          dualtone 4 and 7:  fs4 =  675 Hz +/- 6 Hz
 *                             fs7 = 1240 Hz +/- 10 Hz
 *          dualtone 4 and 5:  fs4 =  675 Hz +/- 6 Hz
 *                             fs5 =  825 Hz +/- 6 Hz
 *
 *      Evaluated only
 *          a) when both tone frequencies are present simultaneously and
 *          b) after a response delay of 2 s +/- 0,5 s.
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 */

/* ---------------------------------------------------------------------- */

#include "multimon.h"

#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------------- */

#define SAMPLE_RATE 22050

/* Nominal dualtone frequencies (fs5/fs4/fs7 order for the Goertzel set) */
#define DUALTONE_FS4 675.0
#define DUALTONE_FS5 825.0
#define DUALTONE_FS7 1240.0

/* Tolerance limits (Hz) according to TR-BOS C 4.6 */
#define DUALTONE_TOL4 6.0
#define DUALTONE_TOL5 6.0
#define DUALTONE_TOL7 10.0

/* Coarse sliding window length = 100 ms at 22050 Hz */
#define DUALTONE_COARSE_BLOCK 2205

/* Coarse bins per tone: band edges + nominal frequency */
#define DUALTONE_COARSE_BINS  3

/* Fine measurement window = 1 s (1 Hz resolution) for the tolerance check */
#define DUALTONE_FINE_BLOCK   SAMPLE_RATE

/* Window evaluation interval = 10 ms (100 windows per second) */
#define DUALTONE_HOP         220

/* Minimum magnitude (normalised to 0..1) of each tone to be detected */
#define DUALTONE_COARSE_THRESHOLD 0.02f
#define DUALTONE_FINE_THRESHOLD   0.02f

/* Response delay 2 s +/- 0,5 s: 200 * 10 ms = 2.0 s */
#define DUALTONE_RESPONSE_DELAY 200

/* Dualtone pair identifiers */
#define DUALTONE_PAIR_NONE 0
#define DUALTONE_PAIR_45   1     /* fs4 + fs5 */
#define DUALTONE_PAIR_47   2     /* fs4 + fs7 */

#define DUALTONE_TWOPI      6.2831853071795864769252867665590057683943387987502

/* ---------------------------------------------------------------------- */

static float goertzel_block(const float *buf, unsigned int wpos,
                            unsigned int block, double coeff)
{
	double q0 = 0.0, q1 = 0.0, q2 = 0.0;
	double power;
	unsigned int i;
	unsigned int idx = wpos;

	for (i = 0; i < block; i++) {
		q0 = buf[idx] + coeff * q1 - q2;
		q2 = q1;
		q1 = q0;
		if (++idx >= block)
			idx = 0;
	}
	power = q1 * q1 + q2 * q2 - coeff * q1 * q2;
	if (power < 0.0)
		power = 0.0;
	return (float)(2.0 * sqrt(power) / block);
}

/* ---------------------------------------------------------------------- */

/*
 * Fine frequency measurement: scan the tolerance band around freq with
 * 1 Hz steps over a 1 s window and accept the tone if it is present
 * within the band.
 */
static int dualtone_measure(struct l1_state_dualtone *st,
                             double freq, double tol)
{
	double best = 0.0;
	double bestfreq = 0.0;
	int bin;

	for (bin = (int)(freq - tol); bin <= (int)(freq + tol); bin++) {
		double coeff = 2.0 * cos(DUALTONE_TWOPI * bin / SAMPLE_RATE);
		float amp = goertzel_block(st->window_long, st->wpos_long,
		                           DUALTONE_FINE_BLOCK, coeff);
		if (amp > best) {
			best = amp;
			bestfreq = bin;
		}
	}
	verbprintf(8, "DUALTONE: %.0f Hz -> %.1f Hz amp=%.4f\n",
	           freq, bestfreq, best);
	return best > DUALTONE_FINE_THRESHOLD;
}

static int dualtone_freq_ok(struct l1_state_dualtone *st)
{
	if (!dualtone_measure(st, DUALTONE_FS4, DUALTONE_TOL4))
		return 0;
	if (st->pair == DUALTONE_PAIR_45)
		return dualtone_measure(st, DUALTONE_FS5, DUALTONE_TOL5);
	return dualtone_measure(st, DUALTONE_FS7, DUALTONE_TOL7);
}

/* ---------------------------------------------------------------------- */

/*
 * Coarse presence check: return the strongest of the band bins so that a
 * tone detuned within the tolerance band is still detected. The exact
 * frequency is verified later by dualtone_measure().
 */
static float dualtone_coarse_amp(struct l1_state_dualtone *st, int tone)
{
	float best = 0.0f;
	int b;

	for (b = 0; b < DUALTONE_COARSE_BINS; b++) {
		float amp = goertzel_block(st->window, st->wpos,
		                           DUALTONE_COARSE_BLOCK,
		                           st->coeff[tone][b]);
		if (amp > best)
			best = amp;
	}
	return best;
}

/* ---------------------------------------------------------------------- */

static void dualtone_process_window(struct demod_state *s)
{
	struct l1_state_dualtone *st = &s->l1.dualtone;
	float a4, a5, a7;
	int pair;

	a4 = dualtone_coarse_amp(st, 0);
	a5 = dualtone_coarse_amp(st, 1);
	a7 = dualtone_coarse_amp(st, 2);

	verbprintf(8, "DUALTONE: fs4=%6.4f fs5=%6.4f fs7=%6.4f\n", a4, a5, a7);

	if (a4 > DUALTONE_COARSE_THRESHOLD &&
	    (a5 > DUALTONE_COARSE_THRESHOLD || a7 > DUALTONE_COARSE_THRESHOLD))
		pair = (a5 > a7) ? DUALTONE_PAIR_45 : DUALTONE_PAIR_47;
	else
		pair = DUALTONE_PAIR_NONE;

	if (pair != st->pair) {
		/* new pair or signal gone: restart the detection counter */
		st->pair = pair;
		st->consec = 0;
		st->triggered = 0;
	}

	if (pair == DUALTONE_PAIR_NONE)
		return;

	st->consec++;
	if (st->consec >= DUALTONE_RESPONSE_DELAY && !st->triggered) {
		st->triggered = 1;
		if (dualtone_freq_ok(st))
			verbprintf(0, "DUALTONE: fs4,fs%d\n",
			           (st->pair == DUALTONE_PAIR_45) ? 5 : 7);
	}
}

/* ---------------------------------------------------------------------- */

static void dualtone_init(struct demod_state *s)
{
	struct l1_state_dualtone *st = &s->l1.dualtone;
	static const double coarse_freq[DUALTONE_COARSE_BINS][3] = {
		{ DUALTONE_FS4 - DUALTONE_TOL4, DUALTONE_FS4,
		  DUALTONE_FS4 + DUALTONE_TOL4 },
		{ DUALTONE_FS5 - DUALTONE_TOL5, DUALTONE_FS5,
		  DUALTONE_FS5 + DUALTONE_TOL5 },
		{ DUALTONE_FS7 - DUALTONE_TOL7, DUALTONE_FS7,
		  DUALTONE_FS7 + DUALTONE_TOL7 },
	};
	int i, j;

	memset(st, 0, sizeof(*st));
	for (i = 0; i < 3; i++)
		for (j = 0; j < DUALTONE_COARSE_BINS; j++)
			st->coeff[i][j] =
				2.0 * cos(DUALTONE_TWOPI * coarse_freq[i][j] / SAMPLE_RATE);
}

/* ---------------------------------------------------------------------- */

static void dualtone_demod(struct demod_state *s, buffer_t buffer, int length)
{
	struct l1_state_dualtone *st = &s->l1.dualtone;
	int i;

	for (i = 0; i < length; i++) {
		st->window[st->wpos] = buffer.fbuffer[i];
		if (++st->wpos >= DUALTONE_COARSE_BLOCK)
			st->wpos = 0;
		st->window_long[st->wpos_long] = buffer.fbuffer[i];
		if (++st->wpos_long >= DUALTONE_FINE_BLOCK)
			st->wpos_long = 0;
		if (++st->hopcount < DUALTONE_HOP)
			continue;
		st->hopcount = 0;
		dualtone_process_window(s);
	}
}

/* ---------------------------------------------------------------------- */

const struct demod_param demod_dualtone = {
	"DUALTONE", true, SAMPLE_RATE, 0, dualtone_init, dualtone_demod, NULL
};

/* ---------------------------------------------------------------------- */
