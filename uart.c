/*
 *      uart.c -- uart decoder and packet dump
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

#include "multimon.h"
#include <string.h>

/* ---------------------------------------------------------------------- */

static void disp_packet(struct demod_state *s, unsigned char *bp, unsigned int len)
{
        unsigned char i,j;

        if (!bp)
		return;
        if (!len) {
                verbprintf(0, "\n");
                return;
        }
        j = 0;
        while (len) {
                i = *bp++;
                if ((i >= 32) && (i < 128))
                        verbprintf(0, "%c",i);
                else if (i == 13) {
                        if (j) 
                                verbprintf(0, "\n");
                        j = 0;
                } else
                       verbprintf(0, "[0x%02X]",i);

                if (i >= 32)
                        j = 1;

                len--;
        }
        if (j)
                verbprintf(0, "\n");
}

/* ---------------------------------------------------------------------- */

void uart_init(struct demod_state *s)
{
	memset(&s->l2.uart, 0, sizeof(s->l2.uart));
	s->l2.uart.rxptr = s->l2.uart.rxbuf;
}

/* ---------------------------------------------------------------------- */

void uart_rxbit(struct demod_state *s, int bit)
{
	s->l2.uart.rxbitstream <<= 1;
	s->l2.uart.rxbitstream |= !!bit;
	if (!s->l2.uart.rxstate) {
		switch (s->l2.uart.rxbitstream & 0x03) {
			case 0x02:	/* start bit */
				s->l2.uart.rxstate = 1;
				s->l2.uart.rxbitbuf = 0x100;
				break;
			case 0x00:	/* no start bit */
			case 0x03:	/* consecutive stop bits*/
				if ((s->l2.uart.rxptr - s->l2.uart.rxbuf) >= 1)
					disp_packet(s, s->l2.uart.rxbuf, s->l2.uart.rxptr - s->l2.uart.rxbuf);
				s->l2.uart.rxptr = s->l2.uart.rxbuf;
				break;
		}
		return;
	}
	if (s->l2.uart.rxbitstream & 1)
		s->l2.uart.rxbitbuf |= 0x200;
//	verbprintf(7, "b=%c", '0'+(s->l2.uart.rxbitstream & 1));
	if (s->l2.uart.rxbitbuf & 1) {
		if (s->l2.uart.rxptr >= s->l2.uart.rxbuf+sizeof(s->l2.uart.rxbuf)) {
			s->l2.uart.rxstate = 0;
			disp_packet(s, s->l2.uart.rxbuf, s->l2.uart.rxptr - s->l2.uart.rxbuf);
			verbprintf(1, "Error: packet size too large\n");
			return;
		}
                if ( !(s->l2.uart.rxbitstream & 1) ) {
			s->l2.uart.rxstate = 0;
			verbprintf(1, "Error: stop bit is 0. Bad framing\n");
			return;
		}
		*s->l2.uart.rxptr++ = s->l2.uart.rxbitbuf >> 1;
//		verbprintf(6, "B=%02X ", (s->l2.uart.rxbitbuf >> 1) & 0xff);
//		verbprintf(5, "%c", (s->l2.uart.rxbitbuf >> 1) & 0xff);
		s->l2.uart.rxbitbuf = 0x100;
		s->l2.uart.rxstate = 0;
		return;
	}
      	s->l2.uart.rxbitbuf >>= 1;
}

/* ---------------------------------------------------------------------- */
