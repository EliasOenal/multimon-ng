/*
 *      gen_fmsfsk.c -- generate CLIP FSK 1200 bps sequences
 *
 *      Copyright (C) 2007
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

#include "gen.h"
#include <string.h>

/*---------------------------------------------------------------------------*/

struct fmsfsktx {
	unsigned int bitstream;
	unsigned int bitbuf;
	int numbits;
};

static void txb_addbyte(struct gen_state *s, struct fmsfsktx *fmsfsktx,
			unsigned char bits, unsigned char nostart)
{
	int i,j;
	if (fmsfsktx->numbits >= 8) {
		if (s->s.fmsfsk.datalen >= sizeof(s->s.fmsfsk.data))
			return;
		s->s.fmsfsk.data[s->s.fmsfsk.datalen++] = fmsfsktx->bitbuf;
		fmsfsktx->bitbuf >>= 8;
		fmsfsktx->numbits -= 8;
	}

        if (!nostart) {
		fmsfsktx->bitbuf &= ~(1 << fmsfsktx->numbits);
		fmsfsktx->numbits++;
	}
	fmsfsktx->bitbuf |= bits << fmsfsktx->numbits;
	fmsfsktx->numbits += 8;
	fmsfsktx->bitbuf |= 1 << fmsfsktx->numbits;
	fmsfsktx->numbits++;

	if (fmsfsktx->numbits >= 8) {
		if (s->s.fmsfsk.datalen >= sizeof(s->s.fmsfsk.data))
			return;
		s->s.fmsfsk.data[s->s.fmsfsk.datalen++] = fmsfsktx->bitbuf;
		fmsfsktx->bitbuf >>= 8;
		fmsfsktx->numbits -= 8;
	}
/*
 printf("bits:%02x\n", bits);
 for (i = 0; i < s->s.fmsfsk.datalen; i++) {
//  printf("%08X", s->s.fmsfsk.data[i]);
  for ( j = 0; j < 8; j++) {
   printf("%c", ((s->s.fmsfsk.data[i]) & (1<<j)) ? '1' : '0');
  }
 }
 printf("\n");
*/
}

/* ---------------------------------------------------------------------- */

void gen_init_fmsfsk(struct gen_params *p, struct gen_state *s)
{
	struct fmsfsktx fmsfsktx = { 0, 0, 0 };
	int i;

	memset(s, 0, sizeof(struct gen_state));
	s->s.fmsfsk.bitmask = 1;
//	for (i = 0; i < (p->p.fmsfsk.txdelay * (1200/100) / 8); i++)
//		txb_addbyte(s, &fmsfsktx, 0xff, 1);
	for (i = 0; i < 30; i++)
		txb_addbyte(s, &fmsfsktx, 0x55, 0);
	for (i = 0; i < 18; i++)
		txb_addbyte(s, &fmsfsktx, 0xff, 1);
	for (i = 0; i < p->p.fmsfsk.pktlen; i++)
		txb_addbyte(s, &fmsfsktx, p->p.fmsfsk.pkt[i], 0);
/* The specifications are to stop carrier inmediatly with last stop bit, but an extra one
   is needed with this demodulator, so a full 10 bit stop must be generated */
	txb_addbyte(s, &fmsfsktx, 0xff, 1);
}

int gen_fmsfsk(signed short *buf, int buflen, struct gen_params *p, struct gen_state *s)
{
	int num = 0;

	if (!s || s->s.fmsfsk.ch_idx < 0 || s->s.fmsfsk.ch_idx >= s->s.fmsfsk.datalen)
		return 0;
	for (; buflen > 0; buflen--, buf++, num++) {
		s->s.fmsfsk.bitph += 0x10000*1200 / SAMPLE_RATE;
		if (s->s.fmsfsk.bitph >= 0x10000u) {
			s->s.fmsfsk.bitph &= 0xffffu;
			s->s.fmsfsk.bitmask <<= 1;
			if (s->s.fmsfsk.bitmask >= 0x100) {
				s->s.fmsfsk.bitmask = 1;
				s->s.fmsfsk.ch_idx++;
				if (s->s.fmsfsk.ch_idx >= s->s.fmsfsk.datalen)
					return num;
			}
			s->s.fmsfsk.phinc = (s->s.fmsfsk.data[s->s.fmsfsk.ch_idx] & s->s.fmsfsk.bitmask) ? 
//				0x10000*120/SAMPLE_RATE : 0x10000*2200/SAMPLE_RATE;
				0x10000*1200/SAMPLE_RATE : 0x10000*1800/SAMPLE_RATE;
		}
		*buf += (p->ampl * COS(s->s.fmsfsk.ph)) >> 15;
		s->s.fmsfsk.ph += s->s.fmsfsk.phinc;
	}
	return num;
}


