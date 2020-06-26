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
#include <byteswap.h>

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

uint8_t decode_BCH_26_16(uint32_t code, uint16_t *value) {
    // this code is adapted from: https://blog.csdn.net/u012750235/article/details/84622161
    const uint32_t gx = 0x05B9 << (26 - 11);
    uint32_t decode = code;
    //2.1 calculate remainder
    for (int i = 0; i < 16; i++) {
        if ((code & 0x2000000) != 0) {
            code ^= gx;
        }
        code = code << 1;
    }
    uint32_t res = code >> (26 - 10);
    if (res == 0) {
        *value = decode >> 10;
        return 0;
    }
    //2.2 correct one bit error
    for (int i = 0; i < 26; i++) {
        if (res == CheckMatrix[i][0]) {
            decode = decode ^ CheckMatrix[i][1];
            *value = decode >> 10;
            return 1;
        }
    }
    //2.3 correct two bit error
    for (int i = 0; i < 26; i++) {
        for (int j = i + 1; j < 26; j++) {
            if (res == (CheckMatrix[i][0] ^ CheckMatrix[j][0])) {
                decode = decode ^ CheckMatrix[i][1] ^ CheckMatrix[j][1];
                *value = decode >> 10;
                return 2;
            }
        }
    }
    return 3;
}

void cir_init(struct demod_state *s) {
    memset(&s->l2.uart, 0, sizeof(s->l2.uart));
    s->l2.cirfsk.rx_buf_pos = 0;
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

static uint16_t actual_rx_length(uint16_t rx_length) {
    if (rx_length % 2 != 0) {
        rx_length += 1;
    }
    return rx_length + 2;
}

static void cir_display_package(uint8_t *buffer, uint16_t length) {
    uint16_t i;
    verbprintf(0, "CIRFSK(%d):", length);
    for (i = 0; i < length; i++) {
        verbprintf(0, "%02x ", *(buffer + i));
    }
    verbprintf(0, "\n");
}

static void cir_display_package_bad_crc(uint8_t *buffer, uint8_t *err, uint16_t length) {
    uint16_t i;
    verbprintf(1, "CIRFSK(%d)(broken):", length);
    for (i = 0; i < length / 2; i++) {
        verbprintf(1, "%02x%02x-%d ", *(buffer + i * 2), *(buffer + i * 2 + 1), *(err + i));
    }
    verbprintf(1, "\n");
}


void cir_rxbit(struct demod_state *s, unsigned char bit) {
    // According to standard TB/T 3052-2002
    // The basic wireless data frame is defined as following:
    // | bit sync (51bit) | frame sync (31bit) | mode word (8bit) | length = n (8bit) | ..payloads.. | crc16 (16bit) |
    //   101010101...101        0x0DD4259F
    //                                         | <----------------- protected by BCH(26,16) ------------------------>|
    //                                         | <-        every 16 bits data is followed by 10 bits FEC code      ->|
    //                       actual length:    |               26 bit                 | <-        n * 26 bit       ->|
    //
    // verbose level: 0 - only succeed decode (default)
    //                1 - decode failure reason
    //                2 - general decode process
    //                3 - detailed decode process
    // Waiting for sync
    uint32_t *sync_buffer = s->l2.cirfsk.sync_buffer;
    if (s->l2.cirfsk.rxbitcount == 0) {
        sync_buffer[1] = (sync_buffer[1] << 1u) | (sync_buffer[0] >> 31u);
        sync_buffer[0] = (sync_buffer[0] << 1u) | bit;
        // use last 64 bits
        static uint32_t sync_header[2] = {0x55555555, 0x0DD4259F};
        uint8_t preamble_errors = __builtin_popcountll(sync_buffer[1] ^ sync_header[0]);
        uint8_t frame_sync_errors = __builtin_popcountll(sync_buffer[0] ^ sync_header[1]);
        if ((preamble_errors + frame_sync_errors <= 4) || (preamble_errors <= 6 && frame_sync_errors <= 2)) {
            verbprintf(2, "CIR> SYNC OK error:%d %d\n", preamble_errors, frame_sync_errors);
            sync_buffer[0] = 0;
            sync_buffer[1] = 0;
            s->l2.cirfsk.rxbitstream = 0; // reset RX FSM buffer
            s->l2.cirfsk.rxbitcount = 1;
            s->l2.cirfsk.rx_buf_pos = 0; // reset RX dump buffer
            s->l2.cirfsk.fec_errors = 0;
        } else if ((preamble_errors + frame_sync_errors <= 10)) {
            verbprintf(1, "CIR> SYNC error:%d %d %x %x\n", preamble_errors, frame_sync_errors, sync_buffer[1],
                       sync_buffer[0]);
        }
    }
        // Decode data and validate
    else if (s->l2.cirfsk.rxbitcount >= 1) {
        s->l2.cirfsk.rxbitstream = (s->l2.cirfsk.rxbitstream << 1) | bit;
        if (s->l2.cirfsk.rxbitcount % 26 == 0) {
            uint16_t decoded;
            uint8_t errors = decode_BCH_26_16(s->l2.cirfsk.rxbitstream, &decoded);
            verbprintf(3, "CIR> %02d 0x%04x -> 0x%04x error:%d\n", s->l2.cirfsk.rx_buf_pos, \
                          s->l2.cirfsk.rxbitstream >> 10, decoded, errors);
            // check broken FEC
            if (errors >= 3) {
                s->l2.cirfsk.fec_errors++;
                decoded = s->l2.cirfsk.rxbitstream >> 10; // if too many error, then don't use FEC result
                if (s->l2.cirfsk.fec_errors >= 3) {
                    s->l2.cirfsk.rxbitcount = 0;
                    verbprintf(1, "CIR> %02d FEC too many error\n", s->l2.cirfsk.rx_buf_pos);
                    return;
                }
            }
            s->l2.cirfsk.rxbitstream = 0;
            // save data
            *(uint16_t *) (&s->l2.cirfsk.rxbuf[s->l2.cirfsk.rx_buf_pos]) = __bswap_16(decoded);
            s->l2.cirfsk.rx_err[s->l2.cirfsk.rx_buf_pos / 2] = errors;
            s->l2.cirfsk.rx_buf_pos += 2;
            // first 2 bytes (mode word, length)
            if (s->l2.cirfsk.rxbitcount == 26) {
                uint8_t length = decoded & 0xff;
                s->l2.cirfsk.rxlength = length;
                if (length == 0) {
                    s->l2.cirfsk.rxbitcount = 0;
                    verbprintf(1, "CIR> zero length\n");
                    return;
                }
                verbprintf(2, "CIR> Length:%d\n", length);
            }
                // if receive completed, check crc
            else if (s->l2.cirfsk.rx_buf_pos == actual_rx_length(s->l2.cirfsk.rxlength)) {
                uint16_t crc = crc16(s->l2.cirfsk.rxbuf, s->l2.cirfsk.rxlength);
                if ((((crc >> 8) & 0x00ff) == s->l2.cirfsk.rxbuf[s->l2.cirfsk.rxlength]) && \
                    ((crc & 0x00ff) == s->l2.cirfsk.rxbuf[s->l2.cirfsk.rxlength + 1])) {
                    verbprintf(2, "crc ok\n");
                    cir_display_package(s->l2.cirfsk.rxbuf, s->l2.cirfsk.rxlength + 2);
                } else {
                    verbprintf(1, "CIR> bad crc\n");
                    cir_display_package_bad_crc(s->l2.cirfsk.rxbuf, s->l2.cirfsk.rx_err,
                                                actual_rx_length(s->l2.cirfsk.rxlength));
                }
                s->l2.cirfsk.rxbitcount = 0;
                return;
            }
        }
        s->l2.cirfsk.rxbitcount++;
    }
}