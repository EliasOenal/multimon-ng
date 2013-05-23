/*
 *      demod_eea.c
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

static const unsigned int eea_freq[16] = {
    PHINC(1981), PHINC(1124), PHINC(1197), PHINC(1275),
    PHINC(1358), PHINC(1446), PHINC(1540), PHINC(1640),
    PHINC(1747), PHINC(1860), PHINC(1055), PHINC(930),
    PHINC(2400), PHINC(991), PHINC(2110), PHINC(2247)
};

/* ---------------------------------------------------------------------- */

static void eea_init(struct demod_state *s)
{
    selcall_init(s);
}

static void eea_deinit(struct demod_state *s)
{
    selcall_deinit(s);
}

static void eea_demod(struct demod_state *s, float *buffer, int length)
{
    selcall_demod(s, buffer, length, eea_freq, demod_eea.name);
}

const struct demod_param demod_eea = {
    "EEA", SAMPLE_RATE, 0, eea_init, eea_demod, eea_deinit
};



