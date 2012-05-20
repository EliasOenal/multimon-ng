/*
 *      multimon.h -- Monitor for many different modulation formats
 *
 *      Copyright (C) 1996
 *          Thomas Sailer (sailer@ife.ee.ethz.ch, hb9jnx@hb9w.che.eu)
 *
 *      Added eas parts - A. Maitland Bottoms 27 June 2000
 *
 *      Copyright (C) 2012
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

#ifndef _MULTIMON_H
#define _MULTIMON_H

#include <stdint.h>

/* ---------------------------------------------------------------------- */

extern const float costabf[0x400];
#define COS(x) costabf[(((x)>>6)&0x3ffu)]
#define SIN(x) COS((x)+0xc000)

/* ---------------------------------------------------------------------- */

enum
{
    POCSAG_MODE_AUTO = 0,
    POCSAG_MODE_NUMERIC = 1,
    POCSAG_MODE_ALPHA = 2,
    POCSAG_MODE_SKYPER = 3,
};

struct demod_state {
    const struct demod_param *dem_par;
    union {
        struct l2_state_clipfsk {
            unsigned char rxbuf[512];
            unsigned char *rxptr;
            uint32_t rxstate;
            uint32_t rxbitstream;
            uint32_t rxbitbuf;
        } clipfsk;

        struct l2_state_uart {
            unsigned char rxbuf[512];
            unsigned char *rxptr;
            uint32_t rxstate;
            uint32_t rxbitstream;
            uint32_t rxbitbuf;
        } uart;

        struct l2_state_hdlc {
            unsigned char rxbuf[512];
            unsigned char *rxptr;
            uint32_t rxstate;
            uint32_t rxbitstream;
            uint32_t rxbitbuf;
        } hdlc;

        struct l2_state_pocsag {
            uint32_t rx_data;
            struct l2_pocsag_rx {
                unsigned char rx_sync;
                unsigned char rx_word;
                unsigned char rx_bit;
                char func;
                uint32_t adr;
                unsigned char buffer[128];
                uint32_t numnibbles;
            } rx[2];
        } pocsag;
    } l2;
    union {
        struct l1_state_poc5 {
            uint32_t dcd_shreg;
            uint32_t sphase;
            uint32_t subsamp;
        } poc5;

        struct l1_state_poc12 {
            uint32_t dcd_shreg;
            uint32_t sphase;
            uint32_t subsamp;
        } poc12;

        struct l1_state_poc24 {
            uint32_t dcd_shreg;
            uint32_t sphase;
        } poc24;

        struct l1_state_eas {
            unsigned int dcd_shreg;
            unsigned int sphase;
            unsigned int lasts;
            unsigned int subsamp;
        } eas;

        struct l1_state_ufsk12 {
            unsigned int dcd_shreg;
            unsigned int sphase;
            unsigned int subsamp;
        } ufsk12;

        struct l1_state_clipfsk {
            unsigned int dcd_shreg;
            unsigned int sphase;
            uint32_t subsamp;
        } clipfsk;

        struct l1_state_afsk12 {
            uint32_t dcd_shreg;
            uint32_t sphase;
            uint32_t lasts;
            uint32_t subsamp;
        } afsk12;

        struct l1_state_afsk24 {
            unsigned int dcd_shreg;
            unsigned int sphase;
            unsigned int lasts;
        } afsk24;

        struct l1_state_hapn48 {
            unsigned int shreg;
            unsigned int sphase;
            float lvllo, lvlhi;
        } hapn48;

        struct l1_state_fsk96 {
            unsigned int dcd_shreg;
            unsigned int sphase;
            unsigned int descram;
        } fsk96;

        struct l1_state_dtmf {
            unsigned int ph[8];
            float energy[4];
            float tenergy[4][16];
            int blkcount;
            int lastch;
        } dtmf;

        struct l1_state_zvei {
            unsigned int ph[16];
            float energy[4];
            float tenergy[4][32];
            int blkcount;
            int lastch;
        } zvei;

        struct l1_state_scope {
            int datalen;
            int dispnum;
            float data[512];
        } scope;
    } l1;
};

struct demod_param {
    const char *name;
    unsigned int samplerate;
    unsigned int overlap;
    void (*init)(struct demod_state *s);
    void (*demod)(struct demod_state *s, float *buffer, int length);
};

/* ---------------------------------------------------------------------- */

extern const struct demod_param demod_poc5;
extern const struct demod_param demod_poc12;
extern const struct demod_param demod_poc24;

extern const struct demod_param demod_eas;

extern const struct demod_param demod_ufsk1200;
extern const struct demod_param demod_clipfsk;

extern const struct demod_param demod_afsk1200;
extern const struct demod_param demod_afsk2400;
extern const struct demod_param demod_afsk2400_2;
extern const struct demod_param demod_afsk2400_3;

extern const struct demod_param demod_hapn4800;
extern const struct demod_param demod_fsk9600;

extern const struct demod_param demod_dtmf;
extern const struct demod_param demod_zvei;

extern const struct demod_param demod_scope;

#define ALL_DEMOD &demod_poc5, &demod_poc12, &demod_poc24, &demod_eas, &demod_ufsk1200, &demod_clipfsk, \
&demod_afsk1200, &demod_afsk2400, &demod_afsk2400_2, &demod_afsk2400_3, &demod_hapn4800, \
&demod_fsk9600, &demod_dtmf, &demod_zvei, &demod_scope

/* ---------------------------------------------------------------------- */

void verbprintf(int verb_level, const char *fmt, ...);

void hdlc_init(struct demod_state *s);
void hdlc_rxbit(struct demod_state *s, int bit);

void uart_init(struct demod_state *s);
void uart_rxbit(struct demod_state *s, int bit);
void clip_init(struct demod_state *s);
void clip_rxbit(struct demod_state *s, int bit);

void pocsag_init(struct demod_state *s);
void pocsag_rxbit(struct demod_state *s, int bit);

void xdisp_terminate(int cnum);
int xdisp_start(void);
int xdisp_update(int cnum, float *f);

/* ---------------------------------------------------------------------- */
#endif /* _MULTIMON_H */
