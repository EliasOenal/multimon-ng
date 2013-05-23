/*
 *      selcall.c
 *
 *      Copyright (C) 1996
 *          Thomas Sailer (sailer@ife.ee.ethz.ch, hb9jnx@hb9w.che.eu)
 *
 *      Copyright (C) 2013
 *          Elias Oenal    (EliasOenal@gmail.com)
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

#define SAMPLE_RATE 22050
#define BLOCKLEN (SAMPLE_RATE/100)  /* 10ms blocks */
#define BLOCKNUM 4    /* must match numbers in multimon.h */
#define TIMEOUT_LIMIT 5 //50ms

void selcall_init(struct demod_state *s)
{
    memset(&s->l1.selcall, 0, sizeof(s->l1.selcall));
}

void selcall_deinit(struct demod_state *s)
{
    if(s->l1.selcall.timeout != 0)
        verbprintf(0, "\n");
}

int find_max_idx(const float *f)
{
    float en = 0;
    int idx = -1, i;

    for (i = 0; i < 16; i++)
        if (f[i] > en) {
            en = f[i];
            idx = i;
        }
    if (idx < 0)
        return -1;
    en *= 0.1;
    for (i = 0; i < 16; i++)
        if (idx != i && f[i] > en)
            return -1;
    return idx;
}

static inline int process_block(struct demod_state *s)
{
    float tote;
    float totte[32];
    int i, j;

    tote = 0;
    for (i = 0; i < BLOCKNUM; i++)
        tote += s->l1.selcall.energy[i];
    for (i = 0; i < 32; i++) {
        totte[i] = 0;
        for (j = 0; j < BLOCKNUM; j++)
            totte[i] += s->l1.selcall.tenergy[j][i];
    }
    for (i = 0; i < 16; i++)
        totte[i] = fsqr(totte[i]) + fsqr(totte[i+16]);
    memmove(s->l1.selcall.energy+1, s->l1.selcall.energy,
            sizeof(s->l1.selcall.energy) - sizeof(s->l1.selcall.energy[0]));
    s->l1.selcall.energy[0] = 0;
    memmove(s->l1.selcall.tenergy+1, s->l1.selcall.tenergy,
            sizeof(s->l1.selcall.tenergy) - sizeof(s->l1.selcall.tenergy[0]));
    memset(s->l1.selcall.tenergy, 0, sizeof(s->l1.selcall.tenergy[0]));
    tote *= (BLOCKNUM*BLOCKLEN*0.5);  /* adjust for block lengths */
    verbprintf(10, "selcall: Energies: %8.5f  %8.5f %8.5f %8.5f %8.5f %8.5f %8.5f %8.5f %8.5f"
               " %8.5f %8.5f %8.5f %8.5f %8.5f %8.5f %8.5f %8.5f\n",
               tote, totte[0], totte[1], totte[2], totte[3], totte[4], totte[5], totte[6], totte[7],
               totte[8], totte[9], totte[10], totte[11], totte[12], totte[13], totte[14], totte[15]);
    if ((i = find_max_idx(totte)) < 0)
        return -1;
    if ((tote * 0.4) > totte[i])
        return -1;
    return i;
}

void selcall_demod(struct demod_state *s, float *buffer, int length,
                   unsigned int *selcall_freq, const char * const name)
{
    float s_in;
    int i;

    for (; length > 0; length--, buffer++) {
        s_in = *buffer;
        s->l1.selcall.energy[0] += fsqr(s_in);
        for (i = 0; i < 16; i++) {
            s->l1.selcall.tenergy[0][i] += COS(s->l1.selcall.ph[i]) * s_in;
            s->l1.selcall.tenergy[0][i+16] += SIN(s->l1.selcall.ph[i]) * s_in;
            s->l1.selcall.ph[i] += selcall_freq[i];
        }
        if ((s->l1.selcall.blkcount--) <= 0) {
            s->l1.selcall.blkcount = BLOCKLEN;
            i = process_block(s);
            if (i != s->l1.selcall.lastch && i >= 0)
            {
                if(s->l1.selcall.timeout == 0)
                    verbprintf(0, "%s: ", name);
                verbprintf(0, "%1X", i);
                s->l1.selcall.timeout = 1;
            }

            if(i == -1 && s->l1.selcall.timeout != 0)
                s->l1.selcall.timeout++;
            if(s->l1.selcall.timeout > TIMEOUT_LIMIT+1)
            {
                verbprintf(0, "\n");
                s->l1.selcall.timeout = 0;
            }

            s->l1.selcall.lastch = i;
        }
    }
}
