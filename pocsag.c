/*
 *      pocsag.c -- POCSAG protocol decoder
 *
 *      Copyright (C) 1996
 *          Thomas Sailer (sailer@ife.ee.ethz.ch, hb9jnx@hb9w.che.eu)
 *
 *      Copyright (C) 2012-2014
 *          Elias Oenal    (multimon-ng@eliasoenal.com)
 *
 *      Copyright (C) 2022
 *          Tobias Girstmair (https://gir.st/)
 *
 *      Copyright (C) 2024
 *          Jason Lingohr (jason@lucid.net.au)
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
#include "bch.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "cJSON.h"

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

#define CAESAR_ALPHA 0
#define CAESAR_SKYPER 1 // skyper messages are ROT-1 enciphered

#define POSCAG
/* ---------------------------------------------------------------------- */

int pocsag_mode = POCSAG_MODE_STANDARD;
int pocsag_invert_input = 0;
int pocsag_error_correction = 2;
int pocsag_show_partial_decodes = 0;
int pocsag_heuristic_pruning = 0;
int pocsag_prune_empty = 0;
int pocsag_polarity = 0;  /* 0=auto, 1=normal only, 2=inverted only */

/* Last charset name passed to pocsag_init_charset(); empty = default US ASCII.
 * Stored here so pocsag_decode() can carry it into the PocsagFrame for JSON. */
char pocsag_active_charset[32] = "";


/* ---------------------------------------------------------------------- */

/* All decoded fields of one POCSAG message event.
 * Populated by pocsag_decode() and consumed by all output paths,
 * eliminating duplicate heuristic decode runs between v1 JSON and text. */
typedef struct {
    const char *demod_name;          /* e.g. "POCSAG1200" */
    long        address;             /* capcode; -1 unknown, -2 partial */
    int         function;            /* function nibble; -1 unknown */
    int         numnibbles;          /* raw payload nibble count */
    int         baud_rate;           /* 512, 1200, or 2400 */
    int         polarity_inverted;   /* 1 = inverted signal */
    int         sync;                /* 1 = in-sync at message end */
    /* raw packed bytes (numnibbles/2 bytes) */
    char        raw_hex[1100];       /* lowercase hex of packed buffer */
    int         raw_bits;            /* nibbles * 4 */
    /* heuristic decode results (only valid when numnibbles > 0) */
    char        num_string[1024];
    char        alpha_string[1024];
    char        skyper_string[1024];
    int         guess_num;
    int         guess_alpha;
    int         guess_skyper;
    int         unsure;              /* 1 if all scores < 20 */
    /* mode selection (what this message should emit) */
    int         do_num;
    int         do_alpha;
    int         do_sky;
    /* Active charset name at decode time; NULL means default US ASCII. */
    const char *charset;
} PocsagFrame;

/* Forward declarations for functions defined later in this file */
static void pocsag_emit_json_v1(const PocsagFrame *f);
static void pocsag_text_emit(const PocsagFrame *f);
static void pocsag_legacy_emit(const PocsagFrame *f);

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
	// ISO 646 national variant: US / IRV (1991)
    char *trtab[128] = {
			"<NUL>", 	//  0x0
			"<SOH>", 	//  0x1
			"<STX>", 	//  0x2
			"<ETX>", 	//  0x3
			"<EOT>", 	//  0x4
			"<ENQ>", 	//  0x5
			"<ACK>", 	//  0x6
			"<BEL>", 	//  0x7
			"<BS>",  	//  0x8
			"<HT>",  	//  0x9
			"<LF>",  	//  0xa
			"<VT>",  	//  0xb
			"<FF>",  	//  0xc
			"<CR>",  	//  0xd
			"<SO>",  	//  0xe
			"<SI>",  	//  0xf
			"<DLE>", 	// 0x10
			"<DC1>", 	// 0x11
			"<DC2>", 	// 0x12
			"<DC3>", 	// 0x13
			"<DC4>", 	// 0x14
			"<NAK>", 	// 0x15
			"<SYN>", 	// 0x16
			"<ETB>", 	// 0x17
			"<CAN>", 	// 0x18
			"<EM>",  	// 0x19
			"<SUB>", 	// 0x1a
			"<ESC>", 	// 0x1b
			"<FS>",  	// 0x1c
			"<GS>",  	// 0x1d
			"<RS>",  	// 0x1e
			"<US>",  	// 0x1f
			" ", 		// 0x20
			"!", 		// 0x21
			"\"", 		// 0x22

						// national variant
			"#", 		// 0x23
			"$", 		// 0x24

			"%", 		// 0x25
			"&", 		// 0x26
			"'", 		// 0x27
			"(", 		// 0x28
			")", 		// 0x29
			"*", 		// 0x2a
			"+", 		// 0x2b
			",", 		// 0x2c
			"-", 		// 0x2d
			".", 		// 0x2e
			"/", 		// 0x2f
			"0", 		// 0x30
			"1", 		// 0x31
			"2", 		// 0x32
			"3", 		// 0x33
			"4", 		// 0x34
			"5", 		// 0x35
			"6", 		// 0x36
			"7", 		// 0x37
			"8", 		// 0x38
			"9", 		// 0x39
			":", 		// 0x3a
			";", 		// 0x3b
			"<", 		// 0x3c
			"=", 		// 0x3d
			">", 		// 0x3e
			"?", 		// 0x3f
			"@", 		// 0x40
			"A", 		// 0x41
			"B", 		// 0x42
			"C", 		// 0x43
			"D", 		// 0x44
			"E", 		// 0x45
			"F", 		// 0x46
			"G", 		// 0x47
			"H", 		// 0x48
			"I", 		// 0x49
			"J", 		// 0x4a
			"K", 		// 0x4b
			"L", 		// 0x4c
			"M", 		// 0x4d
			"N", 		// 0x4e
			"O", 		// 0x4f
			"P", 		// 0x50
			"Q", 		// 0x51
			"R", 		// 0x52
			"S", 		// 0x53
			"T", 		// 0x54
			"U", 		// 0x55
			"V", 		// 0x56
			"W", 		// 0x57
			"X", 		// 0x58
			"Y", 		// 0x59
			"Z", 		// 0x5a
			
						// national variant
			"[", 		// 0x5b
			"\\", 		// 0x5c
			"]", 		// 0x5d
			"^", 		// 0x5e
			
			"_", 		// 0x5f

						// national variant
			"`", 		// 0x60

			"a", 		// 0x61
			"b", 		// 0x62
			"c", 		// 0x63
			"d", 		// 0x64
			"e", 		// 0x65
			"f", 		// 0x66
			"g", 		// 0x67
			"h", 		// 0x68
			"i", 		// 0x69
			"j", 		// 0x6a
			"k", 		// 0x6b
			"l", 		// 0x6c
			"m", 		// 0x6d
			"n", 		// 0x6e
			"o", 		// 0x6f
			"p", 		// 0x70
			"q", 		// 0x71
			"r", 		// 0x72
			"s", 		// 0x73
			"t", 		// 0x74
			"u", 		// 0x75
			"v", 		// 0x76
			"w", 		// 0x77
			"x", 		// 0x78
			"y", 		// 0x79
			"z", 		// 0x7a
			
						// national variant
			"{", 		// 0x7b
			"|", 		// 0x7c
			"}", 		// 0x7d
			"~", 		// 0x7e
			
			"<DEL>"		// 0x7f  
		};


/*
						// national variant
			"#", 		// 0x23
			"$", 		// 0x24

			"[", 		// 0x5b
			"\\", 		// 0x5c
			"]", 		// 0x5d
			"^", 		// 0x5e

			"`", 		// 0x60

			"{", 		// 0x7b
			"|", 		// 0x7c
			"}", 		// 0x7d
			"~", 		// 0x7e
*/

bool pocsag_init_charset(char *charset)
{
    /* Store the active charset name for JSON emission (pocsag.charset field). */
    strncpy(pocsag_active_charset, charset, sizeof(pocsag_active_charset) - 1);
    pocsag_active_charset[sizeof(pocsag_active_charset) - 1] = '\0';

	if(strcmp(charset,"DE")==0) // German charset
	{
		#ifdef CHARSET_UTF8
			trtab[0x5b] = "Ä";
			trtab[0x5c] = "Ö";
			trtab[0x5d] = "Ü";

			trtab[0x7b] = "ä";
			trtab[0x7c] = "ö";
			trtab[0x7d] = "ü";
			trtab[0x7e] = "ß";
		#elif defined CHARSET_LATIN1
			trtab[0x5b] = "\304";
			trtab[0x5c] = "\326";
			trtab[0x5d] = "\334";

			trtab[0x7b] = "\344";
			trtab[0x7c] = "\366";
			trtab[0x7d] = "\374";
			trtab[0x7e] = "\337";
		#else
			trtab[0x5b] = "AE";
			trtab[0x5c] = "OE";
			trtab[0x5d] = "UE";

			trtab[0x7b] = "ae";
			trtab[0x7c] = "oe";
			trtab[0x7d] = "ue";
			trtab[0x7e] = "ss";
		#endif
	}
	else if (strcmp(charset,"DK")==0) // Danish charset /JT Ref. https://www.ascii-code.com
	{
		#ifdef CHARSET_UTF8
			trtab[0x5b] = "Æ";
			trtab[0x5c] = "Ø";
			trtab[0x5d] = "Å";

			trtab[0x7b] = "æ";
			trtab[0x7c] = "ø";
			trtab[0x7d] = "å";
		#elif defined CHARSET_LATIN1
			trtab[0x5b] = "\306";
			trtab[0x5c] = "\330";
			trtab[0x5d] = "\305";

			trtab[0x7b] = "\346";
			trtab[0x7c] = "\370";
			trtab[0x7d] = "\345";
		#else
			trtab[0x5b] = "AE";
			trtab[0x5c] = "OE";
			trtab[0x5d] = "Aa";

			trtab[0x7b] = "ae";
			trtab[0x7c] = "oe";
			trtab[0x7d] = "aa";
		#endif
	}
	else if (strcmp(charset,"SE")==0) // Swedish charset
	{
		#ifdef CHARSET_UTF8
			trtab[0x5b] = "Ä";
			trtab[0x5c] = "Ö";
			trtab[0x5d] = "Å";

			trtab[0x7b] = "ä";
			trtab[0x7c] = "ö";
			trtab[0x7d] = "å";
		#elif defined CHARSET_LATIN1
			trtab[0x5b] = "\304";
			trtab[0x5c] = "\326";
			trtab[0x5d] = "\305";

			trtab[0x7b] = "\344";
			trtab[0x7c] = "\366";
			trtab[0x7d] = "\345";
		#else
			trtab[0x5b] = "AE";
			trtab[0x5c] = "OE";
			trtab[0x5d] = "AO";

			trtab[0x7b] = "ae";
			trtab[0x7c] = "oe";
			trtab[0x7d] = "ao";
		#endif
	}
	else if (strcmp(charset,"FR")==0) // French charset
	{
		trtab[0x24] = "£";

		trtab[0x40] = "à";

		trtab[0x5b] = "°";
		trtab[0x5c] = "ç";
		trtab[0x5d] = "§";
		trtab[0x5e] = "^";
		trtab[0x5f] = "_";
		trtab[0x60] = "µ";

		trtab[0x7b] = "é";
		trtab[0x7c] = "ù";
		trtab[0x7d] = "è";
		trtab[0x7e] = "¨";
	}
	else if (strcmp(charset,"SI")==0) // Slovenian charset
	{
		#ifdef CHARSET_UTF8
			trtab[0x40] = "Ž";
			trtab[0x5b] = "Š";
			trtab[0x5c] = "Đ";
			trtab[0x5d] = "Ć";
			trtab[0x5e] = "Č";

			trtab[0x60] = "ž";
			trtab[0x7b] = "š";
			trtab[0x7c] = "đ";
			trtab[0x7d] = "ć";
			trtab[0x7e] = "č";
		#else
			trtab[0x40] = "Z";
			trtab[0x5b] = "S";
			trtab[0x5c] = "Dj";
			trtab[0x5d] = "C";
			trtab[0x5e] = "C";

			trtab[0x60] = "z";
			trtab[0x7b] = "s";
			trtab[0x7c] = "dj";
			trtab[0x7d] = "c";
			trtab[0x7e] = "c";
		#endif
	}
	else if (strcmp(charset,"US")==0) // US charset
	{
		// default
	}
	else
	{
		fprintf(stderr, "Error: invalid POCSAG charset %s\n", charset);
		fprintf(stderr, "Use: US,FR,DE,DK,SE,SI\n");
		charset = "US";
		return false; 
	}
	return true;
}

static char *translate_alpha(unsigned char chr)
{
	return trtab[chr & 0x7f];
}

/* ---------------------------------------------------------------------- */
static int guesstimate_alpha(const unsigned char cp)
{
    if((cp > 0 && cp < 32) || cp == 127)
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

static int print_msg_numeric(struct l2_state_pocsag *rx, char* buff, unsigned int size)
{
    static const char *conv_table = "084 2.6]195-3U7[";
    unsigned char *bp = rx->buffer;
    int len = rx->numnibbles;
    char* cp = buff;
    int guesstimate = 0;

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

static unsigned char get7(const unsigned char *buf, int n)
{
    /* returns the n-th seven bit word */
    return ( buf[(n*7)/8]<<8 | buf[(n*7+6)/8] ) >> (n+1)%8;
}

static unsigned char rev7(unsigned char b)
{
    /* reverses the bit order of a seven bit word */
    return ((b << 6) & 64) | ((b >> 6) & 1) |
           ((b << 4) & 32) | ((b >> 4) & 2) |
           ((b << 2) & 16) | ((b >> 2) & 4) |
           ((b << 0) & 8);
}

static int print_msg_alpha(struct l2_state_pocsag *rx, char* buff, unsigned int size, int caesar)
{
    int len = rx->numnibbles * 4 / 7;
    char* cp = buff;
    int buffree = size-1;
    unsigned char curchr;
    char *tstr;
    int guesstimate = 0;

    for (int i = 0; i < len; i++)
    {
        curchr = rev7(get7(rx->buffer, i)) - caesar;

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

static void pocsag_bytes_to_hex(const unsigned char *buf, int nbytes,
                                char *out, size_t out_sz)
{
    static const char hexd[] = "0123456789abcdef";
    int i;
    for (i = 0; i < nbytes && (size_t)(i * 2 + 2) < out_sz; i++) {
        out[i * 2]     = hexd[(buf[i] >> 4) & 0xf];
        out[i * 2 + 1] = hexd[buf[i] & 0xf];
    }
    out[i * 2] = '\0';
}

static int pocsag_baud_from_name(const char *name)
{
    if (!name) return 0;
    if (strstr(name, "512"))  return 512;
    if (strstr(name, "1200")) return 1200;
    if (strstr(name, "2400")) return 2400;
    return 0;
}

/* Decode one POCSAG message into a PocsagFrame.
 * Runs all three heuristic decode attempts (numeric, alpha, skyper) exactly
 * once so neither the text path nor the JSON path needs to run them again. */
static void pocsag_decode(struct demod_state *s, bool sync, PocsagFrame *f)
{
    memset(f, 0, sizeof(*f));
    f->demod_name        = s->dem_par->name;
    f->address           = s->l2.pocsag.address;
    f->function          = s->l2.pocsag.function;
    f->numnibbles        = s->l2.pocsag.numnibbles;
    f->baud_rate         = pocsag_baud_from_name(s->dem_par->name);
    f->polarity_inverted = s->l2.pocsag.inverted;
    f->sync              = sync;
    f->charset           = (pocsag_active_charset[0] ? pocsag_active_charset : NULL);

    if (f->numnibbles > 0) {
        int nbytes = (f->numnibbles + 1) / 2;
        pocsag_bytes_to_hex(s->l2.pocsag.buffer, nbytes, f->raw_hex, sizeof(f->raw_hex));
        f->raw_bits = f->numnibbles * 4;

        f->guess_num    = print_msg_numeric(&s->l2.pocsag,
                              f->num_string, sizeof(f->num_string));
        f->guess_alpha  = print_msg_alpha(&s->l2.pocsag,
                              f->alpha_string, sizeof(f->alpha_string), CAESAR_ALPHA);
        f->guess_skyper = print_msg_alpha(&s->l2.pocsag,
                              f->skyper_string, sizeof(f->skyper_string), CAESAR_SKYPER);

        if (f->guess_num < 20 && f->guess_alpha < 20 && f->guess_skyper < 20)
            f->unsure = 1;

        int func = f->function;
        f->do_num   = (pocsag_mode == POCSAG_MODE_NUMERIC) ||
                      ((pocsag_mode == POCSAG_MODE_STANDARD) && (func == 0)) ||
                      ((pocsag_mode == POCSAG_MODE_AUTO)     && (f->guess_num >= 20 || f->unsure));
        f->do_alpha = (pocsag_mode == POCSAG_MODE_ALPHA) ||
                      ((pocsag_mode == POCSAG_MODE_STANDARD) && (func != 0)) ||
                      ((pocsag_mode == POCSAG_MODE_AUTO)     && (f->guess_alpha >= f->guess_skyper || f->unsure));
        f->do_sky   = (pocsag_mode == POCSAG_MODE_SKYPER) ||
                      ((pocsag_mode == POCSAG_MODE_AUTO)     && (f->guess_skyper >= f->guess_alpha || f->unsure));
    }
}

/* Single dispatch point: text, v1 JSON, or legacy JSON. */
static void pocsag_emit(const PocsagFrame *f)
{
    if (json_format == JSON_FORMAT_UNIFIED)
        pocsag_emit_json_v1(f);
    else if (json_format != JSON_FORMAT_NONE)
        pocsag_legacy_emit(f);
    else
        pocsag_text_emit(f);
}

/* Emit one JSON v2 object for a POCSAG message. See docs/json-output-schema-v2.md.
 * POCSAG has no wire message-type field: all heuristic decode attempts are
 * emitted as scored entries in encodings[], raw bytes first.
 * Takes a pre-decoded PocsagFrame so heuristics run only once. */
static void pocsag_emit_json_v1(const PocsagFrame *f)
{
    int tone_only = (f->numnibbles == 0);
    long long capcode = (f->address >= 0) ? (long long)f->address : -1;

    /* Determine semantic msg_type from the winning decode.
     * In auto mode all three may be attempted; use the highest score to pick.
     * In non-auto mode only one flag is set. */
    const char *msg_type_str = "alphanumeric"; /* default for alpha/skyper */
    if (!tone_only) {
        if (f->do_num && !f->do_alpha && !f->do_sky) {
            msg_type_str = "numeric";
        } else if (f->do_alpha || f->do_sky) {
            /* auto: pick winner by score */
            int best_s = -2147483647;
            int num_wins = 0;
            if (f->do_alpha && f->guess_alpha > best_s) { best_s = f->guess_alpha; num_wins = 0; }
            if (f->do_sky   && f->guess_skyper > best_s) { best_s = f->guess_skyper; num_wins = 0; }
            if (f->do_num   && f->guess_num > best_s)    { num_wins = 1; }
            msg_type_str = num_wins ? "numeric" : "alphanumeric";
        }
    }

    cJSON *obj = json_new(f->demod_name, "pocsag",
                            tone_only ? "event" : "message",
                            tone_only ? "tone_only" : msg_type_str);
    if (!obj) return;

    json_set_capcode(obj, capcode, (capcode >= 0) ? "short" : NULL);

    const char *best = NULL;

    if (!tone_only) {
        /* raw encoding always first */
        json_add_encoding(obj, "raw", f->raw_hex, -1, f->raw_bits);

        /* Collect candidates and emit in descending score order (raw first,
         * then text attempts highest score first). In non-auto modes only
         * one flag is set so order is trivial. */
        struct { const char *enc; const char *str; int score; } cands[3];
        int ncands = 0;
        if (f->do_alpha) { cands[ncands].enc = "alpha";   cands[ncands].str = f->alpha_string;   cands[ncands].score = f->guess_alpha;   ncands++; }
        if (f->do_num)   { cands[ncands].enc = "numeric"; cands[ncands].str = f->num_string;     cands[ncands].score = f->guess_num;     ncands++; }
        if (f->do_sky)   { cands[ncands].enc = "skyper";  cands[ncands].str = f->skyper_string;  cands[ncands].score = f->guess_skyper;  ncands++; }

        /* Insertion sort descending by score (at most 3 elements) */
        for (int si = 1; si < ncands; si++) {
            __typeof__(cands[0]) tmp = cands[si];
            int sj = si - 1;
            while (sj >= 0 && cands[sj].score < tmp.score) { cands[sj+1] = cands[sj]; sj--; }
            cands[sj+1] = tmp;
        }

        int show_score = (pocsag_mode == POCSAG_MODE_AUTO);
        for (int ci = 0; ci < ncands; ci++) {
            json_add_encoding(obj, cands[ci].enc, cands[ci].str,
                                show_score ? cands[ci].score : -1, -1);
        }
        /* best = highest scoring candidate (first after sort), or single non-auto pick */
        if (ncands > 0) best = cands[0].str;
        if (pocsag_mode != POCSAG_MODE_AUTO) {
            if      (f->do_alpha) best = f->alpha_string;
            else if (f->do_num)   best = f->num_string;
            else if (f->do_sky)   best = f->skyper_string;
        }
    }

    json_set_message(obj, best);

    cJSON *p = json_proto(obj, "pocsag");
    if (p) {
        if (f->function >= 0) cJSON_AddNumberToObject(p, "function", f->function);
        if (f->baud_rate)     cJSON_AddNumberToObject(p, "baud_rate", f->baud_rate);
        cJSON_AddStringToObject(p, "polarity", f->polarity_inverted ? "inverted" : "normal");
        /* charset: only present when a non-default charset is active (-C flag).
         * Absent means US ASCII (the default). */
        if (f->charset)       cJSON_AddStringToObject(p, "charset", f->charset);
    }

    json_emit(obj);
}

/* Text output for one POCSAG event. */
static void pocsag_text_emit(const PocsagFrame *f)
{
    int has_addr = (f->address != -2) || (f->function != -2);

    if (f->numnibbles == 0) {
        /* tone-only */
        if (has_addr)
            verbprintf(0, "%s: Address: %7lu  Function: %1hhi ",
                       f->demod_name, f->address, f->function);
        if (!f->sync) verbprintf(2, "<LOST SYNC>");
        verbprintf(0, "\n");
        return;
    }

    if (pocsag_heuristic_pruning && f->unsure)
        return; /* already pruned before decode, but guard here too */

    if (f->do_num) {
        if (has_addr)
            verbprintf(0, "%s: Address: %7lu  Function: %1hhi  ",
                       f->demod_name, f->address, f->function);
        else
            verbprintf(0, "%s: Address:       -  Function: -  ", f->demod_name);
        if (pocsag_mode == POCSAG_MODE_AUTO)
            verbprintf(3, "Certainty: %5i  ", f->guess_num);
        verbprintf(0, "Numeric: %s", f->num_string);
        if (!f->sync) verbprintf(2, "<LOST SYNC>");
        verbprintf(0, "\n");
    }

    if (f->do_alpha) {
        if (has_addr)
            verbprintf(0, "%s: Address: %7lu  Function: %1hhi  ",
                       f->demod_name, f->address, f->function);
        else
            verbprintf(0, "%s: Address:       -  Function: -  ", f->demod_name);
        if (pocsag_mode == POCSAG_MODE_AUTO)
            verbprintf(3, "Certainty: %5i  ", f->guess_alpha);
        verbprintf(0, "Alpha:   %s", f->alpha_string);
        if (!f->sync) verbprintf(2, "<LOST SYNC>");
        verbprintf(0, "\n");
    }

    if (f->do_sky) {
        if (has_addr)
            verbprintf(0, "%s: Address: %7lu  Function: %1hhi  ",
                       f->demod_name, f->address, f->function);
        else
            verbprintf(0, "%s: Address:       -  Function: -  ", f->demod_name);
        if (pocsag_mode == POCSAG_MODE_AUTO)
            verbprintf(3, "Certainty: %5i  ", f->guess_skyper);
        verbprintf(0, "Skyper:  %s", f->skyper_string);
        if (!f->sync) verbprintf(2, "<LOST SYNC>");
        verbprintf(0, "\n");
    }
}

/* Legacy JSON emit: produces one object per selected decode attempt,
 * matching the pre-v1 per-attempt output format. */
static void pocsag_legacy_emit(const PocsagFrame *f)
{
    int has_addr = (f->address != -2) || (f->function != -2);

    if (f->numnibbles == 0) {
        /* tone-only legacy: one object with address/function only */
        cJSON *j = cJSON_CreateObject();
        if (!j) return;
        cJSON_AddStringToObject(j, "demod_name", f->demod_name);
        if (has_addr) {
            cJSON_AddNumberToObject(j, "address",  f->address);
            cJSON_AddNumberToObject(j, "function", f->function);
        } else {
            cJSON_AddNullToObject(j, "address");
            cJSON_AddNullToObject(j, "function");
        }
        addJsonTimestamp(j);
        printJson(j);
        cJSON_Delete(j);
        return;
    }

    if (f->do_num) {
        cJSON *j = cJSON_CreateObject();
        if (!j) return;
        cJSON_AddStringToObject(j, "demod_name", f->demod_name);
        if (has_addr) {
            cJSON_AddNumberToObject(j, "address",  f->address);
            cJSON_AddNumberToObject(j, "function", f->function);
        } else {
            cJSON_AddNullToObject(j, "address");
            cJSON_AddNullToObject(j, "function");
        }
        cJSON_AddStringToObject(j, "numeric", f->num_string);
        addJsonTimestamp(j);
        printJson(j);
        fflush(stdout);
        cJSON_Delete(j);
    }

    if (f->do_alpha) {
        cJSON *j = cJSON_CreateObject();
        if (!j) return;
        cJSON_AddStringToObject(j, "demod_name", f->demod_name);
        if (has_addr) {
            cJSON_AddNumberToObject(j, "address",  f->address);
            cJSON_AddNumberToObject(j, "function", f->function);
        } else {
            cJSON_AddNullToObject(j, "address");
            cJSON_AddNullToObject(j, "function");
        }
        cJSON_AddStringToObject(j, "alpha", f->alpha_string);
        addJsonTimestamp(j);
        printJson(j);
        fflush(stdout);
        cJSON_Delete(j);
    }

    if (f->do_sky) {
        cJSON *j = cJSON_CreateObject();
        if (!j) return;
        cJSON_AddStringToObject(j, "demod_name", f->demod_name);
        if (has_addr) {
            cJSON_AddNumberToObject(j, "address",  f->address);
            cJSON_AddNumberToObject(j, "function", f->function);
        } else {
            cJSON_AddNullToObject(j, "address");
            cJSON_AddNullToObject(j, "function");
        }
        cJSON_AddStringToObject(j, "skyper", f->skyper_string);
        addJsonTimestamp(j);
        printJson(j);
        fflush(stdout);
        cJSON_Delete(j);
    }
}

static void pocsag_printmessage(struct demod_state *s, bool sync)
{
    if (!pocsag_show_partial_decodes &&
        ((s->l2.pocsag.address == -2) || (s->l2.pocsag.function == -2) || !sync))
        return; /* hide partial decodes */
    if (pocsag_prune_empty && (s->l2.pocsag.numnibbles == 0))
        return;

    if ((s->l2.pocsag.address == -1) && (s->l2.pocsag.function == -1))
        return; /* nothing to report */

    PocsagFrame f;
    pocsag_decode(s, sync, &f);

    /* heuristic pruning: all scores below threshold and no sync message */
    if (f.numnibbles > 0 && f.unsure && pocsag_heuristic_pruning)
        return;

    pocsag_emit(&f);
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

/* ---------------------------------------------------------------------- */

// BCH error correction using unified bch library
int pocsag_brute_repair(struct l2_state_pocsag *rx, uint32_t* data)
{
    int result = bch_pocsag_correct(data);
    
    if (result == 0) {
        return 0;  /* No errors */
    }
    
    rx->pocsag_total_error_count++;
    verbprintf(6, "Error in syndrome detected!\n");
    
    if (pocsag_error_correction == 0) {
        rx->pocsag_uncorrected_error_count++;
        verbprintf(6, "Couldn't correct error!\n");
        return 1;
    }
    
    if (result < 0) {
        /* Uncorrectable error */
        rx->pocsag_uncorrected_error_count++;
        verbprintf(6, "Couldn't correct error!\n");
        return 1;
    }
    
    /* Check if we're allowed to correct this many errors */
    if (result > pocsag_error_correction) {
        rx->pocsag_uncorrected_error_count++;
        verbprintf(6, "Couldn't correct error!\n");
        return 1;
    }
    
    /* Correction was applied by bch_pocsag_correct */
    rx->pocsag_corrected_error_count++;
    
    if (result == 1) {
        rx->pocsag_corrected_1bit_error_count++;
    } else {
        rx->pocsag_corrected_2bit_error_count++;
    }
    
    return 0;
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
        uint32_t rx_data_try;
        
        s->l2.pocsag.pocsag_bits_processed_while_not_synced++;

        /* Try normal polarity with error correction (unless inverted-only mode) */
        if(pocsag_polarity != 2)
        {
            rx_data_try = rx_data;
            pocsag_brute_repair(&s->l2.pocsag, &rx_data_try);
            if(rx_data_try == POCSAG_SYNC)
            {
                verbprintf(4, "Acquired sync!\n");
                s->l2.pocsag.state = SYNC;
                s->l2.pocsag.inverted = 0;
                return;
            }
        }
        
        /* Try inverted polarity with error correction (unless normal-only mode) */
        if(pocsag_polarity != 1)
        {
            rx_data_try = ~rx_data;
            pocsag_brute_repair(&s->l2.pocsag, &rx_data_try);
            if(rx_data_try == POCSAG_SYNC)
            {
                verbprintf(3, "Acquired sync (inverted polarity detected)!\n");
                s->l2.pocsag.state = SYNC;
                s->l2.pocsag.inverted = 1;
                return;
            }
        }
        return;
    }

    case SYNC:
    {
        s->l2.pocsag.pocsag_bits_processed_while_synced++;

        /* Apply inversion if auto-detected */
        if(s->l2.pocsag.inverted)
            rx_data = ~rx_data;

        if(!word_complete(s))
            return; // Wait for more bits to arrive.

        // it is always 17 words: position 0 is sync, positions 1-16 are data
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
                    if (json_format == JSON_FORMAT_NONE) {
                        verbprintf(2, "%s: Warning: Message too long\n",
                                   s->dem_par->name);
                    } else {
                        fprintf(stdout, "{\"error\": \"%s: Warning: Message too long\n\"}", s->dem_par->name);
                        fflush(stdout);
                    }
                    /* Message too long indicates we're decoding garbage - lose sync */
                    s->l2.pocsag.state = LOSING_SYNC;
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
