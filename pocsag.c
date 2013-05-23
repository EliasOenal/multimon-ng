/*
 *      pocsag.c -- POCSAG protocol decoder
 *
 *      Copyright (C) 1996
 *          Thomas Sailer (sailer@ife.ee.ethz.ch, hb9jnx@hb9w.che.eu)
 *
 *      Copyright (C) 2012
 *          Elias Oenal    (EliasOenal@gmail.com)
 *
 *      POCSAG (Post Office Code Standard Advisory Group)
 *      Radio Paging Decoder
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* ---------------------------------------------------------------------- */

#include "multimon.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------- */

//#define CHARSET_LATIN1
//#define CHARSET_UTF8 //ÄÖÜäöüß

/* ---------------------------------------------------------------------- */



/*
 * some codewords with special POCSAG meaning
 */
#define POCSAG_SYNC     0x7cd215d8
#define POCSAG_IDLE     0x7a89c197
#define POCSAG_SYNCINFO 0x7cf21436 // what is this value?

#define POCSAG_SYNC_WORDS ((2000000 >> 3) << 13)

#define POCSAG_MESSAGE_DETECTION 0x80000000 // Most significant bit is a one

#define POSCAG
/* ---------------------------------------------------------------------- */

static unsigned char service_mask = 0x87;
int pocsag_mode = 0;
uint32_t pocsag_total_error_count = 0;
uint32_t pocsag_corrected_error_count = 0;
uint32_t pocsag_corrected_1bit_error_count = 0;
uint32_t pocsag_corrected_2bit_error_count = 0;
uint32_t pocsag_uncorrected_error_count = 0;
uint32_t pocsag_total_bits_received = 0;
uint32_t pocsag_bits_processed_while_synced = 0;
uint32_t pocsag_bits_processed_while_not_synced = 0;

/* ---------------------------------------------------------------------- */

void lost_sync(struct l2_pocsag_rx *rx);

/* ---------------------------------------------------------------------- */


static inline unsigned char even_parity(uint32_t data)
{
    unsigned int temp = data ^ (data >> 16);

    temp = temp ^ (temp >> 8);
    temp = temp ^ (temp >> 4);
    temp = temp ^ (temp >> 2);
    temp = temp ^ (temp >> 1);
    return temp & 1;
}

/* ---------------------------------------------------------------------- */

/*
 * the code used by POCSAG is a (n=31,k=21) BCH Code with dmin=5,
 * thus it could correct two bit errors in a 31-Bit codeword.
 * It is a systematic code.
 * The generator polynomial is:
 *   g(x) = x^10+x^9+x^8+x^6+x^5+x^3+1
 * The parity check polynomial is:
 *   h(x) = x^21+x^20+x^18+x^16+x^14+x^13+x^12+x^11+x^8+x^5+x^3+1
 * g(x) * h(x) = x^n+1
 */
#define BCH_POLY 03551 /* octal */
#define BCH_N    31
#define BCH_K    21

static uint32_t pocsag_code(uint32_t data)
{
    uint32_t ret = data << (BCH_N-BCH_K), shreg = ret;
    uint32_t mask = 1L << (BCH_N-1), coeff = BCH_POLY << (BCH_K-1);
    int n = BCH_K;

    for(; n > 0; mask >>= 1, coeff >>= 1, n--)
        if (shreg & mask)
            shreg ^= coeff;
    ret ^= shreg;
    ret = (ret << 1) | even_parity(ret);
    verbprintf(9, "BCH coder: data: %08lx shreg: %08lx ret: %08lx\n",
               data, shreg, ret);
    return ret;
}

/* ---------------------------------------------------------------------- */

static unsigned int pocsag_syndrome(uint32_t data)
{
    uint32_t shreg = data >> 1; /* throw away parity bit */
    uint32_t mask = 1L << (BCH_N-1), coeff = BCH_POLY << (BCH_K-1);
    int n = BCH_K;

    for(; n > 0; mask >>= 1, coeff >>= 1, n--)
        if (shreg & mask)
            shreg ^= coeff;
    if (even_parity(data))
        shreg |= (1 << (BCH_N - BCH_K));
    verbprintf(9, "BCH syndrome: data: %08lx syn: %08lx\n", data, shreg);
    return shreg;
}

/* ---------------------------------------------------------------------- */

static void print_msg_numeric(struct l2_pocsag_rx *rx)
{
    static const char *conv_table = "084 2.6]195-3U7[";
    unsigned char *bp = rx->buffer;
    int len = rx->numnibbles;
    char buf[512], *cp = buf;

    if (len >= sizeof(buf))
        len = sizeof(buf)-1;
    for (; len > 0; bp++, len -= 2) {
        *cp++ = conv_table[(*bp >> 4) & 0xf];
        if (len > 1)
            *cp++ = conv_table[*bp & 0xf];
    }
    *cp = '\0';
    verbprintf(-3, "%s\n", buf);
}

/* ---------------------------------------------------------------------- */

static char *translate_alpha(unsigned char chr)
{
    static const struct trtab {
        unsigned char code;
        char *str;
    } trtab[] = {{  0, "<NUL>" },
                 {  1, "<SOH>" },
                 {  2, "<STX>" },
                 {  3, "<ETX>" },
                 {  4, "<EOT>" },
                 {  5, "<ENQ>" },
                 {  6, "<ACK>" },
                 {  7, "<BEL>" },
                 {  8, "<BS>" },
                 {  9, "<HT>" },
                 { 10, "<LF>" },
                 { 11, "<VT>" },
                 { 12, "<FF>" },
                 { 13, "<CR>" },
                 { 14, "<SO>" },
                 { 15, "<SI>" },
                 { 16, "<DLE>" },
                 { 17, "<DC1>" },
                 { 18, "<DC2>" },
                 { 19, "<DC3>" },
                 { 20, "<DC4>" },
                 { 21, "<NAK>" },
                 { 22, "<SYN>" },
                 { 23, "<ETB>" },
                 { 24, "<CAN>" },
                 { 25, "<EM>" },
                 { 26, "<SUB>" },
                 { 27, "<ESC>" },
                 { 28, "<FS>" },
                 { 29, "<GS>" },
                 { 30, "<RS>" },
                 { 31, "<US>" },
#ifdef CHARSET_LATIN1
                 { 0x5b, "\304" }, /* upper case A dieresis */
                 { 0x5c, "\326" }, /* upper case O dieresis */
                 { 0x5d, "\334" }, /* upper case U dieresis */
                 { 0x7b, "\344" }, /* lower case a dieresis */
                 { 0x7c, "\366" }, /* lower case o dieresis */
                 { 0x7d, "\374" }, /* lower case u dieresis */
                 { 0x7e, "\337" }}; /* sharp s */
#elif defined CHARSET_UTF8
                 { 0x5b, "Ä" }, /* upper case A dieresis */
                 { 0x5c, "Ö" }, /* upper case O dieresis */
                 { 0x5d, "Ü" }, /* upper case U dieresis */
                 { 0x7b, "ä" }, /* lower case a dieresis */
                 { 0x7c, "ö" }, /* lower case o dieresis */
                 { 0x7d, "ü" }, /* lower case u dieresis */
                 { 0x7e, "ß" }}; /* sharp s */
#else
                 { 0x5b, "AE" }, /* upper case A dieresis */
                 { 0x5c, "OE" }, /* upper case O dieresis */
                 { 0x5d, "UE" }, /* upper case U dieresis */
                 { 0x7b, "ae" }, /* lower case a dieresis */
                 { 0x7c, "oe" }, /* lower case o dieresis */
                 { 0x7d, "ue" }, /* lower case u dieresis */
                 { 0x7e, "ss" }}; /* sharp s */
#endif
    int min = 0, max = (sizeof(trtab) / sizeof(trtab[0])) - 1;

    /*
     * binary search, list must be ordered!
     */
    for (;;) {
        int mid = (min+max) >> 1;
        const struct trtab *tb = trtab + mid;
        int cmp = ((int) tb->code) - ((int) chr);

        if (!cmp)
            return tb->str;
        if (cmp < 0) {
            min = mid+1;
            if (min > max)
                return NULL;
        }
        if (cmp > 0) {
            max = mid-1;
            if (max < min)
                return NULL;
        }
    }
}

/* ---------------------------------------------------------------------- */

static void print_msg_alpha(struct l2_pocsag_rx *rx)
{
    uint32_t data = 0;
    int datalen = 0;
    unsigned char *bp = rx->buffer;
    int len = rx->numnibbles;
    char buf[512], *cp = buf;
    int buffree = sizeof(buf)-1;
    unsigned char curchr;
    char *tstr;

    while (len > 0) {
        while (datalen < 7 && len > 0) {
            if (len == 1) {
                data = (data << 4) | ((*bp >> 4) & 0xf);
                datalen += 4;
                len = 0;
            } else {
                data = (data << 8) | *bp++;
                datalen += 8;
                len -= 2;
            }
        }
        if (datalen < 7)
            continue;
        datalen -= 7;
        curchr = ((data >> datalen) & 0x7f) << 1;
        curchr = ((curchr & 0xf0) >> 4) | ((curchr & 0x0f) << 4);
        curchr = ((curchr & 0xcc) >> 2) | ((curchr & 0x33) << 2);
        curchr = ((curchr & 0xaa) >> 1) | ((curchr & 0x55) << 1);
        tstr = translate_alpha(curchr);
        if (tstr) {
            int tlen = strlen(tstr);
            if (buffree >= tlen) {
                memcpy(cp, tstr, tlen);
                cp += tlen;
                buffree -= tlen;
            }
        } else if (buffree > 0) {
            *cp++ = curchr;
            buffree--;
        }
    }
    *cp = '\0';
    verbprintf(-3, "%s\n", buf);
}

/* ---------------------------------------------------------------------- */

static void print_msg_skyper(struct l2_pocsag_rx *rx)
{
    uint32_t data = 0;
    int datalen = 0;
    unsigned char *bp = rx->buffer;
    int len = rx->numnibbles;
    char buf[512], *cp = buf;
    int buffree = sizeof(buf)-1;
    unsigned char curchr;
    char *tstr;

    while (len > 0) {
        while (datalen < 7 && len > 0) {
            if (len == 1) {
                data = (data << 4) | ((*bp >> 4) & 0xf);
                datalen += 4;
                len = 0;
            } else {
                data = (data << 8) | *bp++;
                datalen += 8;
                len -= 2;
            }
        }
        if (datalen < 7)
            continue;
        datalen -= 7;
        curchr = ((data >> datalen) & 0x7f) << 1;
        curchr = ((curchr & 0xf0) >> 4) | ((curchr & 0x0f) << 4);
        curchr = ((curchr & 0xcc) >> 2) | ((curchr & 0x33) << 2);
        curchr = ((curchr & 0xaa) >> 1) | ((curchr & 0x55) << 1);
        tstr = translate_alpha(curchr-1);
        if (tstr) {
            int tlen = strlen(tstr);
            if (buffree >= tlen) {
                memcpy(cp, tstr, tlen);
                cp += tlen;
                buffree -= tlen;
            }
        } else if (buffree > 0) {
            *cp++ = curchr-1;
            buffree--;
        }
    }
    *cp = '\0';
    verbprintf(-3, "%s\n", buf);
}

/* ---------------------------------------------------------------------- */

static void pocsag_printmessage(struct demod_state *s, struct l2_pocsag_rx *rx,
                                const char *add_name)
{
    verbprintf(-2, "%s%s: Address: %7lu  Function: %1u\n",
               s->dem_par->name, add_name, rx->adr, rx->func);
    if (!rx->numnibbles)
        return;

    if (((service_mask & (0x01 << rx->func)) || (pocsag_mode == POCSAG_MODE_NUMERIC))) {
        verbprintf(-1, "%s%s: Numeric: ", s->dem_par->name, add_name);
        print_msg_numeric(rx);
    }

    if ((service_mask & (0x10 << rx->func)) || (pocsag_mode == POCSAG_MODE_ALPHA) || (pocsag_mode == POCSAG_MODE_SKYPER))
    {
        if (((rx->func == 3) && (rx->adr >= 4000) && (rx->adr <= 5000))
                || (pocsag_mode == POCSAG_MODE_SKYPER))
        {
            verbprintf(-1, "%s%s: Alpha (SKYPER): ", s->dem_par->name, add_name);
            print_msg_skyper(rx);
        }

        if (!((rx->func == 3) && (rx->adr >= 4000) && (rx->adr <= 5000)) || (pocsag_mode == POCSAG_MODE_ALPHA))
        {
            verbprintf(-1, "%s%s: Alpha: ", s->dem_par->name, add_name);
            print_msg_alpha(rx);
        }
    }
}

/* ---------------------------------------------------------------------- */

void pocsag_init(struct demod_state *s)
{
    memset(&s->l2.pocsag, 0, sizeof(s->l2.pocsag));
}

void pocsag_deinit(struct demod_state *s)
{
    if(pocsag_total_error_count)
        verbprintf(1, "\n===POCSAG stats===\n"
                   "Words BCH checked: %u\n"
                   "Corrected errors: %u\n"
                   "Corrected 1bit errors: %u\n"
                   "Corrected 2bit errors: %u\n"
                   "Invalid word or >2 bits errors: %u\n\n"
                   "Total bits processed: %u\n"
                   "Bits processed while in sync: %u\n"
                   "Bits processed while out of sync: %u\n"
                   "Percentage of successfully decoded bits: %f\n"
                   "(A 50 percent error rate is normal since we always\n"
                   "try to decode the inverted input signal as well)\n",
                   pocsag_total_error_count,
                   pocsag_corrected_error_count,
                   pocsag_corrected_1bit_error_count,
                   pocsag_corrected_2bit_error_count,
                   pocsag_uncorrected_error_count,
                   pocsag_total_bits_received,
                   pocsag_bits_processed_while_synced,
                   pocsag_bits_processed_while_not_synced,
                   (100./pocsag_total_bits_received)*pocsag_bits_processed_while_synced);
    fflush(stdout);
}

static inline void
transpose32(uint32_t *A) {
    int j, k;
    uint32_t m, t;

    m = 0x0000FFFF;
    for (j = 16; j != 0; j = j >> 1, m = m ^ (m << j)) {
        for (k = 0; k < 32; k = (k + j + 1) & ~j) {
            t = (A[k] ^ (A[k+j] >> j)) & m;
            A[k] = A[k] ^ t;
            A[k+j] = A[k+j] ^ (t << j);
        }
    }
}

static uint32_t *
transpose(uint32_t *matrix)
{
    int i, j;
    uint32_t *out = malloc(sizeof(uint32_t)*32);
    memcpy(out, matrix, sizeof(uint32_t)*32);
    transpose32(out);
    return out;
}

static uint32_t
transpose_n(int n, uint32_t *matrix)
{
    uint32_t out = 0;
    int j;

    for (j = 0; j < 32; ++j) {
        if (matrix[j] & (1<<n))
            out |= (1<<j);
    }

    return out;
}

#define ONE 0xffffffff

static uint32_t *
transpose_clone(uint32_t src, uint32_t *out)
{
    int i, j;
    if (!out)
        out = malloc(sizeof(uint32_t)*32);

    for (i = 0; i < 32; ++i) {
        if (src & (1<<i))
            out[i] = ONE;
        else
            out[i] = 0;
    }

    return out;
}

static void
bitslice_syndrome(uint32_t *slices)
{
    const int firstBit = BCH_N - 1;
    int i, n;
    uint32_t paritymask = slices[0];

    // do the parity and shift together
    for (i = 1; i < 32; ++i) {
        paritymask ^= slices[i];
        slices[i-1] = slices[i];
    }
    slices[31] = 0;

    // BCH_POLY << (BCH_K - 1) is
    //                                                              20   21 22 23
    //  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ONE, 0, 0, ONE,
    //  24 25   26  27  28   29   30   31
    //  0, ONE, ONE, 0, ONE, ONE, ONE, 0

    for (n = 0; n < BCH_K; ++n) {
        // one line here for every '1' bit in coeff (above)
        const int bit = firstBit - n;
        slices[20 - n] ^= slices[bit];
        slices[23 - n] ^= slices[bit];
        slices[25 - n] ^= slices[bit];
        slices[26 - n] ^= slices[bit];
        slices[28 - n] ^= slices[bit];
        slices[29 - n] ^= slices[bit];
        slices[30 - n] ^= slices[bit];
        slices[31 - n] ^= slices[bit];
    }

    // apply the parity mask we built up
    slices[BCH_N - BCH_K] |= paritymask;
}

/* ---------------------------------------------------------------------- */

enum{
    NO_SYNC = 0,
    GOT_SYNC = 1,
};

enum{
    MESSAGE_CLASS_INVALID = -1,
    MESSAGE_CLASS_NUMERIC = 0,
    MESSAGE_CLASS_TEXT = 3,
};

// This might not be elegant, yet effective!
// Error correction via bruteforce ;)
//
// It's a pragmatic solution since this was much faster to implement
// than understanding the math to solve it while being as effective.
// Besides that the overhead is neglectable.
int pocsag_brute_repair(uint32_t* data)
{
    if (pocsag_syndrome(*data)) {
        pocsag_total_error_count++;
        verbprintf(5, "Error in syndrome detected!\n");
    } else {
        return 0;
    }

    // check for single bit errors
    {
        int i, n, b1, b2;
        uint32_t res;
        uint32_t *xpose = 0, *in = 0;

        xpose = malloc(sizeof(uint32_t)*32);
        in = malloc(sizeof(uint32_t)*32);

        transpose_clone(*data, xpose);
        for (i = 0; i < 32; ++i)
            xpose[i] ^= (1<<i);

        bitslice_syndrome(xpose);

        res = 0;
        for (i = 0; i < 32; ++i)
            res |= xpose[i];
        res = ~res;

        if (res) {
            int n = 0;
            while (res) {
                ++n;
                res >>= 1;
            }
            --n;

            *data ^= (1<<n);
            pocsag_corrected_error_count++;
            pocsag_corrected_1bit_error_count++;
            goto returnfree;
        }

        //check for two bit errors
        n = 0;
        transpose_clone(*data, xpose);

        for (b1 = 0; b1 < 32; ++b1) {
            for (b2 = b1; b2 < 32; ++b2) {
                xpose[b1] ^= (1<<n);
                xpose[b2] ^= (1<<n);

                if (++n == 32) {
                    memcpy(in, xpose, sizeof(uint32_t)*32);

                    bitslice_syndrome(xpose);

                    res = 0;
                    for (i = 0; i < 32; ++i)
                        res |= xpose[i];
                    res = ~res;

                    if (res) {
                        int n = 0;
                        while (res) {
                            ++n;
                            res >>= 1;
                        }
                        --n;

                        *data = transpose_n(n, in);
                        pocsag_corrected_error_count++;
                        pocsag_corrected_2bit_error_count++;
                        goto returnfree;
                    }

                    transpose_clone(*data, xpose);
                    n = 0;
                }
            }
        }

        if (n > 0) {
            memcpy(in, xpose, sizeof(uint32_t)*32);

            bitslice_syndrome(xpose);

            res = 0;
            for (i = 0; i < 32; ++i)
                res |= xpose[i];
            res = ~res;

            if (res) {
                int n = 0;
                while (res) {
                    ++n;
                    res >>= 1;
                }
                --n;

                *data = transpose_n(n, in);
                pocsag_corrected_error_count++;
                pocsag_corrected_2bit_error_count++;
                goto returnfree;
            }
        }

        pocsag_uncorrected_error_count++;
        verbprintf(5, "Couldn't correct error!\n");
        if (xpose)
            free(xpose);
        if (in)
            free(in);
        return 1;

returnfree:
        if (xpose)
            free(xpose);
        if (in)
            free(in);
        return 0;
    }
}

static void do_one_bit(struct demod_state *s, struct l2_pocsag_rx *rx,
                       uint32_t rx_data, const char *add_name)
{
    pocsag_total_bits_received++;

    if(!(pocsag_total_bits_received % 10000))
        verbprintf(2, "Bits received: %u\n", pocsag_total_bits_received);

    // Search for Sync
    if (!rx->rx_sync)
    {
        pocsag_brute_repair(&rx_data); // try to repair sync code

        if (rx_data == POCSAG_SYNC) // Sync found!
        {
            verbprintf(2, "Aquired sync!\n");
            rx->rx_sync = GOT_SYNC;
            rx->rx_bit = 0;
            rx->rx_word = 0;
            rx->func = MESSAGE_CLASS_INVALID;
            pocsag_bits_processed_while_synced++;
        }
        else
        {
            pocsag_bits_processed_while_not_synced++;
        }
        return;
    }
    else
    {
        pocsag_bits_processed_while_synced++;
    }


    // Do nothing for 31 bits
    // When the word is complete let the program counter pass
    if(rx->rx_bit = ++(rx->rx_bit) % 32)
        return;


    // We're in sync, now check the incoming data
    if(pocsag_brute_repair(&rx_data))
    {
        // Invalid data, we lost sync
        verbprintf(2, "Lost sync due to corrupted data!\n");
        lost_sync(rx);
        return;
    }

    // it is always 17 words
    unsigned char rxword = rx->rx_word; // for address calculation, looks fishy
    rx->rx_word = ++(rx->rx_word) % 17;

    if(!rx->rx_word)
    {
        if(rx_data == POCSAG_SYNC)
        {
            verbprintf(3, "Re-Synced as expected!\n");
        }
        else
        {
            verbprintf(2, "Lost sync, this (0x%x) should have been a re-sync, but wasn't!\n", rx_data);
            lost_sync(rx);
            return;
        }
        return;
    }


    if(rx_data & POCSAG_MESSAGE_DETECTION)
    {
        // It's a message
        verbprintf(3, "Message!\n");

        if(rx->func == MESSAGE_CLASS_INVALID)
        {
            verbprintf(2, "We didn't get the header for this message and thus drop it!\n");
        }

        if (rx->numnibbles > sizeof(rx->buffer)*2 - 5) {
            verbprintf(-1, "%s%s: Warning: Message too long\n",
                       s->dem_par->name, add_name);
            pocsag_printmessage(s, rx, add_name);
            rx->func = MESSAGE_CLASS_INVALID;
            return;
        }

        uint32_t data;
        unsigned char *bp;
        bp = rx->buffer + (rx->numnibbles >> 1);
        data = rx_data >> 11;
        if (rx->numnibbles & 1) {
            bp[0] = (bp[0] & 0xf0) | ((data >> 16) & 0xf);
            bp[1] = data >> 8;
            bp[2] = data;
        } else {
            bp[0] = data >> 12;
            bp[1] = data >> 4;
            bp[2] = data << 4;
        }
        rx->numnibbles += 5;
        verbprintf(3, "We received something!\n");
        return;
    }
    else
    {
        // It's an address

        // Idle messages are a special case since they
        // reside inside the address prefix
        // Idle on idle message :D
        if(rx_data == POCSAG_IDLE)
        {
            verbprintf(3, "Idling!\n");

            // Seems like the transmission is over
            // and we can output now
            if(rx->numnibbles)
            {
                verbprintf(3, "Printing message, received %u nibbles.\n", rx->numnibbles);
                pocsag_printmessage(s, rx, add_name);
                rx->numnibbles = 0;
            }
            return;
        }

        // Well sync messages are special as well
        if (rx_data == POCSAG_SYNC) // Sync found!
        {
            verbprintf(2, "Unexpected re-sync!\n");
            rx->rx_sync = GOT_SYNC;
            rx->rx_bit = 0;
            rx->rx_word = 0;
            rx->func = MESSAGE_CLASS_INVALID;
            // We're already in sync O.o
            return;
        }

        verbprintf(3, "Address!\n");


        // new message incoming, output the old one first
        if(rx->numnibbles)
        {
            pocsag_printmessage(s, rx, add_name);
            rx->numnibbles = 0;
        }
        rx->func = (rx_data >> 11) & 3;
        rx->adr = ((rx_data >> 10) & 0x1ffff8) | ((rxword >> 1) & 7); // no idea what this does
        rx->numnibbles = 0;
        verbprintf(3, "Message class: %u Address: %u\n", rx->func, rx->adr);
    }
}

void lost_sync(struct l2_pocsag_rx *rx)
{
    rx->rx_sync = NO_SYNC;
    rx->rx_bit = 0;
    rx->rx_word = 0;
    rx->numnibbles = 0;
    rx->func = MESSAGE_CLASS_INVALID;
    memset(rx->buffer, 0, sizeof(rx->buffer));
    return;
}


//static void do_one_bit(struct demod_state *s, struct l2_pocsag_rx *rx,
//                       uint32_t rx_data, const char *add_name)
//{
//    unsigned char rxword;

//    if (!rx->rx_sync)
//    {
//        if (rx_data == POCSAG_SYNC || rx_data == POCSAG_SYNCINFO)
//        {
//            rx->rx_sync = 2;
//            rx->rx_bit = rx->rx_word = 0;
//            rx->func = -1;
//            return;
//        }
//        return;
//    }



//    if (rx_data == POCSAG_IDLE) {
//        /*
//         * it seems that we can output the message right here
//         */
//        if (!(rx->func & (~3)))
//            pocsag_printmessage(s, rx, add_name);
//        rx->func = -1; /* invalidate message */
//        return;
//    }

//    /*
//     * one complete word received
//     */
//    if ((++(rx->rx_bit)) < 32)
//        return;

//    rx->rx_bit = 0;


//    /*
//     * check codeword
//     */
//    if (pocsag_syndrome(rx_data))
//    {
//        /*
//         * codeword not valid
//         */
//        rx->rx_sync--;
//        verbprintf(7, "%s: Bad codeword: %08lx%s\n",
//                   s->dem_par->name, rx_data,
//                   rx->rx_sync ? "" : "sync lost");
//        if (!(rx->func & (~3))) {
//            verbprintf(0, "%s%s: Warning: message garbled\n",
//                       s->dem_par->name, add_name);
//            pocsag_printmessage(s, rx, add_name);
//        }
//        rx->func = -1; /* invalidate message */
//        return;
//    }

//    /* do something with the data */
//    verbprintf(8, "%s%s: Codeword: %08lx\n", s->dem_par->name, add_name, rx_data);
//    rxword = rx->rx_word++;
//    if (rxword >= 16) {
//        /*
//         * received word shoud be a
//         * frame synch
//         */
//        rx->rx_word = 0;
//        if ((rx_data == POCSAG_SYNC) ||
//                (rx_data == POCSAG_SYNCINFO))
//            rx->rx_sync = 10;
//        else
//            rx->rx_sync -= 2;
//        return;
//    }

//    if (rx_data & 0x80000000) {
//        /*
//         * this is a data word
//         */
//        uint32_t data;
//        unsigned char *bp;

//        if (rx->func & (~3)) {
//            /*
//             * no message being received
//             */
//            verbprintf(7, "%s%s: Lonesome data codeword: %08lx\n",
//                       s->dem_par->name, add_name, rx_data);
//            return;
//        }
//        if (rx->numnibbles > sizeof(rx->buffer)*2 - 5) {
//            verbprintf(0, "%s%s: Warning: Message too long\n",
//                       s->dem_par->name, add_name);
//            pocsag_printmessage(s, rx, add_name);
//            rx->func = -1;
//            return;
//        }
//        bp = rx->buffer + (rx->numnibbles >> 1);
//        data = rx_data >> 11;
//        if (rx->numnibbles & 1) {
//            bp[0] = (bp[0] & 0xf0) | ((data >> 16) & 0xf);
//            bp[1] = data >> 8;
//            bp[2] = data;
//        } else {
//            bp[0] = data >> 12;
//            bp[1] = data >> 4;
//            bp[2] = data << 4;
//        }
//        rx->numnibbles += 5;
//        return;
//    }

//    /*
//     * process address codeword
//     */
//    if (rx_data >= POCSAG_SYNC_WORDS)
//    {
//        unsigned char func = (rx_data >> 11) & 3;
//        uint32_t adr = ((rx_data >> 10) & 0x1ffff8) |
//                ((rxword >> 1) & 7);

//        verbprintf(0, "%s%s: Nonstandard address codeword: %08lx "
//                   "func %1u adr %08lx\n", s->dem_par->name, add_name, rx_data,
//                   func, adr);
//        return;
//    }

//    if (!(rx->func & (~3)))
//        pocsag_printmessage(s, rx, add_name);
//    rx->func = (rx_data >> 11) & 3;
//    rx->adr = ((rx_data >> 10) & 0x1ffff8) | ((rxword >> 1) & 7);
//    rx->numnibbles = 0;
//}

/* ---------------------------------------------------------------------- */

void pocsag_rxbit(struct demod_state *s, int32_t bit)
{
    s->l2.pocsag.rx_data <<= 1;
    s->l2.pocsag.rx_data |= !bit;
    verbprintf(9, " %c ", '1'-(s->l2.pocsag.rx_data & 1));
    do_one_bit(s, s->l2.pocsag.rx, ~(s->l2.pocsag.rx_data), "+"); // this seems to feed an inverted signal
    // just to be sure I guess :/
    do_one_bit(s, s->l2.pocsag.rx+1, s->l2.pocsag.rx_data, "-");
}

/* ---------------------------------------------------------------------- */
