/*
 *      demod_eia.c
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

static const unsigned int eia_freq[16] = {
    PHINC(600), PHINC(741), PHINC(882), PHINC(1023),
    PHINC(1164), PHINC(1305), PHINC(1446), PHINC(1587),
    PHINC(1728), PHINC(1869), PHINC(2151), PHINC(2433),
    PHINC(2010), PHINC(2292), PHINC(459), PHINC(1091)
};

/* ---------------------------------------------------------------------- */

static void eia_init(struct demod_state *s)
{
    selcall_init(s);
}

static void eia_deinit(struct demod_state *s)
{
    selcall_deinit(s);
}

static void eia_demod(struct demod_state *s, float *buffer, int length)
{
    selcall_demod(s, buffer, length, eia_freq, demod_eia.name);
}

const struct demod_param demod_eia = {
    "EIA", SAMPLE_RATE, 0, eia_init, eia_demod, eia_deinit
};



