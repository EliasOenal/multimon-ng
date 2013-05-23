/*
 *      demod_zvei1.c
 *
 *      Copyright (C) 2013
 *          Elias Oenal    (EliasOenal@gmail.com)
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

#define SAMPLE_RATE 22050
#define PHINC(x) ((x)*0x10000/SAMPLE_RATE)

#include "multimon.h"

static const unsigned int zvei1_freq[16] = {
    PHINC(2400), PHINC(1060), PHINC(1160), PHINC(1270),
    PHINC(1400), PHINC(1530), PHINC(1670), PHINC(1830),
    PHINC(2000), PHINC(2200), PHINC(2800), PHINC(810),
    PHINC(970), PHINC(885), PHINC(2600), PHINC(680)
};

/* ---------------------------------------------------------------------- */

static void zvei1_init(struct demod_state *s)
{
    selcall_init(s);
}

static void zvei1_deinit(struct demod_state *s)
{
    selcall_deinit(s);
}

static void zvei1_demod(struct demod_state *s, float *buffer, int length)
{
    selcall_demod(s, buffer, length, zvei1_freq, demod_zvei1.name);
}

const struct demod_param demod_zvei1 = {
    "ZVEI1", SAMPLE_RATE, 0, zvei1_init, zvei1_demod, zvei1_deinit
};



