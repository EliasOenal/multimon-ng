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

#ifdef _MSC_VER
#include "msvc_support.h"
#endif

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

enum EAS_L2_State
{
   EAS_L2_IDLE = 0,
   EAS_L2_HEADER_SEARCH = 1,
   EAS_L2_READING_MESSAGE = 2,
   EAS_L2_READING_EOM = 3,
};

enum EAS_L1_State
{
    EAS_L1_IDLE = 0,
    EAS_L1_SYNC = 1,
};

struct l2_state_clipfsk {
            unsigned char rxbuf[512];
            unsigned char *rxptr;
            uint32_t rxstate;
            uint32_t rxbitstream;
            uint32_t rxbitbuf;
        };

struct l2_pocsag_rx {
                unsigned char rx_sync;      // sync status
                unsigned char rx_word;      // word counter
                unsigned char rx_bit;       // bit counter, counts 32bits
                char func;                  // POCSAG function (eg 3 for text)
                uint32_t adr;               // POCSAG address
                unsigned char buffer[512];
                uint32_t numnibbles;
            } ;

struct demod_state {
    const struct demod_param *dem_par;
    union {
        struct l2_state_clipfsk clipfsk;
        struct l2_state_uart {
            unsigned char rxbuf[512*100];
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
            struct l2_pocsag_rx rx[2];
        } pocsag;

        struct l2_state_eas {
            char last_message[269];
            char msg_buf[4][269];
            char head_buf[4];
            uint32_t headlen;
            uint32_t msglen;
            uint32_t msgno;
            uint32_t state;
        } eas;
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
            unsigned char lasts;
            unsigned int subsamp;
            unsigned char byte_counter;
            int dcd_integrator;
            uint32_t state;
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

        struct l1_state_selcall {
            unsigned int ph[16];
            float energy[4];
            float tenergy[4][32];
            int blkcount;
            int lastch;
            int timeout;
        } selcall;

#ifndef NO_X11
        struct l1_state_scope {
            int datalen;
            int dispnum;
            float data[512];
        } scope;
#endif
    } l1;
};

struct demod_param {
    const char *name;
    unsigned int samplerate;
    unsigned int overlap;
    void (*init)(struct demod_state *s);
    void (*demod)(struct demod_state *s, float *buffer, int length);
    void (*deinit)(struct demod_state *s);
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

extern const struct demod_param demod_zvei1;
extern const struct demod_param demod_zvei2;
extern const struct demod_param demod_zvei3;
extern const struct demod_param demod_dzvei;
extern const struct demod_param demod_pzvei;
extern const struct demod_param demod_eea;
extern const struct demod_param demod_eia;
extern const struct demod_param demod_ccir;


#ifndef NO_X11
extern const struct demod_param demod_scope;
#endif

#ifndef NO_X11
#define SCOPE_DEMOD , &demod_scope
#else
#define SCOPE_DEMOD
#endif

#define ALL_DEMOD &demod_poc5, &demod_poc12, &demod_poc24, &demod_eas, &demod_ufsk1200, &demod_clipfsk, \
&demod_afsk1200, &demod_afsk2400, &demod_afsk2400_2, &demod_afsk2400_3, &demod_hapn4800, \
&demod_fsk9600, &demod_dtmf, &demod_zvei1, &demod_zvei2, &demod_zvei3, &demod_dzvei, \
&demod_pzvei, &demod_eea, &demod_eia, &demod_ccir SCOPE_DEMOD


/* ---------------------------------------------------------------------- */

void _verbprintf(int verb_level, const char *fmt, ...);
#if !defined(MAX_VERBOSE_LEVEL)
#   define MAX_VERBOSE_LEVEL 0
#endif
#define verbprintf(level, ...) \
    do { if (level <= MAX_VERBOSE_LEVEL) _verbprintf(level, __VA_ARGS__); } while (0)

void hdlc_init(struct demod_state *s);
void hdlc_rxbit(struct demod_state *s, int bit);

void uart_init(struct demod_state *s);
void uart_rxbit(struct demod_state *s, int bit);
void clip_init(struct demod_state *s);
void clip_rxbit(struct demod_state *s, int bit);

void pocsag_init(struct demod_state *s);
void pocsag_rxbit(struct demod_state *s, int32_t bit);
void pocsag_deinit(struct demod_state *s);

void selcall_init(struct demod_state *s);
void selcall_demod(struct demod_state *s, float *buffer, int length,
                   unsigned int *selcall_freq, const char *const name);
void selcall_deinit(struct demod_state *s);

void xdisp_terminate(int cnum);
int xdisp_start(void);
int xdisp_update(int cnum, float *f);

/* ---------------------------------------------------------------------- */
#endif /* _MULTIMON_H */
