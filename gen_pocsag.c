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
#include <string.h>
#include <stdio.h>

/*
 * POCSAG Protocol Constants
 * 
 * POCSAG uses FSK modulation at 512, 1200, or 2400 baud.
 * 
 * Frame structure:
 *   Preamble (576+ bits alternating 1/0)
 *   Sync codeword (32 bits)
 *   Batch of 16 codewords (8 frames x 2 codewords each)
 *   [Repeat sync + batch as needed]
 *
 * Codeword structure (32 bits):
 *   Bit 31: Message flag (0=address, 1=message)
 *   Bits 30-11: Data (20 bits)
 *   Bits 10-1: BCH parity (10 bits)
 *   Bit 0: Even parity
 */

#define POCSAG_SYNC         0x7CD215D8u
#define POCSAG_IDLE         0x7A89C197u
#define POCSAG_PREAMBLE_BITS 576

/*---------------------------------------------------------------------------*/

#include "bch.h"

/* Inject bit errors into a 31-bit codeword (excludes parity bit) */
static uint32_t inject_errors(uint32_t codeword, int num_errors, unsigned int *seed)
{
    int positions[3] = {-1, -1, -1};
    int i, j, pos;
    
    for (i = 0; i < num_errors && i < 3; i++) {
        /* Simple PRNG for reproducible errors */
        do {
            *seed = *seed * 1103515245 + 12345;
            pos = (*seed >> 16) % 31 + 1;  /* Bits 1-31, avoid parity bit 0 */
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

/* Build address codeword
 * Address format:
 *   Bit 31: 0 (address indicator)
 *   Bits 30-13: Address bits 20-3 (18 bits)
 *   Bits 12-11: Function code (2 bits)
 *   Bits 10-1: BCH parity
 *   Bit 0: Even parity
 *
 * Note: Address bits 2-0 are encoded in the frame position (0-7)
 */
static uint32_t build_address_codeword(uint32_t address, int function)
{
    /* Data field: 18 bits of address (bits 20-3) + 2 bits function */
    uint32_t data = ((address >> 3) << 2) | (function & 3);
    return bch_pocsag_encode(data);
}

/* Build message codeword for numeric data
 * Message format:
 *   Bit 31: 1 (message indicator)
 *   Bits 30-11: 5 BCD digits (20 bits, 4 bits each)
 *   Bits 10-1: BCH parity
 *   Bit 0: Even parity
 */
static uint32_t build_message_codeword(uint32_t data20)
{
    /* Set message flag (bit 20 of data field) */
    uint32_t data = (1u << 20) | (data20 & 0xFFFFF);
    return bch_pocsag_encode(data);
}

/* Numeric character to BCD conversion (inverse of pocsag.c conv_table) */
static int char_to_bcd(char c)
{
    switch (c) {
        case '0': return 0;
        case '1': return 8;
        case '2': return 4;
        case '3': return 12;
        case '4': return 2;
        case '5': return 10;
        case '6': return 6;
        case '7': return 14;
        case '8': return 1;
        case '9': return 9;
        case 'U': case 'u': return 13;
        case ' ': return 3;
        case '-': return 11;
        case '.': return 5;
        case '[': return 15;
        case ']': return 7;
        default:  return 3;  /* Space for unknown */
    }
}

/* Reverse bits in a 7-bit value (for alphanumeric encoding) */
static unsigned char rev7(unsigned char b)
{
    return ((b << 6) & 64) | ((b >> 6) & 1) |
           ((b << 4) & 32) | ((b >> 4) & 2) |
           ((b << 2) & 16) | ((b >> 2) & 4) |
           (b & 8);
}

/* Pack a 7-bit character into buffer at bit position n*7 (MSB first) */
static void put7(unsigned char *buf, int n, unsigned char val)
{
    int start_bit = n * 7;
    int b;
    for (b = 0; b < 7; b++) {
        int bit_pos = start_bit + b;
        int byte_idx = bit_pos / 8;
        int bit_in_byte = 7 - (bit_pos % 8);  /* MSB = bit 7 */
        if (val & (0x40 >> b))  /* MSB of val is bit 6 */
            buf[byte_idx] |= (1 << bit_in_byte);
    }
}

/* Encode message into codewords array, returns number of codewords used */
static int encode_message(const char *msg, int function, uint32_t *codewords, int max_codewords)
{
    int num_codewords = 0;
    int len = strlen(msg);
    
    if (function == 0) {
        /* Numeric encoding: 5 BCD digits per codeword */
        int i = 0;
        while (i < len && num_codewords < max_codewords) {
            uint32_t data = 0;
            int digits_packed = 0;
            for (int j = 0; j < 5 && i < len; j++, i++) {
                data = (data << 4) | char_to_bcd(msg[i]);
                digits_packed++;
            }
            /* Pad with spaces if less than 5 digits */
            while (digits_packed < 5) {
                data = (data << 4) | 3;  /* Space */
                digits_packed++;
            }
            codewords[num_codewords++] = build_message_codeword(data);
        }
    } else {
        /* Alphanumeric encoding: pack 7-bit characters */
        unsigned char buffer[256] = {0};
        int bit_count = 0;
        
        for (int i = 0; i < len; i++) {
            unsigned char c = rev7(msg[i] & 0x7F);
            put7(buffer, i, c);
            bit_count += 7;
        }
        
        /* Convert buffer to message codewords (20 bits each) */
        int nibble_idx = 0;
        int total_nibbles = (bit_count + 3) / 4;  /* Round up to nibbles */
        
        while (nibble_idx < total_nibbles && num_codewords < max_codewords) {
            uint32_t data = 0;
            /* Pack 5 nibbles (20 bits) into each codeword */
            int nibbles_in_cw = 0;
            for (int j = 0; j < 5 && nibble_idx < total_nibbles; j++, nibble_idx++) {
                int byte_pos = nibble_idx / 2;
                int nibble;
                if ((nibble_idx % 2) == 0) {
                    nibble = (buffer[byte_pos] >> 4) & 0xF;
                } else {
                    nibble = buffer[byte_pos] & 0xF;
                }
                data = (data << 4) | nibble;
                nibbles_in_cw++;
            }
            /* Pad remaining nibbles with zeros */
            while (nibbles_in_cw < 5) {
                data = (data << 4);
                nibbles_in_cw++;
            }
            codewords[num_codewords++] = build_message_codeword(data);
        }
    }
    
    return num_codewords;
}

/*---------------------------------------------------------------------------*/

/* Initialize POCSAG generator */
void gen_init_pocsag(struct gen_params *p, struct gen_state *s)
{
    int i;
    uint32_t codewords[256];
    int num_msg_codewords;
    int frame_position;
    int batch_count;
    unsigned int error_seed = 12345;  /* Seed for error injection */
    
    /* Determine frame position from address (low 3 bits) */
    frame_position = p->p.pocsag.address & 7;
    
    /* Encode the message */
    num_msg_codewords = encode_message(p->p.pocsag.message, 
                                       p->p.pocsag.function,
                                       codewords, 256);
    
    /* Calculate number of batches needed
     * Each batch has 16 codewords (8 frames x 2 codewords)
     * Address goes in frame_position, message follows
     * We need at least 1 idle codeword after message for decoder to detect end */
    int slots_needed = 1 + num_msg_codewords + 1;  /* Address + message + 1 idle for end detection */
    int slots_in_first_batch = 16 - (frame_position * 2);
    
    if (slots_needed <= slots_in_first_batch) {
        batch_count = 1;
    } else {
        batch_count = 1 + ((slots_needed - slots_in_first_batch + 15) / 16);
    }
    
    /* Calculate total bits:
     * - Preamble: 576 bits
     * - Per batch: 32 (sync) + 16*32 (codewords) = 544 bits
     */
    int total_bits = POCSAG_PREAMBLE_BITS + batch_count * (32 + 16 * 32);
    
    /* Allocate data buffer */
    s->s.pocsag.datalen = (total_bits + 7) / 8;
    if (s->s.pocsag.datalen > sizeof(s->s.pocsag.data)) {
        fprintf(stderr, "gen_pocsag: message too long\n");
        s->s.pocsag.datalen = sizeof(s->s.pocsag.data);
    }
    memset(s->s.pocsag.data, 0, sizeof(s->s.pocsag.data));
    
    /* Build the transmission */
    int bit_idx = 0;
    
    /* Preamble: alternating 1010... pattern (starts with 1) */
    for (i = 0; i < POCSAG_PREAMBLE_BITS; i++) {
        if ((i & 1) == 0)
            s->s.pocsag.data[bit_idx / 8] |= (0x80 >> (bit_idx % 8));
        bit_idx++;
    }
    
    /* Build batches */
    int msg_cw_idx = 0;
    int address_sent = 0;
    
    for (int batch = 0; batch < batch_count; batch++) {
        /* Sync codeword (32 bits, MSB first) - also inject errors if requested */
        uint32_t sync_word = POCSAG_SYNC;
        if (p->p.pocsag.errors > 0)
            sync_word = inject_errors(sync_word, p->p.pocsag.errors, &error_seed);
        for (i = 31; i >= 0; i--) {
            if (sync_word & (1u << i))
                s->s.pocsag.data[bit_idx / 8] |= (0x80 >> (bit_idx % 8));
            bit_idx++;
        }
        
        /* 16 codewords (8 frames x 2 codewords) */
        for (int frame = 0; frame < 8; frame++) {
            for (int cw = 0; cw < 2; cw++) {
                uint32_t codeword;
                
                if (!address_sent && frame == frame_position && cw == 0) {
                    /* Send address codeword */
                    codeword = build_address_codeword(p->p.pocsag.address,
                                                      p->p.pocsag.function);
                    if (p->p.pocsag.errors > 0)
                        codeword = inject_errors(codeword, p->p.pocsag.errors, &error_seed);
                    address_sent = 1;
                } else if (address_sent && msg_cw_idx < num_msg_codewords) {
                    /* Send message codeword */
                    codeword = codewords[msg_cw_idx++];
                    if (p->p.pocsag.errors > 0)
                        codeword = inject_errors(codeword, p->p.pocsag.errors, &error_seed);
                } else {
                    /* Send idle codeword */
                    codeword = POCSAG_IDLE;
                    if (p->p.pocsag.errors > 0)
                        codeword = inject_errors(codeword, p->p.pocsag.errors, &error_seed);
                }
                
                /* Output codeword (32 bits, MSB first) */
                for (i = 31; i >= 0; i--) {
                    if (codeword & (1u << i))
                        s->s.pocsag.data[bit_idx / 8] |= (0x80 >> (bit_idx % 8));
                    bit_idx++;
                }
            }
        }
    }
    
    s->s.pocsag.bit_idx = 0;
    s->s.pocsag.datalen = (bit_idx + 7) / 8;
    s->s.pocsag.baud = p->p.pocsag.baud;
    s->s.pocsag.bitph = 0;
}

/* Generate POCSAG samples */
int gen_pocsag(signed short *buf, int buflen, struct gen_params *p, struct gen_state *s)
{
    int num = 0;
    
    /* Samples per bit based on baud rate */
    /* At 22050 Hz sample rate:
     *   512 baud: ~43.07 samples/bit
     *   1200 baud: ~18.375 samples/bit
     *   2400 baud: ~9.1875 samples/bit
     */
    float samples_per_bit = 22050.0f / s->s.pocsag.baud;
    
    while (num < buflen) {
        if ((unsigned int)s->s.pocsag.bit_idx >= s->s.pocsag.datalen * 8)
            return num;  /* Done */
        
        /* Get current bit */
        int byte_idx = s->s.pocsag.bit_idx / 8;
        int bit_pos = 7 - (s->s.pocsag.bit_idx % 8);
        int bit = (s->s.pocsag.data[byte_idx] >> bit_pos) & 1;
        
        /* Output sample: negative for 1, positive for 0 (matches decoder's !bit inversion) */
        /* If inverted, flip the polarity */
        if (p->p.pocsag.invert)
            buf[num++] = bit ? p->ampl : -p->ampl;
        else
            buf[num++] = bit ? -p->ampl : p->ampl;
        
        /* Advance bit phase */
        s->s.pocsag.bitph += 1.0f;
        if (s->s.pocsag.bitph >= samples_per_bit) {
            s->s.pocsag.bitph -= samples_per_bit;
            s->s.pocsag.bit_idx++;
        }
    }
    
    return num;
}
