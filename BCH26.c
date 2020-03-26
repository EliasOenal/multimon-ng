/*
 *      bch26.c -- BCH(26,16) Decoder
 *      author: Alex L Manstein (alex.l.manstein@gmail.com)
 *      Copyright (C) 2020
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include "BCH26.h"

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
