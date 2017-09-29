/*
 *      pocsag.c -- POCSAG protocol decoder
 *
 *      Copyright (C) 1996
 *          Thomas Sailer (sailer@ife.ee.ethz.ch, hb9jnx@hb9w.che.eu)
 *
 *      Copyright (C) 2012-2014
 *          Elias Oenal    (multimon-ng@eliasoenal.com)
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
#include <stdbool.h>

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

int pocsag_mode = 0;
int pocsag_invert_input = 0;
int pocsag_error_correction = 2;
int pocsag_show_partial_decodes = 0;
int pocsag_heuristic_pruning = 0;
int pocsag_prune_empty = 0;

/* ---------------------------------------------------------------------- */


enum states{
    NO_SYNC = 0,            //0b00000000
    SYNC = 64,              //0b10000000
    LOSING_SYNC = 65,       //0b10000001
    LOST_SYNC = 66,         //0b10000010
    ADDRESS = 67,           //0b10000011
    MESSAGE = 68,           //0b10000100
    END_OF_MESSAGE = 69,    //0b10000101
};


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
                 { 0x7e, "\337" }, /* sharp s */
             #elif defined CHARSET_UTF8
                 { 0x5b, "Ä" }, /* upper case A dieresis */
                 { 0x5c, "Ö" }, /* upper case O dieresis */
                 { 0x5d, "Ü" }, /* upper case U dieresis */
                 { 0x7b, "ä" }, /* lower case a dieresis */
                 { 0x7c, "ö" }, /* lower case o dieresis */
                 { 0x7d, "ü" }, /* lower case u dieresis */
                 { 0x7e, "ß" }, /* sharp s */
             #else
                 { 0x5b, "AE" }, /* upper case A dieresis */
                 { 0x5c, "OE" }, /* upper case O dieresis */
                 { 0x5d, "UE" }, /* upper case U dieresis */
                 { 0x7b, "ae" }, /* lower case a dieresis */
                 { 0x7c, "oe" }, /* lower case o dieresis */
                 { 0x7d, "ue" }, /* lower case u dieresis */
                 { 0x7e, "ss" }, /* sharp s */
             #endif
                 { 127, "<DEL>" }};

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
static int guesstimate_alpha(const unsigned char cp)
{
    if(cp < 32 || cp == 127)
        return -5; // Non printable characters are uncommon
    else if((cp > 32 && cp < 48)
            || (cp > 57 && cp < 65)
            || (cp > 90 && cp < 97)
            || (cp > 122 && cp < 127))
        return -2; // Penalize special characters
    else
        return 1;
}

static int guesstimate_numeric(const unsigned char cp, int pos)
{
    if(cp == 'U')
        return -10;
    else if(cp == '[' || cp == ']')
        return -5;
    else if(cp == ' ' || cp == '.' || cp == '-')
        return -2;
    else if(pos < 10) // Penalize long messages
        return 5;
    else
        return 0;
}

static unsigned int print_msg_numeric(struct l2_state_pocsag *rx, char* buff, unsigned int size)
{
    static const char *conv_table = "084 2.6]195-3U7[";
    unsigned char *bp = rx->buffer;
    int len = rx->numnibbles;
    char* cp = buff;
    unsigned int guesstimate = 0;

    if ( (unsigned int) len >= size)
        len = size-1;
    for (; len > 0; bp++, len -= 2) {
        *cp++ = conv_table[(*bp >> 4) & 0xf];
        if (len > 1)
            *cp++ = conv_table[*bp & 0xf];
    }
    *cp = '\0';

    cp = buff;
    for(int i = 0; *(cp+i); i++)
        guesstimate += guesstimate_numeric(*(cp+i), i);
    return guesstimate;
}

static int print_msg_alpha(struct l2_state_pocsag *rx, char* buff, unsigned int size)
{
    uint32_t data = 0;
    int datalen = 0;
    unsigned char *bp = rx->buffer;
    int len = rx->numnibbles;
    char* cp = buff;
    int buffree = size-1;
    unsigned char curchr;
    char *tstr;
    int guesstimate = 0;

    while (len > 0)
    {
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

        guesstimate += guesstimate_alpha(curchr);

        tstr = translate_alpha(curchr);
        if (tstr)
        {
            int tlen = strlen(tstr);
            if (buffree >= tlen)
            {
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

    return guesstimate;
}

/* ---------------------------------------------------------------------- */

static int print_msg_skyper(struct l2_state_pocsag *rx, char* buff, unsigned int size)
{
    uint32_t data = 0;
    int datalen = 0;
    unsigned char *bp = rx->buffer;
    int len = rx->numnibbles;
    char* cp = buff;
    int buffree = size-1;
    unsigned char curchr;
    char *tstr;
    unsigned int guesstimate = 0;

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

        guesstimate += guesstimate_alpha(curchr-1);

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
    return guesstimate;
}

/* ---------------------------------------------------------------------- */

static void pocsag_printmessage(struct demod_state *s, bool sync)
{
    if(!pocsag_show_partial_decodes && ((s->l2.pocsag.address == -2) || (s->l2.pocsag.function == -2) || !sync))
        return; // Hide partial decodes
    if(pocsag_prune_empty && (s->l2.pocsag.numnibbles == 0))
        return;

    if((s->l2.pocsag.address != -1) || (s->l2.pocsag.function != -1))
    {
        if(s->l2.pocsag.numnibbles == 0)
        {
            verbprintf(0, "%s: Address: %7lu  Function: %1hhi ",s->dem_par->name,
                       s->l2.pocsag.address, s->l2.pocsag.function);
            if(!sync) verbprintf(2,"<LOST SYNC>");
            verbprintf(0,"\n");
        }
        else
        {
            char num_string[1024];
            char alpha_string[1024];
            char skyper_string[1024];
            int guess_num = 0;
            int guess_alpha = 0;
            int guess_skyper = 0;
            int unsure = 0;

            guess_num = print_msg_numeric(&s->l2.pocsag, num_string, sizeof(num_string));
            guess_alpha = print_msg_alpha(&s->l2.pocsag, alpha_string, sizeof(alpha_string));
            guess_skyper = print_msg_skyper(&s->l2.pocsag, skyper_string, sizeof(skyper_string));

            if(guess_num < 20 && guess_alpha < 20 && guess_skyper < 20)
            {
                if(pocsag_heuristic_pruning)
                    return;
                unsure = 1;
            }


            if((pocsag_mode == POCSAG_MODE_NUMERIC) || ((pocsag_mode == POCSAG_MODE_AUTO) && (guess_num >= 20 || unsure)))
            {
                if((s->l2.pocsag.address != -2) || (s->l2.pocsag.function != -2))
                    verbprintf(0, "%s: Address: %7lu  Function: %1hhi  ",s->dem_par->name,
                           s->l2.pocsag.address, s->l2.pocsag.function);
                else
                    verbprintf(0, "%s: Address:       -  Function: -  ",s->dem_par->name);
                if(pocsag_mode == POCSAG_MODE_AUTO)
                    verbprintf(3, "Certainty: %5i  ", guess_num);
                verbprintf(0, "Numeric: %s", num_string);
                if(!sync) verbprintf(2,"<LOST SYNC>");
                verbprintf(0,"\n");
            }

            if((pocsag_mode == POCSAG_MODE_ALPHA) || ((pocsag_mode == POCSAG_MODE_AUTO) && (guess_alpha >= 20 || unsure)))
            {
                if((s->l2.pocsag.address != -2) || (s->l2.pocsag.function != -2))
                    verbprintf(0, "%s: Address: %7lu  Function: %1hhi  ",s->dem_par->name,
                           s->l2.pocsag.address, s->l2.pocsag.function);
                else
                    verbprintf(0, "%s: Address:       -  Function: -  ",s->dem_par->name);
                if(pocsag_mode == POCSAG_MODE_AUTO)
                    verbprintf(3, "Certainty: %5i  ", guess_alpha);
                verbprintf(0, "Alpha:   %s", alpha_string);
                if(!sync) verbprintf(2,"<LOST SYNC>");
                verbprintf(0,"\n");
            }

            if((pocsag_mode == POCSAG_MODE_SKYPER) || ((pocsag_mode == POCSAG_MODE_AUTO) && (guess_skyper >= 20 || unsure)))
            {
                if((s->l2.pocsag.address != -2) || (s->l2.pocsag.function != -2))
                    verbprintf(0, "%s: Address: %7lu  Function: %1hhi  ",s->dem_par->name,
                           s->l2.pocsag.address, s->l2.pocsag.function);
                else
                    verbprintf(0, "%s: Address:       -  Function: -  ",s->dem_par->name);
                if(pocsag_mode == POCSAG_MODE_AUTO)
                    verbprintf(3, "Certainty: %5i  ", guess_skyper);
                verbprintf(0, "Skyper:  %s", skyper_string);
                if(!sync) verbprintf(2,"<LOST SYNC>");
                verbprintf(0,"\n");
            }
        }
    }
}

/* ---------------------------------------------------------------------- */

void pocsag_init(struct demod_state *s)
{
    memset(&s->l2.pocsag, 0, sizeof(s->l2.pocsag));
    s->l2.pocsag.address = -1;
    s->l2.pocsag.function = -1;
}

void pocsag_deinit(struct demod_state *s)
{
    if(s->l2.pocsag.pocsag_total_error_count)
        verbprintf(1, "\n===%s stats===\n"
                   "Words BCH checked: %u\n"
                   "Corrected errors: %u\n"
                   "Corrected 1bit errors: %u\n"
                   "Corrected 2bit errors: %u\n"
                   "Invalid word or >2 bits errors: %u\n\n"
                   "Total bits processed: %u\n"
                   "Bits processed while in sync: %u\n"
                   "Bits processed while out of sync: %u\n"
                   "Successfully decoded: %f%%\n",
                   s->dem_par->name,
                   s->l2.pocsag.pocsag_total_error_count,
                   s->l2.pocsag.pocsag_corrected_error_count,
                   s->l2.pocsag.pocsag_corrected_1bit_error_count,
                   s->l2.pocsag.pocsag_corrected_2bit_error_count,
                   s->l2.pocsag.pocsag_uncorrected_error_count,
                   s->l2.pocsag.pocsag_total_bits_received,
                   s->l2.pocsag.pocsag_bits_processed_while_synced,
                   s->l2.pocsag.pocsag_bits_processed_while_not_synced,
                   (100./s->l2.pocsag.pocsag_total_bits_received)*s->l2.pocsag.pocsag_bits_processed_while_synced);
    fflush(stdout);
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
    int i;
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



// This might not be elegant, yet effective!
// Error correction via bruteforce ;)
//
// It's a pragmatic solution since this was much faster to implement
// than understanding the math to solve it while being as effective.
// Besides that the overhead is neglectable.
int pocsag_brute_repair(struct l2_state_pocsag *rx, uint32_t* data)
{
    if (pocsag_syndrome(*data)) {
        rx->pocsag_total_error_count++;
        verbprintf(6, "Error in syndrome detected!\n");
    } else {
        return 0;
    }

    if(pocsag_error_correction == 0)
    {
        rx->pocsag_uncorrected_error_count++;
        verbprintf(6, "Couldn't correct error!\n");
        return 1;
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
            rx->pocsag_corrected_error_count++;
            rx->pocsag_corrected_1bit_error_count++;
            goto returnfree;
        }

        if(pocsag_error_correction == 1)
        {
            rx->pocsag_uncorrected_error_count++;
            verbprintf(6, "Couldn't correct error!\n");
            if (xpose)
                free(xpose);
            if (in)
                free(in);
            return 1;
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
                        rx->pocsag_corrected_error_count++;
                        rx->pocsag_corrected_2bit_error_count++;
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
                rx->pocsag_corrected_error_count++;
                rx->pocsag_corrected_2bit_error_count++;
                goto returnfree;
            }
        }

        rx->pocsag_uncorrected_error_count++;
        verbprintf(6, "Couldn't correct error!\n");
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

static inline bool word_complete(struct demod_state *s)
{    
    // Do nothing for 31 bits
    // When the word is complete let the program counter pass
    s->l2.pocsag.rx_bit = (s->l2.pocsag.rx_bit + 1) % 32;
    return s->l2.pocsag.rx_bit == 0;
}

static inline bool is_sync(const uint32_t * const rx_data)
{
    if(*rx_data == POCSAG_SYNC)
        return true; // Sync found!
    return false;
}

static inline bool is_idle(const uint32_t * const rx_data)
{
    if(*rx_data == POCSAG_IDLE)
        return true; // Idle found!
    return false;
}

static void do_one_bit(struct demod_state *s, uint32_t rx_data)
{
    s->l2.pocsag.pocsag_total_bits_received++;

    switch(s->l2.pocsag.state & SYNC)
    {
    case NO_SYNC:
    {
        s->l2.pocsag.pocsag_bits_processed_while_not_synced++;

        pocsag_brute_repair(&s->l2.pocsag, &rx_data);
        if(is_sync(&rx_data))
        {
            verbprintf(4, "Aquired sync!\n");
            s->l2.pocsag.state = SYNC;
        }
        return;
    }

    case SYNC:
    {
        s->l2.pocsag.pocsag_bits_processed_while_synced++;

        if(!word_complete(s))
            return; // Wait for more bits to arrive.

        // it is always 17 words
        unsigned char rxword = s->l2.pocsag.rx_word; // for address calculation
        s->l2.pocsag.rx_word = (s->l2.pocsag.rx_word + 1) % 17;

        if(s->l2.pocsag.state == SYNC)
            s->l2.pocsag.state = ADDRESS; // We're in sync, move on.

        if(pocsag_brute_repair(&s->l2.pocsag, &rx_data))
        {
            // Arbitration lost
            if(s->l2.pocsag.state != LOST_SYNC)
                s->l2.pocsag.state = LOSING_SYNC;
        }
        else
        {
            if(s->l2.pocsag.state == LOST_SYNC)
            {
                verbprintf(4, "Recovered sync!\n");
                s->l2.pocsag.state = ADDRESS;
            }
        }

        if(is_sync(&rx_data))
            return; // Already sync'ed.

        while(true)
            switch(s->l2.pocsag.state)
            {
            case LOSING_SYNC:
            {
                verbprintf(4, "Losing sync!\n");
                // Output what we've received so far.
                pocsag_printmessage(s, false);
                s->l2.pocsag.numnibbles = 0;
                s->l2.pocsag.address = -1;
                s->l2.pocsag.function = -1;
                s->l2.pocsag.state = LOST_SYNC;
                return;
            }

            case LOST_SYNC:
            {
                verbprintf(4, "Lost sync!\n");
                s->l2.pocsag.state = NO_SYNC;
                s->l2.pocsag.rx_word = 0;
                return;
            }

            case ADDRESS:
            {
                if(is_idle(&rx_data)) // Idle codewords have a magic address
                    return;

                if(rx_data & POCSAG_MESSAGE_DETECTION)
                {
                    verbprintf(4, "Got a message: %u\n", rx_data);
                    s->l2.pocsag.function = -2;
                    s->l2.pocsag.address  = -2;
                    s->l2.pocsag.state = MESSAGE;
                    break; // Performing partial decode
                }

                verbprintf(4, "Got an address: %u\n", rx_data);
                s->l2.pocsag.function = (rx_data >> 11) & 3;
                s->l2.pocsag.address  = ((rx_data >> 10) & 0x1ffff8) | ((rxword >> 1) & 7);
                s->l2.pocsag.state = MESSAGE;
                return;
            }

            case MESSAGE:
            {
                if(rx_data & POCSAG_MESSAGE_DETECTION)
                    verbprintf(4, "Got a message: %u\n", rx_data);
                else
                {
                    // Address/idle signals end of message
                    verbprintf(4, "Got an address: %u\n", rx_data);
                    s->l2.pocsag.state = END_OF_MESSAGE;
                    break;
                }

                if (s->l2.pocsag.numnibbles > sizeof(s->l2.pocsag.buffer)*2 - 5) {
                    verbprintf(0, "%s: Warning: Message too long\n",
                               s->dem_par->name);
                    s->l2.pocsag.state = END_OF_MESSAGE;
                    break;
                }

                uint32_t data;
                unsigned char *bp;
                bp = s->l2.pocsag.buffer + (s->l2.pocsag.numnibbles >> 1);
                data = (rx_data >> 11);
                if (s->l2.pocsag.numnibbles & 1) {
                    bp[0] = (bp[0] & 0xf0) | ((data >> 16) & 0xf);
                    bp[1] = data >> 8;
                    bp[2] = data;
                } else {
                    bp[0] = data >> 12;
                    bp[1] = data >> 4;
                    bp[2] = data << 4;
                }
                s->l2.pocsag.numnibbles += 5;
                verbprintf(5, "We received something!\n");
                return;
            }

            case END_OF_MESSAGE:
            {
                verbprintf(4, "End of message!\n");
                pocsag_printmessage(s, true);
                s->l2.pocsag.numnibbles = 0;
                s->l2.pocsag.address = -1;
                s->l2.pocsag.function = -1;
                s->l2.pocsag.state = ADDRESS;
                break;
            }

            default:
                break;
            }

    }

    default:
        break;
    }
}



/* ---------------------------------------------------------------------- */

void pocsag_rxbit(struct demod_state *s, int32_t bit)
{
    s->l2.pocsag.rx_data <<= 1;
    s->l2.pocsag.rx_data |= !bit;
    verbprintf(9, " %c ", '1'-(s->l2.pocsag.rx_data & 1));
    if(pocsag_invert_input)
        do_one_bit(s, ~(s->l2.pocsag.rx_data)); // this tries the inverted signal
    else
        do_one_bit(s, s->l2.pocsag.rx_data);
}

/* ---------------------------------------------------------------------- */
