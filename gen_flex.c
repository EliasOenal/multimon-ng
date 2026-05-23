/*
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * For more information, please refer to <https://unlicense.org/>
 */

#include "gen.h"
#include "bch.h"
#include <string.h>
#include <stdio.h>

/*
 * FLEX Protocol Constants
 * 
 * FLEX uses 2-FSK or 4-FSK modulation at 1600 or 3200 baud.
 * We implement 1600 baud 2-FSK for simplicity.
 * 
 * Frame structure:
 *   SYNC1 (64 bits) -> FIW (32 bits) -> SYNC2 (25ms) -> DATA (1760ms)
 */

#define FLEX_SYNC_MARKER    0xA6C6AAAAul
#define FLEX_SYNC_1600_2FSK 0x870C          /* Sync code for 1600 baud 2-FSK */
#define FLEX_BAUD           1600

/* Phase A has 88 codewords at 1600 baud */
#define FLEX_CODEWORDS_PER_PHASE 88

/* FLEX message types */
#define FLEX_PAGETYPE_ALPHANUMERIC 5

/*---------------------------------------------------------------------------*/

/* Encode 21-bit data into a 32-bit FLEX word with even parity (bit 31).
 * Bits 0-20 = data, bits 21-30 = BCH parity, bit 31 = even parity over bits 0-30. */
static uint32_t flex_encode_word(uint32_t data)
{
    uint32_t cw = bch_flex_encode(data & 0x1FFFFF);
    /* Compute even parity over bits 0-30 and set bit 31 */
    uint32_t p = 0, tmp = cw;
    while (tmp) { p ^= 1; tmp &= tmp - 1; }
    return cw | (p << 31);
}

/* Inject bit errors into a 31-bit codeword */
static uint32_t inject_errors(uint32_t codeword, int num_errors, unsigned int *seed)
{
    int positions[3] = {-1, -1, -1};
    int i, j, pos;
    
    for (i = 0; i < num_errors && i < 3; i++) {
        /* Simple PRNG for reproducible errors */
        do {
            *seed = *seed * 1103515245 + 12345;
            pos = (*seed >> 16) % 31;
            /* Ensure unique positions */
            for (j = 0; j < i; j++) {
                if (positions[j] == pos) {
                    pos = -1;
                    break;
                }
            }
        } while (pos < 0);
        
        positions[i] = pos;
        codeword ^= (1u << pos);
    }
    
    return codeword;
}

/* Build 64-bit SYNC1 word */
static uint64_t build_sync1(void)
{
    /* SYNC1 format: AAAA:BBBBBBBB:CCCC
     * BBBBBBBB = 0xA6C6AAAA (marker)
     * AAAA = sync code for mode (0x870C for 1600/2FSK)
     * CCCC = AAAA ^ 0xFFFF
     */
    uint16_t sync_code = FLEX_SYNC_1600_2FSK;
    uint16_t complement = sync_code ^ 0xFFFF;
    
    return ((uint64_t)sync_code << 48) |
           ((uint64_t)FLEX_SYNC_MARKER << 16) |
           complement;
}

/* Build FIW (Frame Information Word) - returns BCH-encoded 31-bit word */
static uint32_t build_fiw(int cycle, int frame)
{
    /* FIW format (21 data bits):
     * Bits 3:0   = checksum (computed)
     * Bits 7:4   = cycle number (0-14)
     * Bits 14:8  = frame number (0-127)
     * Bits 20:15 = fix3/reserved (0)
     * 
     * Checksum: nibble0 + nibble1 + nibble2 + nibble3 + nibble4 + bit20 = 0xF
     */
    uint32_t fiw = 0;
    
    fiw |= (cycle & 0xF) << 4;
    fiw |= (frame & 0x7F) << 8;
    
    /* Compute checksum such that total sum = 0xF */
    int sum = 0;
    sum += (fiw >> 4) & 0xF;   /* nibble1: cycle */
    sum += (fiw >> 8) & 0xF;   /* nibble2: frame low */
    sum += (fiw >> 12) & 0xF;  /* nibble3: frame high + fix3 low */
    sum += (fiw >> 16) & 0xF;  /* nibble4: fix3 mid */
    sum += (fiw >> 20) & 0x1;  /* bit20 */
    int checksum = (0xF - sum) & 0xF;
    fiw |= checksum;
    
    return flex_encode_word(fiw);
}

/* Build BIW (Block Information Word) - returns BCH-encoded 31-bit word */
static uint32_t build_biw(int voffset, int aoffset)
{
    /* BIW format (21 data bits):
     * Bits 9:8   = address field offset (aoffset)
     * Bits 15:10 = vector field offset (voffset)
     * Other bits = various flags (we set to 0)
     */
    uint32_t biw = 0;
    
    biw |= (aoffset & 0x3) << 8;
    biw |= (voffset & 0x3F) << 10;
    
    return flex_encode_word(biw);
}

/* Build address word - returns BCH-encoded 31-bit word */
static uint32_t build_address(uint32_t capcode)
{
    /* Address format (21 data bits):
     * The decoder does: capcode = aw1 - 0x8000
     * So we encode: aw1 = capcode + 0x8000
     */
    return flex_encode_word((capcode + 0x8000) & 0x1FFFFF);
}

/* Build vector word - returns BCH-encoded 31-bit word */
static uint32_t build_vector(int msg_type, int msg_start, int msg_len)
{
    /* Vector format (21 data bits):
     * Bits 3:0   = 4-bit checksum
     * Bits 6:4   = message type V
     * Bits 13:7  = message word start (b)
     * Bits 20:14 = message length in words (n)
     *
     * Checksum: sum of five 4-bit nibbles + bit 20, lower 4 bits = 0xF.
     */
    uint32_t vec = 0;
    
    vec |= (msg_type & 0x7) << 4;
    vec |= (msg_start & 0x7F) << 7;
    vec |= (msg_len & 0x7F) << 14;
    
    /* Compute 4-bit checksum: set bits 0-3 so that
     * (nib0 + nib1 + nib2 + nib3 + nib4 + bit20) & 0xF == 0xF */
    uint32_t sum = ((vec >> 4) & 0xF) + ((vec >> 8) & 0xF) +
                   ((vec >> 12) & 0xF) + ((vec >> 16) & 0xF) + ((vec >> 20) & 0x1);
    uint32_t checksum = (0xF - (sum & 0xF)) & 0xF;
    vec |= checksum;
    
    return flex_encode_word(vec);
}

/* Build message word - returns BCH-encoded 31-bit word */
static uint32_t build_message_word(uint32_t data)
{
    return flex_encode_word(data & 0x1FFFFF);
}

/* Encode a string to FLEX message words (7-bit ASCII)
 * For frag==3 (complete message), skip_first_char should be 1 to leave
 * the first character position of the first word empty.
 */
static int encode_message(const char *msg, uint32_t *words, int max_words, int skip_first_char)
{
    int len = strlen(msg);
    int word_idx = 0;
    int bit_pos = skip_first_char ? 7 : 0;  /* Skip first 7 bits if needed */
    uint32_t current = 0;
    int i;
    
    for (i = 0; i < len && word_idx < max_words; i++) {
        uint32_t ch = msg[i] & 0x7F;
        
        /* Pack 7-bit characters into 21-bit words (3 chars per word) */
        current |= ch << bit_pos;
        bit_pos += 7;
        
        if (bit_pos >= 21) {
            words[word_idx++] = current & 0x1FFFFF;
            current = ch >> (7 - (bit_pos - 21));
            bit_pos -= 21;
        }
    }
    
    /* Handle remaining bits */
    if (bit_pos > 0 && word_idx < max_words) {
        words[word_idx++] = current & 0x1FFFFF;
    }
    
    return word_idx;
}

/*---------------------------------------------------------------------------*/

/* Helper: add bits to bitstream, MSB first, inverted (for sync patterns) */
static void add_bits_msb_inv(unsigned char *data, int *idx, uint64_t value, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--) {
        data[(*idx)++] = !((value >> i) & 1);
    }
}

/* Helper: add bits to bitstream, LSB first from value */
static void add_bits_lsb(unsigned char *data, int *idx, uint32_t value, int nbits)
{
    for (int i = 0; i < nbits; i++) {
        data[(*idx)++] = (value >> i) & 1;
    }
}

void gen_init_flex(struct gen_params *p, struct gen_state *s)
{
    int i, bit_idx;
    uint32_t codewords[FLEX_CODEWORDS_PER_PHASE];
    uint32_t msg_words[84];  /* Max 84 content words in single-phase frame */
    int num_msg_words;
    unsigned int error_seed = 12345;  /* Seed for error injection */
    
    memset(s, 0, sizeof(struct gen_state));
    
    /* Initialize all codewords to idle with alternating patterns
     * This ensures signal transitions for demodulator timing recovery.
     * Use alternating 0xAAAAAA/0x155555 patterns instead of all-1s */
    for (i = 0; i < FLEX_CODEWORDS_PER_PHASE; i++) {
        if (i % 2 == 0) {
            codewords[i] = flex_encode_word(0x0AAAAA);  /* 010101... pattern */
        } else {
            codewords[i] = flex_encode_word(0x155555);  /* 101010... pattern */
        }
    }
    
    /* Encode the message
     * When frag==3 (complete message), the decoder skips the first char position
     * of the first content word. So we need to leave it empty (skip_first_char=1).
     * Max 84 content words = 251 characters (2 + 83*3)
     */
    num_msg_words = encode_message(p->p.flex.message, msg_words, 84, 1);
    
    /* Build frame structure:
     * Word 0: BIW
     * Word 1: Address
     * Word 2: Vector
     * Word 3+: Message data
     */
    int voffset = 2;  /* Vector starts at word 2 */
    int aoffset = 0;  /* Address offset */
    int msg_start = 3;  /* Message header at word 3, content starts at word 4 */
    int total_msg_words = num_msg_words + 1;  /* +1 for header word */
    
    codewords[0] = build_biw(voffset, aoffset);
    codewords[1] = build_address(p->p.flex.capcode);
    codewords[2] = build_vector(FLEX_PAGETYPE_ALPHANUMERIC, msg_start, total_msg_words);
    
    /* Message header word at msg_start:
     *   bits 0-9:   K (10-bit fragment checksum, computed last)
     *   bit  10:    C=0 (not continued)
     *   bits 11-12: F=11 (complete message, frag=3)
     *   bits 13-18: N=0 (message number)
     *   bit  19:    R=1 (new message)
     *   bit  20:    M=0 (no maildrop)
     */
    uint32_t msg_header = 0;
    msg_header |= (3 << 11);   /* F=11 */
    msg_header |= (0 << 13);   /* N=0 */
    msg_header |= (1 << 19);   /* R=1 */

    /* First content word (msg_start+1): bits 0-6 = signature S.
     * Signature = 1's complement of sum of all 7-bit character slots
     * across all content words, excluding ETX (0x03).
     * Characters start at bits 7-13 and 14-20 of the first content word,
     * and bits 0-6, 7-13, 14-20 of subsequent content words.
     *
     * The message was encoded with skip_first_char=1, so msg_words[0]
     * has bits 0-6 empty (signature slot), chars at bits 7-13 and 14-20.
     */
    {
        /* Compute signature from the encoded content words (same as decoder).
         * The decoder extracts 7-bit character slots from the transmitted words
         * and sums them, so we must do the same — NOT sum the raw input string,
         * which may be longer than what fits in 84 content words.
         *
         * First content word (msg_words[0]): bits 0-6 reserved for signature,
         *   character slots at bits 7-13 and 14-20.
         * Subsequent words: character slots at bits 0-6, 7-13, 14-20.
         */
        uint32_t sig_sum = 0;
        uint32_t ch;
        /* First word: skip bits 0-6 (signature slot), sum bits 7-13 and 14-20 */
        ch = (msg_words[0] >> 7) & 0x7F;
        if (ch != 0x03) sig_sum += ch;
        ch = (msg_words[0] >> 14) & 0x7F;
        if (ch != 0x03) sig_sum += ch;
        /* Remaining words: sum all three 7-bit slots */
        for (i = 1; i < num_msg_words; i++) {
            ch = msg_words[i] & 0x7F;
            if (ch != 0x03) sig_sum += ch;
            ch = (msg_words[i] >> 7) & 0x7F;
            if (ch != 0x03) sig_sum += ch;
            ch = (msg_words[i] >> 14) & 0x7F;
            if (ch != 0x03) sig_sum += ch;
        }
        uint32_t sig = (~sig_sum) & 0x7F;
        /* Store signature in bits 0-6 of first content word */
        msg_words[0] |= sig;
    }

    /* Place header and content words into frame */
    codewords[msg_start] = build_message_word(msg_header);
    for (i = 0; i < num_msg_words && (msg_start + 1 + i) < FLEX_CODEWORDS_PER_PHASE; i++) {
        codewords[msg_start + 1 + i] = build_message_word(msg_words[i]);
    }

    /* Compute K checksum (10-bit):
     * 1's complement of binary sum of all message words (header + content)
     * in three groups per word: bits 0-7, 8-15, 16-20.
     * K field (bits 0-9 of header) is zeroed during computation.
     */
    {
        uint32_t k_sum = 0;
        /* Header word (K field already 0) */
        k_sum += msg_header & 0xFF;
        k_sum += (msg_header >> 8) & 0xFF;
        k_sum += (msg_header >> 16) & 0x1F;
        /* Content words */
        for (i = 0; i < num_msg_words; i++) {
            uint32_t w = msg_words[i] & 0x1FFFFF;
            k_sum += w & 0xFF;
            k_sum += (w >> 8) & 0xFF;
            k_sum += (w >> 16) & 0x1F;
        }
        msg_header |= (~k_sum) & 0x3FF;  /* K in bits 0-9 */
        /* Re-encode header with K checksum */
        codewords[msg_start] = build_message_word(msg_header);
    }
    
    /* Inject errors if requested */
    if (p->p.flex.errors > 0) {
        /* Inject errors into specific codewords for testing */
        for (i = 0; i < FLEX_CODEWORDS_PER_PHASE; i++) {
            if (i < 10) {  /* Apply errors to first 10 codewords */
                codewords[i] = inject_errors(codewords[i], p->p.flex.errors, &error_seed);
            }
        }
    }
    
    /* Build the complete bitstream - one bit per byte */
    bit_idx = 0;
    
    /* Preamble: alternating bits for sync detection
     * Sync detector uses negative=1 convention, so we output inverted
     * Original sync expects 10101010, but with positive=1 output, we need 01010101 */
    for (i = 0; i < 960; i++) {
        s->s.flex.data[bit_idx++] = (i & 1) ? 1 : 0;  /* Inverted: 01010101... */
    }
    
    /* SYNC1: 64 bits, MSB first, inverted for sync convention */
    uint64_t sync1 = build_sync1();
    add_bits_msb_inv(s->s.flex.data, &bit_idx, sync1, 64);
    
    /* Dotting bits before FIW: 16 bits, inverted for sync convention */
    for (i = 0; i < 16; i++) {
        s->s.flex.data[bit_idx++] = (i & 1) ? 1 : 0;  /* Inverted: 01010101... */
    }
    
    /* FIW: 32 bits, LSB first (bit 0 transmitted first)
     * The parity table is computed to give zero syndrome after the decoder's
     * bit reversal during BCH processing */
    uint32_t fiw = build_fiw(p->p.flex.cycle, p->p.flex.frame);
    if (p->p.flex.errors > 0 && p->p.flex.errors <= 2) {
        fiw = inject_errors(fiw, p->p.flex.errors, &error_seed);
    }
    add_bits_lsb(s->s.flex.data, &bit_idx, fiw, 32);
    
    /* SYNC2: BS2(4) + C(16) + inv.BS2(4) + inv.C(16) = 40 bits at 1600/2FSK
     * Per the FLEX standard:
     *   C = 0xED84 (16 bits), inv.C = 0x127B
     *   BS2 = alternating dotting (4 bits) */
    /* BS2: 4 bits dotting */
    for (i = 0; i < 4; i++) {
        s->s.flex.data[bit_idx++] = (i & 1) ? 1 : 0;
    }
    /* C pattern: 16 bits MSB first */
    {
        uint16_t c_pat = 0xED84u;
        for (i = 15; i >= 0; i--) {
            s->s.flex.data[bit_idx++] = (c_pat >> i) & 1;
        }
    }
    /* inv.BS2: 4 bits dotting inverted */
    for (i = 0; i < 4; i++) {
        s->s.flex.data[bit_idx++] = (i & 1) ? 0 : 1;
    }
    /* inv.C pattern: 16 bits MSB first */
    {
        uint16_t cinv_pat = 0x127Bu;
        for (i = 15; i >= 0; i--) {
            s->s.flex.data[bit_idx++] = (cinv_pat >> i) & 1;
        }
    }
    
    /* DATA: 88 codewords, interleaved transmission
     * FLEX uses block interleaving: 8 codewords per block, 11 blocks total
     * For each block: transmit bit 0 of all 8 codewords, then bit 1, etc.
     * This provides burst error protection across adjacent codewords.
     */
    for (int block = 0; block < 11; block++) {
        int base_cw = block * 8;
        for (int bit = 0; bit < 32; bit++) {
            for (int cw_in_block = 0; cw_in_block < 8; cw_in_block++) {
                int cw = base_cw + cw_in_block;
                int tx_bit = (codewords[cw] >> bit) & 1;
                s->s.flex.data[bit_idx++] = tx_bit;
            }
        }
    }
    
    /* Add some trailing idle bits to ensure decoder triggers decode_data */
    for (i = 0; i < 64; i++) {
        s->s.flex.data[bit_idx++] = (i & 1) ? 1 : 0;
    }
    
    s->s.flex.datalen = bit_idx;
}

int gen_flex(signed short *buf, int buflen, struct gen_params *p, struct gen_state *s)
{
    int num = 0;
    
    if (!s || s->s.flex.bit_idx >= (int)s->s.flex.datalen)
        return 0;
    
    for (; buflen > 0; buflen--, buf++, num++) {
        /* Advance bit timing at 1600 baud */
        s->s.flex.bitph += (unsigned int)(0x10000 * FLEX_BAUD / SAMPLE_RATE);
        
        if (s->s.flex.bitph >= 0x10000u) {
            s->s.flex.bitph &= 0xFFFFu;
            s->s.flex.bit_idx++;
            if (s->s.flex.bit_idx >= (int)s->s.flex.datalen)
                return num;
        }
        
        /* Output baseband signal:
         * bit=1 -> positive amplitude -> decoder sees logic 1
         * bit=0 -> negative amplitude -> decoder sees logic 0 */
        int bit = s->s.flex.data[s->s.flex.bit_idx];
        *buf += bit ? p->ampl : -p->ampl;
    }
    
    return num;
}
