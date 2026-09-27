/*
 *      demod_ccir_fast.c
 *
 *      Decoder for the "fast" CCIR selcall variant (~20 ms tones) that uses a
 *      fixed RRRRR-S frame: five radio-ID tones followed by a single status
 *      tone (0-15, hex tones A-F = 10-15). Consecutive identical digits are
 *      sent as the CCIR repeat tone E (2110 Hz).
 *
 *      The stock CCIR decoder (selcall.c) integrates over a 40 ms window,
 *      which is fine for standard ~100 ms CCIR tones but smears the 20 ms
 *      tones here so that only the isolated (longer) status tone survives.
 *      This decoder integrates over ~15 ms and segments the contiguous
 *      address block by tone change, then frames RRRRR-S.
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
 */

#define SAMPLE_RATE 22050
#define PHINC(x) ((x)*0x10000/SAMPLE_RATE)

#include "multimon.h"
#include "filter.h"
#include <math.h>
#include <string.h>

/* Same 16 CCIR tones as demod_ccir.c. Index 14 (2110 Hz) is the repeat tone. */
static const unsigned int ccir_fast_freq[16] = {
    PHINC(1981), PHINC(1124), PHINC(1197), PHINC(1275),
    PHINC(1358), PHINC(1446), PHINC(1540), PHINC(1640),
    PHINC(1747), PHINC(1860), PHINC(2400), PHINC(930),
    PHINC(2247), PHINC(991), PHINC(2110), PHINC(1055)
};

#define CCIR_FAST_REPEAT 14        /* tone index of the repeat tone (E) */

#define BLOCKLEN  (SAMPLE_RATE/400)  /* ~2.5 ms blocks */
#define NBLK      6                  /* integrate ~15 ms (must match multimon.h) */
#define CONFIRM   2                  /* stable blocks (~5 ms) before committing */
#define RESET_SIL 4                  /* silent blocks (~10 ms) that clear last tone */
#define TIMEOUT_SIL 60               /* silent blocks (~150 ms) that end a frame */

/* ---------------------------------------------------------------------- */

static void ccir_fast_init(struct demod_state *s)
{
    memset(&s->l1.ccir_fast, 0, sizeof(s->l1.ccir_fast));
    s->l1.ccir_fast.last_detected = -1;
    s->l1.ccir_fast.last_committed = -1;
    s->l1.ccir_fast.blkcount = BLOCKLEN;
}

/* Emit a completed frame, if it looks like a valid RRRRR-S message. */
static void ccir_fast_flush(struct demod_state *s)
{
    struct l1_state_ccir_fast *st = &s->l1.ccir_fast;
    int expanded[CCIR_FAST_MAXTONES];
    int i, prev = -1;

    if (st->frame_len >= 5) {
        /* Expand the repeat tone: E means "same digit as the previous tone". */
        for (i = 0; i < st->frame_len; i++) {
            int t = st->frame[i];
            if (t == CCIR_FAST_REPEAT && prev >= 0)
                t = prev;
            expanded[i] = t;
            prev = t;
        }

        verbprintf(0, "CCIR_FAST: ");
        for (i = 0; i < 5; i++)
            verbprintf(0, "%1X", expanded[i]);
        if (st->frame_len >= 6)
            verbprintf(0, " status %d\n", expanded[5]);
        else
            verbprintf(0, " status ?\n");
    }

    st->frame_len = 0;
    st->last_committed = -1;
}

/* Return the dominant tone index for the last NBLK blocks, or -1 if none
 * dominates (silence, noise, or a tone boundary straddle). */
static int ccir_fast_detect(struct demod_state *s)
{
    struct l1_state_ccir_fast *st = &s->l1.ccir_fast;
    float tote = 0.0f, totte[16];
    float en = 0.0f;
    int i, j, idx = -1;

    for (i = 0; i < NBLK; i++)
        tote += st->energy[i];
    for (i = 0; i < 16; i++) {
        float re = 0.0f, im = 0.0f;
        for (j = 0; j < NBLK; j++) {
            re += st->tenergy[j][i];
            im += st->tenergy[j][i + 16];
        }
        totte[i] = fsqr(re) + fsqr(im);
    }
    tote *= (NBLK * BLOCKLEN * 0.5f);

    for (i = 0; i < 16; i++)
        if (totte[i] > en) { en = totte[i]; idx = i; }
    if (idx < 0)
        return -1;
    /* Require the winner to clearly dominate every other tone. */
    en *= 0.1f;
    for (i = 0; i < 16; i++)
        if (i != idx && totte[i] > en)
            return -1;
    /* Require enough of the signal energy to sit in the matched tone. */
    if ((tote * 0.4f) > totte[idx])
        return -1;
    return idx;
}

static void ccir_fast_process_block(struct demod_state *s)
{
    struct l1_state_ccir_fast *st = &s->l1.ccir_fast;
    int d = ccir_fast_detect(s);

    /* Slide the block history down by one. */
    memmove(st->energy + 1, st->energy,
            sizeof(st->energy) - sizeof(st->energy[0]));
    st->energy[0] = 0.0f;
    memmove(st->tenergy + 1, st->tenergy,
            sizeof(st->tenergy) - sizeof(st->tenergy[0]));
    memset(st->tenergy[0], 0, sizeof(st->tenergy[0]));

    /* Track how long the current detection has been stable. */
    if (d == st->last_detected)
        st->stable_count++;
    else {
        st->last_detected = d;
        st->stable_count = 1;
    }

    if (d >= 0) {
        st->silence_blocks = 0;
        if (st->stable_count >= CONFIRM && d != st->last_committed) {
            if (st->frame_len < CCIR_FAST_MAXTONES)
                st->frame[st->frame_len++] = d;
            st->last_committed = d;
        }
    } else {
        st->silence_blocks++;
        /* A short gap lets an isolated tone re-trigger even if it repeats the
         * last committed frequency (e.g. status == final address digit). */
        if (st->silence_blocks == RESET_SIL)
            st->last_committed = -1;
        /* A long gap ends the message. */
        if (st->silence_blocks == TIMEOUT_SIL && st->frame_len > 0)
            ccir_fast_flush(s);
    }
}

static void ccir_fast_demod(struct demod_state *s, buffer_t buffer, int length)
{
    struct l1_state_ccir_fast *st = &s->l1.ccir_fast;
    const float *buf = buffer.fbuffer;
    int i;

    for (; length > 0; length--, buf++) {
        float s_in = *buf;
        st->energy[0] += fsqr(s_in);
        for (i = 0; i < 16; i++) {
            st->tenergy[0][i]      += COS(st->ph[i]) * s_in;
            st->tenergy[0][i + 16] += SIN(st->ph[i]) * s_in;
            st->ph[i] += ccir_fast_freq[i];
        }
        if (--st->blkcount <= 0) {
            st->blkcount = BLOCKLEN;
            ccir_fast_process_block(s);
        }
    }
}

static void ccir_fast_deinit(struct demod_state *s)
{
    /* Emit whatever complete frame is pending at end of stream. */
    if (s->l1.ccir_fast.frame_len > 0)
        ccir_fast_flush(s);
}

const struct demod_param demod_ccir_fast = {
    "CCIR_FAST", true, SAMPLE_RATE, 0,
    ccir_fast_init, ccir_fast_demod, ccir_fast_deinit
};
