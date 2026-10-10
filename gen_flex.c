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
#include <stdlib.h>

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

/* Bits in one FLEX 1600/2FSK Phase-A frame written by build_flex_frame:
 *   960 preamble + 64 SYNC1 + 16 dotting + 32 FIW
 *   + 40 SYNC2 (BS2+C+inv.BS2+inv.C) + 2816 data (11*8*32) + 64 trailing
 *   = 3992 bits (one byte per bit in the data[] array)
 */
#define FLEX_BITS_PER_FRAME  3992

/* FLEX group address range: capcodes 2029568-2029583 (aw 0x1F7800-0x1F780F) */
#define FLEX_GROUP_ADDR_MIN  2029568u

/* Enrollments per frame: floor((88-1) / 2) = 43 (1 BIW + 43 addr/vec pairs) */
#define FLEX_ENROLL_PER_FRAME 43

/* Build a Short Instruction Vector word for group enrollment:
 *   bits 0-3:   4-bit checksum (nibble sum = 0xF)
 *   bits 4-6:   V=001 (Short Instruction)
 *   bits 7-9:   instr_type=0 (temp_addr assignment)
 *   bits 10-16: iAssignedFrame (7-bit delivery frame number)
 *   bits 17-20: groupbit (4-bit group slot index, 0-15)
 */
static uint32_t build_instruction_vector(unsigned int assign_frame, int groupbit)
{
    uint32_t vec = 0;
    vec |= (1u << 4);                        /* V=001 */
    vec |= (0u << 7);                        /* instr_type=0 (temp_addr) */
    vec |= ((assign_frame & 0x7F) << 10);   /* 7-bit delivery frame */
    vec |= ((groupbit & 0x0F) << 17);       /* 4-bit group slot */

    /* 4-bit checksum: sum of five nibbles + bit20 = 0xF */
    uint32_t sum = ((vec >> 4) & 0xF) + ((vec >> 8) & 0xF) +
                   ((vec >> 12) & 0xF) + ((vec >> 16) & 0xF) + ((vec >> 20) & 0x1);
    vec |= (0xF - (sum & 0xF)) & 0xF;
    return flex_encode_word(vec);
}

/* Write one FLEX 1600/2FSK frame to data[bit_idx..].
 * codewords[0..87] must be pre-built.  Returns the new bit_idx. */
static int build_flex_frame(const uint32_t *codewords, int cycle, int frame,
                             unsigned char *data, int bit_idx)
{
    int i;

    /* Preamble: 960 alternating bits (01010101...) */
    for (i = 0; i < 960; i++)
        data[bit_idx++] = (i & 1) ? 1 : 0;

    /* SYNC1: 64 bits MSB-first, inverted */
    uint64_t sync1 = build_sync1();
    add_bits_msb_inv(data, &bit_idx, sync1, 64);

    /* Dotting before FIW: 16 bits (01010101...) */
    for (i = 0; i < 16; i++)
        data[bit_idx++] = (i & 1) ? 1 : 0;

    /* FIW: 32 bits LSB-first */
    add_bits_lsb(data, &bit_idx, build_fiw(cycle, frame), 32);

    /* SYNC2: BS2(4) + C(16) + inv.BS2(4) + inv.C(16) */
    for (i = 0; i < 4; i++)
        data[bit_idx++] = (i & 1) ? 1 : 0;
    { uint16_t c = 0xED84u; for (i = 15; i >= 0; i--) data[bit_idx++] = (c >> i) & 1; }
    for (i = 0; i < 4; i++)
        data[bit_idx++] = (i & 1) ? 0 : 1;
    { uint16_t ci = 0x127Bu; for (i = 15; i >= 0; i--) data[bit_idx++] = (ci >> i) & 1; }

    /* DATA: 88 codewords, block-interleaved (8 codewords/block, 11 blocks) */
    for (int block = 0; block < 11; block++) {
        int base_cw = block * 8;
        for (int bit = 0; bit < 32; bit++)
            for (int cw = 0; cw < 8; cw++)
                data[bit_idx++] = (codewords[base_cw + cw] >> bit) & 1;
    }

    /* Trailing idle: 64 bits */
    for (i = 0; i < 64; i++)
        data[bit_idx++] = (i & 1) ? 1 : 0;

    return bit_idx;
}

void gen_init_flex(struct gen_params *p, struct gen_state *s)
{
    int i;
    uint32_t codewords[FLEX_CODEWORDS_PER_PHASE];
    uint32_t msg_words[84];  /* Max 84 content words in single-phase frame */
    int num_msg_words;
    unsigned int error_seed = 12345;

    memset(s, 0, sizeof(struct gen_state));

    /* ------------------------------------------------------------------
     * Determine how many frames we need:
     *   group call: ceil(group_count / FLEX_ENROLL_PER_FRAME) enrollment
     *               frames + 1 delivery frame
     *   plain message: 1 frame
     * ------------------------------------------------------------------ */
    int group_count  = p->p.flex.group_count;
    int enroll_frames = (group_count > 0)
                        ? (group_count + FLEX_ENROLL_PER_FRAME - 1) / FLEX_ENROLL_PER_FRAME
                        : 0;
    int total_frames  = enroll_frames + 1;   /* +1 for message/delivery frame */
    unsigned int total_bits = (unsigned int)total_frames * FLEX_BITS_PER_FRAME;

    s->s.flex.data = (unsigned char *)malloc(total_bits);
    if (!s->s.flex.data) {
        fprintf(stderr, "gen_flex: out of memory\n");
        return;
    }

    int bit_idx = 0;

    /* ------------------------------------------------------------------
     * Enrollment frames (group call only)
     *   Each frame enrolls up to FLEX_ENROLL_PER_FRAME member capcodes
     *   into group slot 0 using Short Instruction Vectors (V=001, type=0).
     *   The delivery frame is always (enroll_frames % 128) in the same
     *   cycle, ensuring it is always after the last enrollment frame.
     * ------------------------------------------------------------------ */
    unsigned int deliver_frame = (unsigned int)(enroll_frames % 128);

    for (int ef = 0; ef < enroll_frames; ef++) {
        /* Idle fill */
        for (i = 0; i < FLEX_CODEWORDS_PER_PHASE; i++)
            codewords[i] = flex_encode_word((i % 2 == 0) ? 0x0AAAAA : 0x155555);

        int members_in_frame = group_count - ef * FLEX_ENROLL_PER_FRAME;
        if (members_in_frame > FLEX_ENROLL_PER_FRAME)
            members_in_frame = FLEX_ENROLL_PER_FRAME;

        /* BIW: aoffset field encodes (desired_start - 1) because the decoder
         * does aoffset = ((biw >> 8) & 0x3) + 1.
         * We want addresses starting at codeword[1] (0 = BIW), so encode 0. */
        int aoff = 0;   /* decoder reads: 0+1 = 1 → first address at codeword[1] */
        int voff = 1 + members_in_frame;  /* vectors start right after last address */
        codewords[0] = build_biw(voff, aoff);

        for (int m = 0; m < members_in_frame; m++) {
            int member_idx = ef * FLEX_ENROLL_PER_FRAME + m;
            codewords[1 + m] = build_address(p->p.flex.group_capcodes[member_idx]);
            codewords[voff + m] = build_instruction_vector(deliver_frame, /*groupbit=*/0);
        }

        bit_idx = build_flex_frame(codewords, p->p.flex.cycle,
                                   (ef % 128), s->s.flex.data, bit_idx);
    }

    /* ------------------------------------------------------------------
     * Message / delivery frame
     * ------------------------------------------------------------------ */
    for (i = 0; i < FLEX_CODEWORDS_PER_PHASE; i++)
        codewords[i] = flex_encode_word((i % 2 == 0) ? 0x0AAAAA : 0x155555);

    num_msg_words = encode_message(p->p.flex.message, msg_words, 84, 1);

    /* For a group call: use the group delivery address (capcode =
     * FLEX_GROUP_ADDR_MIN + groupbit); for a plain message: use capcode. */
    uint32_t delivery_capcode = (group_count > 0)
                                ? (FLEX_GROUP_ADDR_MIN + 0u)  /* groupbit=0 */
                                : p->p.flex.capcode;

    int voffset = 2;
    int aoffset = 0;
    int msg_start = 3;
    int total_msg_words = num_msg_words + 1;  /* +1 for header word */

    codewords[0] = build_biw(voffset, aoffset);
    codewords[1] = build_address(delivery_capcode);
    codewords[2] = build_vector(FLEX_PAGETYPE_ALPHANUMERIC, msg_start, total_msg_words);

    /* Message header: F=11 (complete), N=0, R=1, M=0, K computed below */
    uint32_t msg_header = (3u << 11) | (1u << 19);

    /* Compute message signature over content words */
    {
        uint32_t sig_sum = 0, ch;
        ch = (msg_words[0] >> 7) & 0x7F; if (ch != 0x03) sig_sum += ch;
        ch = (msg_words[0] >> 14) & 0x7F; if (ch != 0x03) sig_sum += ch;
        for (i = 1; i < num_msg_words; i++) {
            ch = msg_words[i] & 0x7F;        if (ch != 0x03) sig_sum += ch;
            ch = (msg_words[i] >> 7) & 0x7F; if (ch != 0x03) sig_sum += ch;
            ch = (msg_words[i] >> 14) & 0x7F;if (ch != 0x03) sig_sum += ch;
        }
        msg_words[0] |= (~sig_sum) & 0x7F;
    }

    codewords[msg_start] = build_message_word(msg_header);
    for (i = 0; i < num_msg_words && (msg_start + 1 + i) < FLEX_CODEWORDS_PER_PHASE; i++)
        codewords[msg_start + 1 + i] = build_message_word(msg_words[i]);

    /* Compute K checksum */
    {
        uint32_t k_sum = 0;
        k_sum += msg_header & 0xFF;
        k_sum += (msg_header >> 8) & 0xFF;
        k_sum += (msg_header >> 16) & 0x1F;
        for (i = 0; i < num_msg_words; i++) {
            uint32_t w = msg_words[i] & 0x1FFFFF;
            k_sum += w & 0xFF; k_sum += (w >> 8) & 0xFF; k_sum += (w >> 16) & 0x1F;
        }
        msg_header |= (~k_sum) & 0x3FF;
        codewords[msg_start] = build_message_word(msg_header);
    }

    /* Error injection applies only to plain-message frames */
    if (p->p.flex.errors > 0 && group_count == 0) {
        for (i = 0; i < 10 && i < FLEX_CODEWORDS_PER_PHASE; i++)
            codewords[i] = inject_errors(codewords[i], p->p.flex.errors, &error_seed);
    }

    int delivery_frame_no = (group_count > 0) ? (int)deliver_frame : p->p.flex.frame;
    bit_idx = build_flex_frame(codewords, p->p.flex.cycle,
                               delivery_frame_no, s->s.flex.data, bit_idx);

    s->s.flex.datalen = (unsigned int)bit_idx;
}

int gen_flex(signed short *buf, int buflen, struct gen_params *p, struct gen_state *s)
{
    int num = 0;

    if (!s || !s->s.flex.data || s->s.flex.bit_idx >= (int)s->s.flex.datalen)
        return 0;

    for (; buflen > 0; buflen--, buf++, num++) {
        s->s.flex.bitph += (unsigned int)(0x10000 * FLEX_BAUD / SAMPLE_RATE);

        if (s->s.flex.bitph >= 0x10000u) {
            s->s.flex.bitph &= 0xFFFFu;
            s->s.flex.bit_idx++;
            if (s->s.flex.bit_idx >= (int)s->s.flex.datalen)
                return num;
        }

        int bit = s->s.flex.data[s->s.flex.bit_idx];
        *buf += bit ? p->ampl : -p->ampl;
    }

    return num;
}
