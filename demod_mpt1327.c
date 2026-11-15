/*
 *      demod_mpt1327.c -- MPT 1327 trunked radio signalling decoder
 *
 *      Copyright (C) 2026 Ryan Drake
 *
 *      Decodes the 1200 bit/s FFSK signalling that MPT 1327 ("A Signalling
 *      Standard for Trunked Private Land Mobile Radio Systems", June 1997
 *      issue) trunking system controllers transmit, and prints one line per
 *      codeword. Section numbers below refer to that standard.
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
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------- */

/*
 * FFSK, 1200 bit/s (3.1)
 * "1" frequency: 1200 Hz
 * "0" frequency: 1800 Hz
 */

#define FREQ_1 1200
#define FREQ_0 1800
#define FREQ_SAMP 22050
#define BAUD 1200
#define SUBSAMP 2

#define CORRLEN ((int)(FREQ_SAMP/BAUD))
#define SPHASEINC (0x10000u*BAUD*SUBSAMP/FREQ_SAMP)

/* codeword synchronisation sequences (3.2.1) */
#define SYNC 0xC4D7u
#define SYNT 0x3B28u

/* x^15 + x^14 + x^13 + x^11 + x^4 + x^2 + 1 without the x^15 term (3.2.3) */
#define GENERATOR 0x6815u

extern int json_mode;

static float corr_1_i[CORRLEN];
static float corr_1_q[CORRLEN];
static float corr_0_i[CORRLEN];
static float corr_0_q[CORRLEN];

/* ---------------------------------------------------------------------- */

/* Field of a codeword at bits first..last, numbered as in the standard (bit 1 is sent first) */
static unsigned int mpt1327_field(uint64_t cw, int first, int last)
{
    return (unsigned int)((cw >> (64 - last)) & ((1ULL << (last - first + 1)) - 1));
}

/* Remainder of info(x) * x^15 divided by the generator, for the 48 information bits */
static unsigned int mpt1327_remainder(uint64_t info)
{
    unsigned int r = 0;
    int i;

    for (i = 47; i >= 0; i--) {
        unsigned int feedback = ((unsigned int)(info >> i) ^ (r >> 14)) & 1;
        r = (r << 1) & 0x7FFF;
        if (feedback)
            r ^= GENERATOR;
    }
    return r;
}

/* Check bits with the last one inverted, and even parity over all 64 bits (3.2.3) */
static int mpt1327_valid(uint64_t cw)
{
    uint64_t v = cw;
    int parity = 0;

    if (((cw >> 1) & 0x7FFF) != (mpt1327_remainder(cw >> 16) ^ 1))
        return 0;
    while (v) {
        parity ^= (int)(v & 1);
        v >>= 1;
    }
    return !parity;
}

/* Corrects up to one bit error; returns the number of bits corrected, or -1 */
static int mpt1327_correct(uint64_t *cw)
{
    int i;

    if (mpt1327_valid(*cw))
        return 0;
    for (i = 0; i < 64; i++) {
        if (mpt1327_valid(*cw ^ (1ULL << i))) {
            *cw ^= 1ULL << i;
            return 1;
        }
    }
    return -1;
}

/* System codewords (5.1.1, 5.1.2): identical layouts, different check bits. */
static int mpt1327_is_system_codeword(uint64_t cw, unsigned int sync)
{
    return mpt1327_field(cw, 1, 1) == 0 &&
           mpt1327_field(cw, 33, 48) == 0xAAAA &&
           mpt1327_field(cw, 49, 64) == sync;
}

static int mpt1327_is_ccsc(uint64_t cw)
{
    return mpt1327_is_system_codeword(cw, SYNC);
}

static int mpt1327_is_dcsc(uint64_t cw)
{
    return mpt1327_is_system_codeword(cw, SYNT);
}

/* ---------------------------------------------------------------------- */

/* An ident as text: by name for the special idents (table 4-1), otherwise its number */
static const char *mpt1327_ident(char *buf, size_t size, unsigned int ident)
{
    static const char *const special[] = { "PSTNGI", "PABXI", "DNI" };
    static const char *const special2[] = {
        "REGI", "INCI", "DIVERTI", "SDMI", "IPFIXI", "TSCI", "ALLI"
    };

    if (ident == 0)
        return "DUMMYI";
    if (ident >= 8101 && ident <= 8103)
        return special[ident - 8101];
    if (ident >= 8185 && ident <= 8191)
        return special2[ident - 8185];
    snprintf(buf, size, "%u", ident);
    return buf;
}

/* A field as a string of n binary digits */
static const char *mpt1327_bits(char *buf, unsigned int value, int n)
{
    int i;

    for (i = 0; i < n; i++)
        buf[i] = (char)('0' + ((value >> (n - 1 - i)) & 1));
    buf[n] = '\0';
    return buf;
}

/* Prints one codeword as a text line, or as a JSON object with --json.
 * Names and layouts from section 5. */
static void mpt1327_print(uint64_t cw)
{
    /* CAT '000' function names by TYPE (5.3) for messages from the trunking system
     * controller; NULL for spare and reserved functions */
    static const char *const funcs[4][8] = {
        { "ALH", "ALHS", "ALHD", "ALHE", "ALHR", "ALHX", "ALHF", NULL },
        { "ACK", "ACKI", "ACKQ", "ACKX", "ACKV", "ACKE", "ACKT", "ACKB" },
        { "AHY", NULL, "AHYX", NULL, NULL, "AHYP", "AHYQ", "AHYC" },
        { "MARK", "MAINT", "CLEAR", "MOVE", "BCAST", NULL, NULL, NULL },
    };
    cJSON *json_output = json_mode ? cJSON_CreateObject() : NULL;
    unsigned int pfix = mpt1327_field(cw, 2, 8);
    unsigned int ident1 = mpt1327_field(cw, 9, 21);
    unsigned int cat = mpt1327_field(cw, 23, 25);
    unsigned int type = mpt1327_field(cw, 26, 27);
    unsigned int func = mpt1327_field(cw, 28, 30);
    unsigned int ident2, sysdef;
    const char *name;
    char id1[8], id2[8], bits[3][4], hex[24];

    if (json_mode && json_output == NULL)
        return; /* avoid a null dereference under memory pressure */
    if (!json_mode)
        verbprintf(0, "MPT1327:");
    else
        cJSON_AddStringToObject(json_output, "demod_name", "MPT1327");

    if (mpt1327_field(cw, 1, 1) == 0) {
        if (mpt1327_is_ccsc(cw)) {
            snprintf(hex, sizeof(hex), "%04X", mpt1327_field(cw, 2, 16));
            if (!json_mode)
                verbprintf(0, " CCSC SYS=%s", hex);
            else {
                cJSON_AddStringToObject(json_output, "message", "CCSC");
                cJSON_AddStringToObject(json_output, "sys", hex);
            }
        } else if (mpt1327_is_dcsc(cw)) {
            /* Standard data channel system codeword (5.1.2). */
            snprintf(hex, sizeof(hex), "%04X", mpt1327_field(cw, 2, 16));
            if (!json_mode)
                verbprintf(0, " DCSC SYS=%s", hex);
            else {
                cJSON_AddStringToObject(json_output, "message", "DCSC");
                cJSON_AddStringToObject(json_output, "sys", hex);
            }
        } else {
            /* information field of a data codeword */
            snprintf(hex, sizeof(hex), "0x%012" PRIx64, (uint64_t)((cw >> 16) & 0x7FFFFFFFFFFFULL));
            if (!json_mode)
                verbprintf(0, " DATA %s", hex);
            else {
                cJSON_AddStringToObject(json_output, "message", "DATA");
                cJSON_AddStringToObject(json_output, "info", hex);
            }
        }
    } else if (mpt1327_field(cw, 22, 22) == 0) {
        /* Go To Channel (5.4) */
        ident2 = mpt1327_field(cw, 34, 46);
        if (!json_mode)
            verbprintf(0, " GTC PFIX=%u IDENT1=%s D=%u CHAN=%u IDENT2=%s N=%u",
                       pfix, mpt1327_ident(id1, sizeof(id1), ident1), mpt1327_field(cw, 23, 23),
                       mpt1327_field(cw, 24, 33), mpt1327_ident(id2, sizeof(id2), ident2),
                       mpt1327_field(cw, 47, 48));
        else {
            cJSON_AddStringToObject(json_output, "message", "GTC");
            cJSON_AddNumberToObject(json_output, "pfix", pfix);
            cJSON_AddNumberToObject(json_output, "ident1", ident1);
            cJSON_AddNumberToObject(json_output, "d", mpt1327_field(cw, 23, 23));
            cJSON_AddNumberToObject(json_output, "chan", mpt1327_field(cw, 24, 33));
            cJSON_AddNumberToObject(json_output, "ident2", ident2);
            cJSON_AddNumberToObject(json_output, "n", mpt1327_field(cw, 47, 48));
        }
    } else if (cat != 0) {
        /* categories other than '000': parameters in hex */
        snprintf(hex, sizeof(hex), "0x%06x", mpt1327_field(cw, 26, 48));
        if (!json_mode)
            verbprintf(0, " CAT=%s PFIX=%u IDENT1=%s PARAMS=%s", mpt1327_bits(bits[0], cat, 3),
                       pfix, mpt1327_ident(id1, sizeof(id1), ident1), hex);
        else {
            /* Classification of an unparsed category, not an OTA mnemonic. */
            cJSON_AddStringToObject(json_output, "message", "UNDECODED");
            cJSON_AddStringToObject(json_output, "cat", mpt1327_bits(bits[0], cat, 3));
            cJSON_AddNumberToObject(json_output, "pfix", pfix);
            cJSON_AddNumberToObject(json_output, "ident1", ident1);
            cJSON_AddStringToObject(json_output, "params", hex);
        }
    } else if ((name = funcs[type][func]) == NULL) {
        /* spare and reserved functions: parameters in hex */
        snprintf(hex, sizeof(hex), "0x%05x", mpt1327_field(cw, 31, 48));
        mpt1327_bits(bits[0], cat, 3);
        mpt1327_bits(bits[1], type, 2);
        mpt1327_bits(bits[2], func, 3);
        if (!json_mode)
            verbprintf(0, " CAT=%s TYPE=%s FUNC=%s PFIX=%u IDENT1=%s PARAMS=%s", bits[0], bits[1],
                       bits[2], pfix, mpt1327_ident(id1, sizeof(id1), ident1), hex);
        else {
            /* Unknown function: do not assign a made-up protocol mnemonic. */
            cJSON_AddStringToObject(json_output, "message", "UNDECODED");
            cJSON_AddStringToObject(json_output, "cat", bits[0]);
            cJSON_AddStringToObject(json_output, "type", bits[1]);
            cJSON_AddStringToObject(json_output, "func", bits[2]);
            cJSON_AddNumberToObject(json_output, "pfix", pfix);
            cJSON_AddNumberToObject(json_output, "ident1", ident1);
            cJSON_AddStringToObject(json_output, "params", hex);
        }
    } else if (type == 0) {
        /* Aloha (5.5.1) */
        if (!json_mode)
            verbprintf(0, " %s PFIX=%u IDENT1=%s CHAN4=%u WT=%u M=%u N=%u",
                       name, pfix, mpt1327_ident(id1, sizeof(id1), ident1),
                       mpt1327_field(cw, 31, 34), mpt1327_field(cw, 35, 37),
                       mpt1327_field(cw, 40, 44), mpt1327_field(cw, 45, 48));
        else {
            cJSON_AddStringToObject(json_output, "message", name);
            cJSON_AddNumberToObject(json_output, "pfix", pfix);
            cJSON_AddNumberToObject(json_output, "ident1", ident1);
            cJSON_AddNumberToObject(json_output, "chan4", mpt1327_field(cw, 31, 34));
            cJSON_AddNumberToObject(json_output, "wt", mpt1327_field(cw, 35, 37));
            cJSON_AddNumberToObject(json_output, "m", mpt1327_field(cw, 40, 44));
            cJSON_AddNumberToObject(json_output, "n", mpt1327_field(cw, 45, 48));
        }
    } else if (type == 1) {
        /* acknowledgement (5.5.2) */
        ident2 = mpt1327_field(cw, 31, 43);
        if (!json_mode)
            verbprintf(0, " %s PFIX=%u IDENT1=%s IDENT2=%s QUAL=%u N=%u",
                       name, pfix, mpt1327_ident(id1, sizeof(id1), ident1),
                       mpt1327_ident(id2, sizeof(id2), ident2),
                       mpt1327_field(cw, 44, 44), mpt1327_field(cw, 45, 48));
        else {
            cJSON_AddStringToObject(json_output, "message", name);
            cJSON_AddNumberToObject(json_output, "pfix", pfix);
            cJSON_AddNumberToObject(json_output, "ident1", ident1);
            cJSON_AddNumberToObject(json_output, "ident2", ident2);
            cJSON_AddNumberToObject(json_output, "qual", mpt1327_field(cw, 44, 44));
            cJSON_AddNumberToObject(json_output, "n", mpt1327_field(cw, 45, 48));
        }
    } else if (type == 2) {
        /* Ahoy (5.5.3.2) */
        ident2 = mpt1327_field(cw, 31, 43);
        if (!json_mode)
            verbprintf(0, " %s PFIX=%u IDENT1=%s IDENT2=%s", name, pfix,
                       mpt1327_ident(id1, sizeof(id1), ident1),
                       mpt1327_ident(id2, sizeof(id2), ident2));
        else {
            cJSON_AddStringToObject(json_output, "message", name);
            cJSON_AddNumberToObject(json_output, "pfix", pfix);
            cJSON_AddNumberToObject(json_output, "ident1", ident1);
            cJSON_AddNumberToObject(json_output, "ident2", ident2);
        }
        if (func == 0) {
            /* AHY */
            if (!json_mode)
                verbprintf(0, " D=%u POINT=%u CHECK=%u E=%u AD=%u",
                           mpt1327_field(cw, 44, 44), mpt1327_field(cw, 45, 45),
                           mpt1327_field(cw, 46, 46), mpt1327_field(cw, 47, 47),
                           mpt1327_field(cw, 48, 48));
            else {
                cJSON_AddNumberToObject(json_output, "d", mpt1327_field(cw, 44, 44));
                cJSON_AddNumberToObject(json_output, "point", mpt1327_field(cw, 45, 45));
                cJSON_AddNumberToObject(json_output, "check", mpt1327_field(cw, 46, 46));
                cJSON_AddNumberToObject(json_output, "e", mpt1327_field(cw, 47, 47));
                cJSON_AddNumberToObject(json_output, "ad", mpt1327_field(cw, 48, 48));
            }
        } else if (func == 6) {
            /* AHYQ */
            if (!json_mode)
                verbprintf(0, " STATUS=%u", mpt1327_field(cw, 44, 48));
            else
                cJSON_AddNumberToObject(json_output, "status", mpt1327_field(cw, 44, 48));
        } else if (func == 7) {
            /* AHYC */
            if (!json_mode)
                verbprintf(0, " SLOTS=%u DESC=%u",
                           mpt1327_field(cw, 44, 45), mpt1327_field(cw, 46, 48));
            else {
                cJSON_AddNumberToObject(json_output, "slots", mpt1327_field(cw, 44, 45));
                cJSON_AddNumberToObject(json_output, "desc", mpt1327_field(cw, 46, 48));
            }
        }
    } else if (func == 0) {
        /* MARK (5.5.4.1) */
        snprintf(hex, sizeof(hex), "%04X", mpt1327_field(cw, 7, 21));
        if (!json_mode)
            verbprintf(0, " MARK CHAN4=%u SYS=%s", mpt1327_field(cw, 2, 5), hex);
        else {
            cJSON_AddStringToObject(json_output, "message", "MARK");
            cJSON_AddNumberToObject(json_output, "chan4", mpt1327_field(cw, 2, 5));
            cJSON_AddStringToObject(json_output, "sys", hex);
        }
    } else if (func == 1) {
        /* MAINT (5.5.4.2) */
        if (!json_mode)
            verbprintf(0, " MAINT PFIX=%u IDENT1=%s CHAN=%u OPER=%u", pfix,
                       mpt1327_ident(id1, sizeof(id1), ident1),
                       mpt1327_field(cw, 31, 40), mpt1327_field(cw, 41, 43));
        else {
            cJSON_AddStringToObject(json_output, "message", "MAINT");
            cJSON_AddNumberToObject(json_output, "pfix", pfix);
            cJSON_AddNumberToObject(json_output, "ident1", ident1);
            cJSON_AddNumberToObject(json_output, "chan", mpt1327_field(cw, 31, 40));
            cJSON_AddNumberToObject(json_output, "oper", mpt1327_field(cw, 41, 43));
        }
    } else if (func == 2) {
        /* CLEAR (5.5.4.3) */
        if (!json_mode)
            verbprintf(0, " CLEAR CHAN=%u CONT=%u",
                       mpt1327_field(cw, 2, 11), mpt1327_field(cw, 12, 21));
        else {
            cJSON_AddStringToObject(json_output, "message", "CLEAR");
            cJSON_AddNumberToObject(json_output, "chan", mpt1327_field(cw, 2, 11));
            cJSON_AddNumberToObject(json_output, "cont", mpt1327_field(cw, 12, 21));
        }
    } else if (func == 3) {
        /* MOVE (5.5.4.4) */
        if (!json_mode)
            verbprintf(0, " MOVE PFIX=%u IDENT1=%s CONT=%u M=%u", pfix,
                       mpt1327_ident(id1, sizeof(id1), ident1),
                       mpt1327_field(cw, 31, 40), mpt1327_field(cw, 41, 45));
        else {
            cJSON_AddStringToObject(json_output, "message", "MOVE");
            cJSON_AddNumberToObject(json_output, "pfix", pfix);
            cJSON_AddNumberToObject(json_output, "ident1", ident1);
            cJSON_AddNumberToObject(json_output, "cont", mpt1327_field(cw, 31, 40));
            cJSON_AddNumberToObject(json_output, "m", mpt1327_field(cw, 41, 45));
        }
    } else {
        /* BCAST (5.5.4.5) */
        sysdef = mpt1327_field(cw, 2, 6);
        snprintf(hex, sizeof(hex), "%04X", mpt1327_field(cw, 7, 21));
        if (!json_mode)
            verbprintf(0, " BCAST SYSDEF=%u SYS=%s", sysdef, hex);
        else {
            cJSON_AddStringToObject(json_output, "message", "BCAST");
            cJSON_AddNumberToObject(json_output, "sysdef", sysdef);
            cJSON_AddStringToObject(json_output, "sys", hex);
        }
        if (sysdef == 0 || sysdef == 1) {
            /* announce or withdraw control channel */
            if (!json_mode)
                verbprintf(0, " CHAN=%u", mpt1327_field(cw, 31, 40));
            else
                cJSON_AddNumberToObject(json_output, "chan", mpt1327_field(cw, 31, 40));
        } else if (sysdef == 2) {
            /* call maintenance parameters */
            if (!json_mode)
                verbprintf(0, " PER=%u IVAL=%u PON=%u ID=%u",
                           mpt1327_field(cw, 31, 31), mpt1327_field(cw, 32, 36),
                           mpt1327_field(cw, 37, 37), mpt1327_field(cw, 38, 38));
            else {
                cJSON_AddNumberToObject(json_output, "per", mpt1327_field(cw, 31, 31));
                cJSON_AddNumberToObject(json_output, "ival", mpt1327_field(cw, 32, 36));
                cJSON_AddNumberToObject(json_output, "pon", mpt1327_field(cw, 37, 37));
                cJSON_AddNumberToObject(json_output, "id", mpt1327_field(cw, 38, 38));
            }
        } else if (sysdef == 4 || sysdef == 5) {
            /* adjacent site control channel, vote now advice */
            if (!json_mode)
                verbprintf(0, " CHAN=%u ADJSITE=%u",
                           mpt1327_field(cw, 31, 40), mpt1327_field(cw, 45, 48));
            else {
                cJSON_AddNumberToObject(json_output, "chan", mpt1327_field(cw, 31, 40));
                cJSON_AddNumberToObject(json_output, "adjsite", mpt1327_field(cw, 45, 48));
            }
        } else {
            snprintf(hex, sizeof(hex), "0x%05x", mpt1327_field(cw, 31, 48));
            if (!json_mode)
                verbprintf(0, " PARAMS=%s", hex);
            else
                cJSON_AddStringToObject(json_output, "params", hex);
        }
    }

    if (!json_mode)
        verbprintf(0, "\n");
    else {
        addJsonTimestamp(json_output);
        printJson(json_output);
        cJSON_Delete(json_output);
    }
}

/* ---------------------------------------------------------------------- */

/* Print system changes, including a change between control/data channels.
 * A repeated CCSC or DCSC with the same identity is suppressed. */
static void mpt1327_system(struct demod_state *s, uint64_t cw)
{
    int sys = (int)mpt1327_field(cw, 2, 16);
    unsigned int sync = mpt1327_is_ccsc(cw) ? SYNC : SYNT;

    if (sys != s->l1.mpt1327.sys || sync != s->l1.mpt1327.sys_sync) {
        s->l1.mpt1327.sys = sys;
        s->l1.mpt1327.sys_sync = sync;
        mpt1327_print(cw);
    }
}

/* Emit a checksum-validated, possibly corrected codeword. */
static void mpt1327_emit(struct demod_state *s, uint64_t cw)
{
    if (mpt1327_is_ccsc(cw) || mpt1327_is_dcsc(cw))
        mpt1327_system(s, cw);
    else
        mpt1327_print(cw);
}

/*
 * Inspect every SYNT + address and preamble + SYNC + address candidate,
 * including overlapping markers. A preamble alone is not proof of a new
 * frame: it can occur inside a valid DATA information field.
 *
 * A validated word may be held for up to 64 more bits (or EOF) while
 * overlapping address candidates mature. A marker-only acquisition also
 * holds its first word: if no stronger candidate appears, emit that word
 * even without a second valid word. Two valid words confirm the alignment;
 * they are not a prerequisite for reporting a solitary message.
 *
 * Ambiguity policy: preserve confirmed framing when both interpretations
 * pass the checksum. At an expected word boundary, a clean new address may
 * beat a one-bit-corrected old word, but equal correction counts favor the
 * old alignment. A system word overlapping recent confirmed data loses;
 * without confirmed framing, a validated SYNT/address or system word wins.
 * A standalone SYNC/SYNT must match exactly; a corrupt standalone marker
 * cannot establish framing. For a marker at the start of a validated word,
 * wait up to 48 bits for another word on the old alignment: a valid second
 * word wins; a failed second word accepts the verified new address. At EOF,
 * prefer the interpretation needing fewer bit corrections; equal-strength
 * candidates keep established framing. An equally valid real boundary can
 * therefore be missed when reception ends before arbitration completes.
 */
static int mpt1327_candidate_conflicts(const struct demod_state *s)
{
    /* A validated codeword ended within the candidate's 64-bit span.
     * Prefer the established alignment when both checksums are valid. */
    return s->l1.mpt1327.alignment_confirmed &&
           s->l1.mpt1327.valid_age < 64;
}

/* When an aligned word *begins* with a plausible new marker, both the
 * aligned word and a shifted address can pass the checksum. Neither check
 * proves the boundary. Hold the shifted address until the next aligned word
 * completes. Continuation of the old alignment wins; an invalid next word
 * admits the shifted address. At EOF, fewer corrections wins; equal-strength
 * interpretations favor established framing. */
static int mpt1327_competing_boundary(const struct demod_state *s)
{
    unsigned int age = s->l1.mpt1327.valid_age;

    if (!s->l1.mpt1327.alignment_confirmed ||
        !s->l1.mpt1327.deferred_valid ||
        s->l1.mpt1327.deferred_age != age ||
        s->l1.mpt1327.competing_valid ||
        s->l1.mpt1327.count != (int)age - 1)
        return 0;
    /* Test the on-air marker history, not deferred_cw: correcting a bit in
     * the old codeword may change SYNT/SYNC and erase the evidence of a
     * genuine new boundary. Both interpretations still need a valid CRC. */
    if (age == 16)
        return s->l1.mpt1327.preceding_sync == SYNT;
    if (age == 32)
        return s->l1.mpt1327.preceding_preamble == 0xAAAAu &&
               (s->l1.mpt1327.preceding_sync == SYNT ||
                s->l1.mpt1327.preceding_sync == SYNC);
    if (age == 48)
        return s->l1.mpt1327.history_bits >= 112 &&
               s->l1.mpt1327.preceding_preamble2 == 0xAAAAu &&
               s->l1.mpt1327.preceding_preamble == 0xAAAAu &&
               (s->l1.mpt1327.preceding_sync == SYNT ||
                s->l1.mpt1327.preceding_sync == SYNC);
    return 0;
}

/* The continuation on the old alignment failed. Adopt the buffered address
 * and account for bits already received on its alignment. */
static void mpt1327_accept_competing(struct demod_state *s)
{
    unsigned int elapsed = 64 - s->l1.mpt1327.competing_prefix_bits;
    uint64_t cw = s->l1.mpt1327.competing_cw;
    unsigned int corrections = s->l1.mpt1327.competing_corrections;

    s->l1.mpt1327.competing_valid = 0;
    s->l1.mpt1327.deferred_valid = 0;
    s->l1.mpt1327.count = (int)elapsed;
    s->l1.mpt1327.tentative_alignment = 0;
    s->l1.mpt1327.alignment_confirmed = 1;
    s->l1.mpt1327.aligned_words = 1;
    s->l1.mpt1327.valid_age = elapsed;
    s->l1.mpt1327.last_word_corrections = corrections;
    mpt1327_print(cw);
}

static int mpt1327_address_candidate_conflicts(const struct demod_state *s, int corrected)
{
    if (!mpt1327_candidate_conflicts(s))
        return 0;
    /* A clean word on the established alignment is at least as strong as
     * a competing checksum-valid address, even when a full SYNT/preamble
     * happens to begin at that word boundary. Prefer a new boundary only
     * when the prior word required correction and the candidate does not.
     * Otherwise two overlapping, perfectly valid codewords could make a
     * spurious traffic message and suppress both real words. */
    if (s->l1.mpt1327.last_word_corrections <= (unsigned int)corrected)
        return 1;
    /* At an aligned word boundary, a verified SYNT + address can look like
     * a one-bit-correctable DATA word on the old alignment. Prefer the
     * explicit traffic boundary. Also allow 16- or 32-bit alternating
     * preambles followed by SYNC/SYNT at the word boundary. Else preserve
     * the validated word containing an incidental marker. */
    if (s->l1.mpt1327.preceding_sync == SYNT &&
        s->l1.mpt1327.valid_age == 16)
        return 0;
    if (s->l1.mpt1327.preceding_preamble == 0xAAAAu &&
        s->l1.mpt1327.valid_age == 32)
        return 0;
    if (s->l1.mpt1327.history_bits >= 112 &&
        s->l1.mpt1327.preceding_preamble == 0xAAAAu &&
        s->l1.mpt1327.preceding_preamble2 == 0xAAAAu &&
        s->l1.mpt1327.valid_age == 48)
        return 0;
    return 1;
}

static void mpt1327_rxbit(struct demod_state *s, int bit)
{
    uint64_t cw;
    unsigned int sync;
    int corrected;

    s->l1.mpt1327.preceding_preamble2 =
        (uint16_t)((s->l1.mpt1327.preceding_preamble2 << 1) |
                   (s->l1.mpt1327.preceding_preamble >> 15));
    s->l1.mpt1327.preceding_preamble =
        (uint16_t)((s->l1.mpt1327.preceding_preamble << 1) |
                   (s->l1.mpt1327.preceding_sync >> 15));
    s->l1.mpt1327.preceding_sync =
        (uint16_t)((s->l1.mpt1327.preceding_sync << 1) |
                   (s->l1.mpt1327.shreg >> 63));
    s->l1.mpt1327.shreg = (s->l1.mpt1327.shreg << 1) | (bit & 1);
    if (s->l1.mpt1327.history_bits < 112)
        s->l1.mpt1327.history_bits++;
    if (s->l1.mpt1327.sync_age < 64)
        s->l1.mpt1327.sync_age++;
    if (s->l1.mpt1327.valid_age < 65)
        s->l1.mpt1327.valid_age++;
    if (s->l1.mpt1327.competing_valid && s->l1.mpt1327.competing_wait)
        s->l1.mpt1327.competing_wait--;
    if (s->l1.mpt1327.deferred_valid) {
        if (s->l1.mpt1327.deferred_delay)
            s->l1.mpt1327.deferred_delay--;
        if (s->l1.mpt1327.deferred_age < 80)
            s->l1.mpt1327.deferred_age++;
    }

    sync = (unsigned int)(s->l1.mpt1327.shreg & 0xFFFFu);
    if (sync == SYNC || sync == SYNT)
        s->l1.mpt1327.sync_age = 0;

    /* Only acquire a CCSC/DCSC when no recently validated confirmed word
     * overlaps it; a matching 16-bit trailer alone is not decisive. */
    if (s->l1.mpt1327.history_bits >= 64 && (sync == SYNC || sync == SYNT)) {
        cw = s->l1.mpt1327.shreg;
        corrected = mpt1327_correct(&cw);
        if (corrected >= 0 &&
            (mpt1327_is_ccsc(cw) || mpt1327_is_dcsc(cw)) &&
            !mpt1327_candidate_conflicts(s)) {
            if (s->l1.mpt1327.deferred_valid &&
                s->l1.mpt1327.deferred_age >= 64)
                mpt1327_emit(s, s->l1.mpt1327.deferred_cw);
            s->l1.mpt1327.deferred_valid = 0;
            s->l1.mpt1327.competing_valid = 0;
            s->l1.mpt1327.count = 0;
            s->l1.mpt1327.tentative_alignment = 0;
            s->l1.mpt1327.alignment_confirmed = 1;
            s->l1.mpt1327.aligned_words = 1;
            s->l1.mpt1327.valid_age = 0;
            s->l1.mpt1327.last_word_corrections = (unsigned int)corrected;
            mpt1327_system(s, cw);
            return;
        }
    }

    /* Sliding 80/96/112-bit window: a verified address after SYNT, or after
     * SYNC with its required alternating-bit preamble. A bare SYNC is
     * provisional, since it also occurs as a CCSC trailer. */
    if (s->l1.mpt1327.history_bits >= 80 &&
        (s->l1.mpt1327.preceding_sync == SYNT ||
         (s->l1.mpt1327.history_bits >= 96 &&
          s->l1.mpt1327.preceding_sync == SYNC &&
          s->l1.mpt1327.preceding_preamble == 0xAAAAu))) {
        cw = s->l1.mpt1327.shreg;
        corrected = mpt1327_correct(&cw);
        if (!s->l1.mpt1327.competing_valid &&
            corrected >= 0 && mpt1327_field(cw, 1, 1) == 1 &&
            /* An existing checksum-valid word ending exactly at the
             * marker is stronger evidence of continuous framing than an
             * incidental synchronization pattern in its check bits. */
            !(s->l1.mpt1327.count == 63 &&
              s->l1.mpt1327.deferred_valid &&
              s->l1.mpt1327.deferred_age >= 64)) {
            if (mpt1327_competing_boundary(s)) {
                /* At this point two interpretations validate. Keep the
                 * prior word unprinted until one more aligned CRC/EOF. */
                s->l1.mpt1327.competing_cw = cw;
                s->l1.mpt1327.competing_corrections = (unsigned int)corrected;
                s->l1.mpt1327.competing_prefix_bits = s->l1.mpt1327.valid_age;
                s->l1.mpt1327.competing_wait = 64 - s->l1.mpt1327.valid_age;
                s->l1.mpt1327.competing_valid = 1;
                s->l1.mpt1327.deferred_delay = s->l1.mpt1327.competing_wait;
            } else if (!mpt1327_address_candidate_conflicts(s, corrected)) {
                s->l1.mpt1327.deferred_valid = 0;
                s->l1.mpt1327.count = 0;
                s->l1.mpt1327.tentative_alignment = 0;
                s->l1.mpt1327.alignment_confirmed = 1;
                s->l1.mpt1327.aligned_words = 1;
                s->l1.mpt1327.valid_age = 0;
                s->l1.mpt1327.last_word_corrections = (unsigned int)corrected;
                mpt1327_print(cw);
                return;
            }
        }
    }

    /* Once every overlapping alternative has matured, print the deferred
     * word, before any later word on the current framing. */
    if (s->l1.mpt1327.deferred_valid &&
        !s->l1.mpt1327.competing_valid &&
        s->l1.mpt1327.deferred_delay == 0) {
        mpt1327_emit(s, s->l1.mpt1327.deferred_cw);
        s->l1.mpt1327.deferred_valid = 0;
    }

    if (s->l1.mpt1327.count >= 0 && ++s->l1.mpt1327.count == 64) {
        cw = s->l1.mpt1327.shreg;
        corrected = mpt1327_correct(&cw);
        if (s->l1.mpt1327.competing_valid &&
            s->l1.mpt1327.competing_wait == 0) {
            if (corrected < 0) {
                mpt1327_accept_competing(s);
                return;
            }
            /* The second old-aligned codeword validates: retain framing. */
            s->l1.mpt1327.competing_valid = 0;
            if (s->l1.mpt1327.deferred_valid) {
                mpt1327_emit(s, s->l1.mpt1327.deferred_cw);
                s->l1.mpt1327.deferred_valid = 0;
            }
        }
        if (corrected < 0) {
            int offset;

            s->l1.mpt1327.count = -1;
            s->l1.mpt1327.tentative_alignment = 0;
            s->l1.mpt1327.alignment_confirmed = 0;
            s->l1.mpt1327.aligned_words = 0;
            /* A later exact marker may be fully contained in the rejected
             * word. Try the earliest first; a further failure can then
             * reconsider subsequent markers. Partial markers do not count. */
            for (offset = 48; offset > 0; --offset) {
                unsigned int marker = (unsigned int)
                    ((s->l1.mpt1327.shreg >> offset) & 0xFFFFu);
                if (marker == SYNC || marker == SYNT) {
                    s->l1.mpt1327.count = offset;
                    s->l1.mpt1327.tentative_alignment = 1;
                    break;
                }
            }
        } else {
            s->l1.mpt1327.count = 0;
            if (s->l1.mpt1327.aligned_words < 2)
                s->l1.mpt1327.aligned_words++;
            if (s->l1.mpt1327.aligned_words == 2)
                s->l1.mpt1327.alignment_confirmed = 1;
            s->l1.mpt1327.valid_age = 0;
            s->l1.mpt1327.last_word_corrections = (unsigned int)corrected;
            if (s->l1.mpt1327.tentative_alignment ||
                s->l1.mpt1327.sync_age < 64) {
                /* Hold at most 64 bits to check alternatives. On timeout
                 * (or EOF), emit even if no second word validated: a lone
                 * genuine address must not disappear. */
                s->l1.mpt1327.deferred_cw = cw;
                s->l1.mpt1327.deferred_delay = s->l1.mpt1327.tentative_alignment ?
                                              64 : 64 - s->l1.mpt1327.sync_age;
                s->l1.mpt1327.deferred_age = 0;
                s->l1.mpt1327.deferred_valid = 1;
            } else {
                mpt1327_emit(s, cw);
            }
            s->l1.mpt1327.tentative_alignment = 0;
        }
    }
    if (s->l1.mpt1327.count < 0 && (sync == SYNC || sync == SYNT)) {
        s->l1.mpt1327.count = 0;
        s->l1.mpt1327.tentative_alignment = 1;
        s->l1.mpt1327.alignment_confirmed = 0;
        s->l1.mpt1327.aligned_words = 0;
    }
}

/* ---------------------------------------------------------------------- */

static void mpt1327_init(struct demod_state *s)
{
    float f;
    int i;

    memset(&s->l1.mpt1327, 0, sizeof(s->l1.mpt1327));
    s->l1.mpt1327.count = -1;
    s->l1.mpt1327.sync_age = 64;
    s->l1.mpt1327.valid_age = 65;
    s->l1.mpt1327.sys = -1;
    s->l1.mpt1327.sys_sync = 0;
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

/* EOF arbitration uses the same correction-count ordering as live framing:
 * a clean new address beats a corrected old word; equal confidence keeps the
 * established alignment. Without more bits, an equal-strength real boundary
 * can still be missed. */
static void mpt1327_deinit(struct demod_state *s)
{
    if (s->l1.mpt1327.competing_valid &&
        s->l1.mpt1327.deferred_valid &&
        s->l1.mpt1327.competing_corrections <
            s->l1.mpt1327.last_word_corrections) {
        mpt1327_accept_competing(s);
        return;
    }
    s->l1.mpt1327.competing_valid = 0;
    if (s->l1.mpt1327.deferred_valid) {
        mpt1327_emit(s, s->l1.mpt1327.deferred_cw);
        s->l1.mpt1327.deferred_valid = 0;
    }
}

/* ---------------------------------------------------------------------- */

/* FFSK demodulator of demod_fmsfsk.c */
static void mpt1327_demod(struct demod_state *s, buffer_t buffer, int length)
{
    float f;

    if (s->l1.mpt1327.subsamp) {
        if (length <= (int)s->l1.mpt1327.subsamp) {
            s->l1.mpt1327.subsamp -= length;
            return;
        }
        buffer.fbuffer += s->l1.mpt1327.subsamp;
        length -= s->l1.mpt1327.subsamp;
        s->l1.mpt1327.subsamp = 0;
    }
    for (; length > 0; length -= SUBSAMP, buffer.fbuffer += SUBSAMP) {
        f = fsqr(mac(buffer.fbuffer, corr_1_i, CORRLEN)) +
            fsqr(mac(buffer.fbuffer, corr_1_q, CORRLEN)) -
            fsqr(mac(buffer.fbuffer, corr_0_i, CORRLEN)) -
            fsqr(mac(buffer.fbuffer, corr_0_q, CORRLEN));
        s->l1.mpt1327.dcd_shreg <<= 1;
        s->l1.mpt1327.dcd_shreg |= (f > 0);
        /*
         * check if transition
         */
        if ((s->l1.mpt1327.dcd_shreg ^ (s->l1.mpt1327.dcd_shreg >> 1)) & 1) {
            if (s->l1.mpt1327.sphase < (0x8000u-(SPHASEINC/2)))
                s->l1.mpt1327.sphase += SPHASEINC/8;
            else
                s->l1.mpt1327.sphase -= SPHASEINC/8;
        }
        s->l1.mpt1327.sphase += SPHASEINC;
        if (s->l1.mpt1327.sphase >= 0x10000u) {
            s->l1.mpt1327.sphase &= 0xffffu;
            mpt1327_rxbit(s, s->l1.mpt1327.dcd_shreg & 1);
        }
    }
    s->l1.mpt1327.subsamp = -length;
}

/* ---------------------------------------------------------------------- */

const struct demod_param demod_mpt1327 = {
    "MPT1327", true, FREQ_SAMP, CORRLEN, mpt1327_init, mpt1327_demod, mpt1327_deinit
};

/* ---------------------------------------------------------------------- */
