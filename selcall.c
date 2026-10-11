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

/* All fields of one completed selcall code.  Populated at the flush point
 * from s->l1.selcall and consumed by both the text and JSON paths, so neither
 * reads the demod state directly.  See docs/json-output-schema-v2.md. */
typedef struct {
    char        code[64];       /* accumulated digit string */
    int         codelen;        /* length of code */
    const char *demod_name;     /* e.g. "ZVEI1", "DZVEI" */
} SelcallFrame;

static void selcall_text_emit(const SelcallFrame *f);
static void selcall_emit(const SelcallFrame *f);
static void selcall_emit_json_v1(const SelcallFrame *f);

/* Copy the completed code out of the demod state into a SelcallFrame. */
static void selcall_fill_frame(const struct demod_state *s, const char *name,
                               SelcallFrame *f)
{
    int n = s->l1.selcall.codelen;
    if (n < 0)
        n = 0;
    if (n > (int)sizeof(f->code) - 1)
        n = (int)sizeof(f->code) - 1;
    memcpy(f->code, s->l1.selcall.code, (size_t)n);
    f->code[n] = '\0';
    f->codelen = n;
    f->demod_name = name;
}

void selcall_init(struct demod_state *s)
{
    memset(&s->l1.selcall, 0, sizeof(s->l1.selcall));
}

void selcall_deinit(struct demod_state *s)
{
    if(s->l1.selcall.timeout != 0)
    {
        if (s->l1.selcall.codelen > 0) {
            SelcallFrame f;
            selcall_fill_frame(s, s->dem_par->name, &f);
            selcall_emit(&f);
        }
        s->l1.selcall.codelen = 0;
        s->l1.selcall.code[0] = '\0';
    }
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

/* Emit one JSON v2 object for a selcall (ZVEI/DZVEI/PZVEI) code.
 * See docs/json-output-schema-v2.md: no capcode, no message, empty
 * encodings[], the accumulated digit string under selcall.code. */
static void selcall_emit_json_v1(const SelcallFrame *f)
{
    cJSON *obj = json_new(f->demod_name, "selcall", "message", "selcall");
    if (!obj) return;
    json_set_capcode(obj, -1, NULL);
    json_set_message(obj, NULL);
    cJSON *p = json_proto(obj, "selcall");
    if (p)
        cJSON_AddStringToObject(p, "code", f->code);
    json_emit(obj);
}

/* Text output for one completed selcall code: "<name>: <digits>\n".
 * Reproduces the single line the former incremental verbprintf() calls
 * emitted (name prefix on the first digit, each digit, trailing newline). */
static void selcall_text_emit(const SelcallFrame *f)
{
    verbprintf(0, "%s: %s\n", f->demod_name, f->code);
}

/* Single dispatch point for all selcall output. */
static void selcall_emit(const SelcallFrame *f)
{
    if (json_format == JSON_FORMAT_UNIFIED)
        selcall_emit_json_v1(f);
    else
        selcall_text_emit(f);
}

void selcall_demod(struct demod_state *s, const float *buffer, int length,
                   const unsigned int *selcall_freq, const char * const name)
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
                /* Digit accumulation: the completed code is emitted as one
                 * line/object at the flush point below via selcall_emit(). */
                if (s->l1.selcall.codelen < 63) {
                    static const char hexd[] = "0123456789ABCDEF";
                    s->l1.selcall.code[s->l1.selcall.codelen++] = hexd[i & 0xf];
                    s->l1.selcall.code[s->l1.selcall.codelen] = '\0';
                }
                s->l1.selcall.timeout = 1;
            }

            if(i == -1 && s->l1.selcall.timeout != 0)
                s->l1.selcall.timeout++;
            if(s->l1.selcall.timeout > TIMEOUT_LIMIT+1)
            {
                if (s->l1.selcall.codelen > 0) {
                    SelcallFrame f;
                    selcall_fill_frame(s, name, &f);
                    selcall_emit(&f);
                }
                s->l1.selcall.codelen = 0;
                s->l1.selcall.code[0] = '\0';
                s->l1.selcall.timeout = 0;
            }

            s->l1.selcall.lastch = i;
        }
    }
}
