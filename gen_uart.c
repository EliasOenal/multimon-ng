/*
 *      gen_uart.c -- generate DTMF sequences
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
#include <stdio.h>

/*---------------------------------------------------------------------------*/

struct uarttx {
	unsigned int bitstream;
	unsigned int bitbuf;
	int numbits;
};

static void txb_addbyte(struct gen_state *s, struct uarttx *uarttx,
			unsigned char bits, unsigned char nostart)
{
	int i,j;
	if (uarttx->numbits >= 8) {
		if (s->s.uart.datalen >= sizeof(s->s.uart.data))
			return;
		s->s.uart.data[s->s.uart.datalen++] = uarttx->bitbuf;
		uarttx->bitbuf >>= 8;
		uarttx->numbits -= 8;
	}

        if (!nostart) {
		uarttx->bitbuf &= ~(1 << uarttx->numbits);
		uarttx->numbits++;
	}
	uarttx->bitbuf |= bits << uarttx->numbits;
	uarttx->numbits += 8;
	uarttx->bitbuf |= 1 << uarttx->numbits;
	uarttx->numbits++;

	if (uarttx->numbits >= 8) {
		if (s->s.uart.datalen >= sizeof(s->s.uart.data))
			return;
		s->s.uart.data[s->s.uart.datalen++] = uarttx->bitbuf;
		uarttx->bitbuf >>= 8;
		uarttx->numbits -= 8;
	}
/*
  printf("bits:%02x\n", bits);
  for (i = 0; i < s->s.uart.datalen; i++) {
//    printf("%08X", s->s.uart.data[i]);
    for ( j = 0; j < 8; j++) {
     printf("%c", ((s->s.uart.data[i]) & (1<<j)) ? '1' : '0');
    }
  }
  printf("\n");
*/
}

/* ---------------------------------------------------------------------- */

void gen_init_uart(struct gen_params *p, struct gen_state *s)
{
	struct uarttx uarttx = { 0, 0, 0 };
	int i;

	memset(s, 0, sizeof(struct gen_state));
	s->s.uart.bitmask = 1;
	for (i = 0; i < (p->p.uart.txdelay * (1200/100) / 8); i++)
		txb_addbyte(s, &uarttx, 0xff, 1);
	for (i = 0; i < 30; i++)
		txb_addbyte(s, &uarttx, 0x55, 0);
	for (i = 0; i < 18; i++)
		txb_addbyte(s, &uarttx, 0xff, 1);
/*	txb_addbyte(s, &uarttx, 0xff, 0);
	txb_addbyte(s, &uarttx, 0x00, 0);
	txb_addbyte(s, &uarttx, 0x01, 0);
	txb_addbyte(s, &uarttx, 0x02, 0);
	txb_addbyte(s, &uarttx, 0x03, 0);
	txb_addbyte(s, &uarttx, 0x00, 0);
	txb_addbyte(s, &uarttx, 0x55, 0);
*/	for (i = 0; i < p->p.uart.pktlen; i++)
		txb_addbyte(s, &uarttx, p->p.uart.pkt[i], 0);
	txb_addbyte(s, &uarttx, 0xff, 1);
	txb_addbyte(s, &uarttx, 0xff, 1);
}

int gen_uart(signed short *buf, int buflen, struct gen_params *p, struct gen_state *s)
{
	int num = 0;

	if (!s || s->s.uart.ch_idx < 0 || s->s.uart.ch_idx >= s->s.uart.datalen)
		return 0;
	for (; buflen > 0; buflen--, buf++, num++) {
		s->s.uart.bitph += 0x10000*1200 / SAMPLE_RATE;
		if (s->s.uart.bitph >= 0x10000u) {
			s->s.uart.bitph &= 0xffffu;
			s->s.uart.bitmask <<= 1;
			if (s->s.uart.bitmask >= 0x100) {
				s->s.uart.bitmask = 1;
				s->s.uart.ch_idx++;
				if (s->s.uart.ch_idx >= s->s.uart.datalen)
					return num;
			}
			s->s.uart.phinc = (s->s.uart.data[s->s.uart.ch_idx] & s->s.uart.bitmask) ? 
//				0x10000*120/SAMPLE_RATE : 0x10000*2200/SAMPLE_RATE;
				0x10000*1200/SAMPLE_RATE : 0x10000*2200/SAMPLE_RATE;
		}
		*buf += (p->ampl * COS(s->s.uart.ph)) >> 15;
		s->s.uart.ph += s->s.uart.phinc;
	}
	return num;
}


