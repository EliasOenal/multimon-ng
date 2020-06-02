/*
 *      cir.c -- cn rail cir decoder and packet dump
 *      author: Alex L Manstein (alex.l.manstein@gmail.com)
 *      Copyright (C) 2020
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
#include <string.h>

#define gx 0x05B9<<(26-11)

unsigned int CheckMatrix[26][2] = {
        {119, 33554432},
        {743, 16777216},
        {943, 8388608},
        {779, 4194304},
        {857, 2097152},
        {880, 1048576},
        {440, 524288},
        {220, 262144},
        {110, 131072},
        {55,  65536},
        {711, 32768},
        {959, 16384},
        {771, 8192},
        {861, 4096},
        {882, 2048},
        {441, 1024},
        {512, 512},
        {256, 256},
        {128, 128},
        {64,  64},
        {32,  32},
        {16,  16},
        {8,   8},
        {4,   4},
        {2,   2},
        {1,   1}
};

unsigned int decode_BCH_26_16(unsigned int code, unsigned int *value) {
    // this code is adapted from: https://blog.csdn.net/u012750235/article/details/84622161
    unsigned int decode = 0;
    unsigned int res;
    decode = code;
    //2.1 calculate remainder
    for (int i = 0; i < 16; i++) {
        if ((code & 0x2000000) != 0) {
            code ^= gx;
        }
        code = code << 1;
    }
    res = code >> (26 - 10);
    if (res == 0) {
        *value = decode;
        return 0;
    }
    //2.2 correct one bit error
    for (int i = 0; i < 26; i++) {
        if (res == CheckMatrix[i][0]) {
            decode = decode ^ CheckMatrix[i][1];
            *value = decode;
            return 1;
        }
    }
    //2.3 correct two bit error
    for (int i = 0; i < 26; i++) {
        for (int j = i + 1; j < 26; j++) {
            if (res == (CheckMatrix[i][0] ^ CheckMatrix[j][0])) {
                decode = decode ^ CheckMatrix[i][1] ^ CheckMatrix[j][1];
                *value = decode;
                return 2;
            }
        }
    }
    return 3;
}

void cir_init(struct demod_state *s) {
    memset(&s->l2.uart, 0, sizeof(s->l2.uart));
    s->l2.cirfsk.rxbytes = 0;
    s->l2.cirfsk.padding = 0;
    s->l2.cirfsk.rxbitstream = 0;
    s->l2.cirfsk.rxbitcount = 0;
}


unsigned short crc16(unsigned char *ptr, int count) {
    int crc;
    char i;
    crc = 0;
    while (--count >= 0) {
        crc = crc ^ (int) *ptr++ << 8;
        i = 8;
        do {
            if (crc & 0x8000)
                crc = crc << 1 ^ 0x1021;
            else
                crc = crc << 1;
        } while (--i);
    }
    return (crc);
}

static void cir_display_package(unsigned char *buffer, uint16_t length) {
    uint16_t i;
    verbprintf(0, "CIRFSK(%d):", length);
    for (i = 0; i < length; i++) {
        verbprintf(0, "%02x ", *(buffer + i));
    }
    verbprintf(0, "\n");
}

uint16_t sync_count = 0;
uint16_t last_bit = 0;

void cir_rxbit(struct demod_state *s, unsigned char bit) {
    // According to standard TB/T 3052-2002
    // The basic wireless data frame is defined as following:
    // | bit sync (51bit) | frame sync (31bit) | mode char (8bit) | length = n (8bit) | ..payloads.. | crc16 (16bit) |
    //   101010101...101        0x0DD4259F
    //                                         | <----------------- protected by BCH(26,16) ------------------------>|
    //                                         | <- every (8+8)data and 10(FEC) = 26bit per unit ->                  |
    //                                                                                | <- n * 26 bit              ->|

    // Part I Bit Sync
    if (s->l2.cirfsk.rxbitcount == 0) {
        if (bit != last_bit) {
            sync_count++;
        } else {
            if (sync_count >= 44) {
                verbprintf(1, "CIR> Bit Sync len: %d\n", sync_count);
                s->l2.cirfsk.rxbitstream = bit; // reset RX FSM buffer
                s->l2.cirfsk.rxbitcount = 2;  // > 1 means we have a valid SYNC
                s->l2.cirfsk.rxptr = s->l2.cirfsk.rxbuf; // reset RX dump buffer
            } else if (sync_count >= 30) {
                verbprintf(2, "CIR> Bit Sync break at len: %d\n\n", sync_count);
            }
            sync_count = 0;
        }
        last_bit = bit;
    }
        // Part II Frame Sync & Length Read then Save Data
    else if (s->l2.cirfsk.rxbitcount >= 1) {
        s->l2.cirfsk.rxbitstream = (s->l2.cirfsk.rxbitstream << 1 & 0xFFFFFFFE) | bit;
        s->l2.cirfsk.rxbitcount++;
        if (s->l2.cirfsk.rxbitcount == 32) {
            // Check frame sync
            uint32_t frame_sync = 0x0dd4259f;
            uint8_t bit_error = 0;
            for (uint8_t i = 0; i < 32; i++) {
                if ((s->l2.cirfsk.rxbitstream >> i & 1) != (frame_sync >> i & 1)) {
                    bit_error++;
                }
            }
            if (bit_error < 3) {
                verbprintf(1, "CIR> Frame Sync OK (bit error:%d)\n", bit_error);
            } else {
                verbprintf(1, "CIR> Frame Sync ERR (bit error:%d)\n", bit_error);
                s->l2.cirfsk.rxbitcount = 0;
                return;
            }
            s->l2.cirfsk.rxbitstream = 0;
        } else if (s->l2.cirfsk.rxbitcount >= 58 && (s->l2.cirfsk.rxbitcount - 58) % 26 == 0) {
            uint32_t decoded;
            uint8_t errors = decode_BCH_26_16(s->l2.cirfsk.rxbitstream, &decoded);
            decoded >>= 10u;
            if (errors >= 3) {
                s->l2.cirfsk.rxbitcount = 0;
                verbprintf(2, "CIR> %d FEC too many error\n\n", s->l2.cirfsk.rxptr - s->l2.cirfsk.rxbuf);
                return;
            } else {
                if (s->l2.cirfsk.rxbitcount == 58) {
                    uint8_t length = (decoded & 0x000000ffu);
                    s->l2.cirfsk.padding = length % 2;
                    length = length + length % 2;
                    s->l2.cirfsk.rxbytes = length + 2;
                    if (length == 0) {
                        s->l2.cirfsk.rxbitcount = 0;
                        verbprintf(1, "CIR> zero length\n\n");
                        return;
                    }
                    verbprintf(1, "CIR> rx:%d (%d padding) \n", length, s->l2.cirfsk.padding);
                }
                // save data
                for (uint8_t i = 0; i < 2; i++)
                    *(s->l2.cirfsk.rxptr)++ = (decoded >> (1 - i) * 8) & 0x000000ff;
                verbprintf(3, "CIR> %d 0x%04x -> 0x%04x error:%d\n", s->l2.cirfsk.rxptr - s->l2.cirfsk.rxbuf - 2, \
                           s->l2.cirfsk.rxbitstream >> 10, decoded, errors);
                s->l2.cirfsk.rxbitstream = 0;
                // if receive completed, check crc
                if (s->l2.cirfsk.rxptr - s->l2.cirfsk.rxbuf == s->l2.cirfsk.rxbytes) {
                    verbprintf(3, "CIR> padding:%d\n", s->l2.cirfsk.padding);
                    uint8_t padding = s->l2.cirfsk.padding;
                    uint16_t crc = crc16(s->l2.cirfsk.rxbuf, s->l2.cirfsk.rxbytes - 2 - padding);
                    verbprintf(2, "CIR> crc:%04x ", crc);

                    if ((((crc >> 8) & 0x00ff) == s->l2.cirfsk.rxbuf[s->l2.cirfsk.rxbytes - 2 - padding]) && \
                        ((crc & 0x00ff) == s->l2.cirfsk.rxbuf[s->l2.cirfsk.rxbytes - 1 - padding])) {
                        verbprintf(2, "crc ok\n");
                        cir_display_package(s->l2.cirfsk.rxbuf, s->l2.cirfsk.rxbytes - padding);
                    } else {
                        verbprintf(2, "bad crc\n\n");
                    }
                    s->l2.cirfsk.rxbitcount = 0;
                }
            }
        }
    }
}