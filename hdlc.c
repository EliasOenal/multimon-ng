/*
 *      hdlc.c -- hdlc decoder and AX.25 packet dump
 *
 *      Copyright (C) 1996  
 *          Thomas Sailer (sailer@ife.ee.ethz.ch, hb9jnx@hb9w.che.eu)
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

/*
 * the CRC routines are stolen from WAMPES
 * by Dieter Deyke
 */

static const unsigned short crc_ccitt_table[] = {
        0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
        0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
        0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
        0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
        0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
        0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
        0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
        0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
        0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
        0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
        0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
        0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
        0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
        0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
        0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
        0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
        0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
        0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
        0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
        0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
        0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
        0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
        0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
        0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
        0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
        0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
        0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
        0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
        0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
        0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
        0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
        0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
};

/* ---------------------------------------------------------------------- */

static inline int check_crc_ccitt(const unsigned char *buf, int cnt)
{
        unsigned int crc = 0xffff;

        for (; cnt > 0; cnt--)
                crc = (crc >> 8) ^ crc_ccitt_table[(crc ^ *buf++) & 0xff];
        return (crc & 0xffff) == 0xf0b8;
}

/* ---------------------------------------------------------------------- */

int aprs_mode = 0;

static void aprs_print_ax25call(unsigned char *call, int is_repeater)
{
	int i;
	for (i = 0; i < 6; i++)
		if ((call[i] &0xfe) != 0x40)
			verbprintf(0, "%c",call[i] >> 1);
	int ssid = (call[6] >> 1) & 0xf;
	if (ssid)
		verbprintf(0, "-%u",ssid);
	// hack: only display "*" on the last repeater, as opposed to all that already repeated
	if (is_repeater && (call[6] & 0x80))
			verbprintf(0, "*");
}
static void aprs_disp_packet(unsigned char *bp, unsigned int len)
{
	unsigned char *hdr = bp + 14;
	unsigned int hlen = len - 14;
	// skip address fields
	while ((!(hdr[-1] & 1)) && (hlen >= 7)) {
		hdr += 7;
		hlen -= 7;
	}
	if (*hdr++ != 0x03) // Ctrl 0x03 = UI frame
		return;
	if (*hdr++ != 0xf0) // PID 0xf0 = no layer 3 protocol
		return;

	verbprintf(0, "APRS: ");
	// source call
	aprs_print_ax25call(&bp[7], 0);
	verbprintf(0, ">");
	// tocall
	aprs_print_ax25call(&bp[0], 0);
	bp += 14;
	len -= 14;
	// via callsigns
	while ((!(bp[-1] & 1)) && (len >= 7)) {
		if ((!(bp[-1] & 1)) && (len >= 7))
			verbprintf(0, ",");
		aprs_print_ax25call(&bp[0], 1);
		bp += 7;
		len -= 7;
	}
	verbprintf(0, ":");
	// end of header
	bp += 2;
	len -= 2;
	if(!len)
		return;
	while (len) {
		verbprintf(0, "%c",*bp++);
		len--;
	}
	verbprintf(0, "\n");
}

static void ax25_disp_packet(struct demod_state *s, unsigned char *bp, unsigned int len)
{
        unsigned char v1=1,cmd=0;
        unsigned char i,j;

        if (!bp || len < 10) 
		return;
#if 1
	if (!check_crc_ccitt(bp, len))
		return;
#endif
	len -= 2;
        if (bp[1] & 1) {
                /*
                 * FlexNet Header Compression
                 */
                v1 = 0;
                cmd = (bp[1] & 2) != 0;
                verbprintf(0, "%s: fm ? to ", s->dem_par->name);
                i = (bp[2] >> 2) & 0x3f;
                if (i) 
			verbprintf(0, "%c",i+0x20);
                i = ((bp[2] << 4) | ((bp[3] >> 4) & 0xf)) & 0x3f;
                if (i) 
			verbprintf(0, "%c",i+0x20);
                i = ((bp[3] << 2) | ((bp[4] >> 6) & 3)) & 0x3f;
                if (i) 
			verbprintf(0, "%c",i+0x20);
                i = bp[4] & 0x3f;
                if (i) 
			verbprintf(0, "%c",i+0x20);
                i = (bp[5] >> 2) & 0x3f;
                if (i) 
			verbprintf(0, "%c",i+0x20);
                i = ((bp[5] << 4) | ((bp[6] >> 4) & 0xf)) & 0x3f;
                if (i) 
			verbprintf(0, "%c",i+0x20);
                verbprintf(0, "-%u QSO Nr %u", bp[6] & 0xf, (bp[0] << 6) | (bp[1] >> 2));
                bp += 7;
                len -= 7;
        } else {
                /*
                 * normal header
                 */
                if (len < 15) 
			return;
		if (aprs_mode) {
			aprs_disp_packet(bp, len);
			return;
		}
                if ((bp[6] & 0x80) != (bp[13] & 0x80)) {
                        v1 = 0;
                        cmd = (bp[6] & 0x80);
                }
                verbprintf(0, "%s: fm ", s->dem_par->name);
		for(i = 7; i < 13; i++) 
                        if ((bp[i] &0xfe) != 0x40) 
                                verbprintf(0, "%c",bp[i] >> 1);
                verbprintf(0, "-%u to ",(bp[13] >> 1) & 0xf);
                for(i = 0; i < 6; i++) 
                        if ((bp[i] &0xfe) != 0x40) 
                                verbprintf(0, "%c",bp[i] >> 1);
                verbprintf(0, "-%u",(bp[6] >> 1) & 0xf);
                bp += 14;
                len -= 14;
                if ((!(bp[-1] & 1)) && (len >= 7)) 
			verbprintf(0, " via ");
                while ((!(bp[-1] & 1)) && (len >= 7)) {
                        for(i = 0; i < 6; i++) 
                                if ((bp[i] &0xfe) != 0x40) 
                                        verbprintf(0, "%c",bp[i] >> 1);
                        verbprintf(0, "-%u",(bp[6] >> 1) & 0xf);
                        bp += 7;
                        len -= 7;
                        if ((!(bp[-1] & 1)) && (len >= 7)) 
                                verbprintf(0, ",");
                }
        }
        if(!len) 
                return;
        i = *bp++;
        len--;
        j = v1 ? ((i & 0x10) ? '!' : ' ') : 
                ((i & 0x10) ? (cmd ? '+' : '-') : (cmd ? '^' : 'v'));
        if (!(i & 1)) {
                /*
                 * Info frame
                 */
                verbprintf(0, " I%u%u%c",(i >> 5) & 7,(i >> 1) & 7,j);
        } else if (i & 2) {
                /*
                 * U frame
                 */
                switch (i & (~0x10)) {
                case 0x03:
                        verbprintf(0, " UI%c",j);
                        break;
                case 0x2f:
                        verbprintf(0, " SABM%c",j);
                        break;
                case 0x43:
                        verbprintf(0, " DISC%c",j);
                        break;
                case 0x0f:
                        verbprintf(0, " DM%c",j);
                        break;
                case 0x63:
                        verbprintf(0, " UA%c",j);
                        break;
                case 0x87:
                        verbprintf(0, " FRMR%c",j);
                        break;
                default:
                        verbprintf(0, " unknown U (0x%x)%c",i & (~0x10),j);
                        break;
                }
        } else {
                /*
                 * supervisory
                 */
                switch (i & 0xf) {
                case 0x1:
                        verbprintf(0, " RR%u%c",(i >> 5) & 7,j);
                        break;
                case 0x5:
                        verbprintf(0, " RNR%u%c",(i >> 5) & 7,j);
                        break;
                case 0x9:
                        verbprintf(0, " REJ%u%c",(i >> 5) & 7,j);
                        break;
                default:
                        verbprintf(0, " unknown S (0x%x)%u%c", i & 0xf, 
                               (i >> 5) & 7, j);
                        break;
                }
        }
        if (!len) {
                verbprintf(0, "\n");
                return;
        }
        verbprintf(0, " pid=%02X\n", *bp++);
        len--;
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
                        verbprintf(0, ".");
                if (i >= 32) 
                        j = 1;
                len--;
        }
        if (j) 
                verbprintf(0, "\n");
}

/* ---------------------------------------------------------------------- */

void hdlc_init(struct demod_state *s)
{
	memset(&s->l2.hdlc, 0, sizeof(s->l2.hdlc));
}

/* ---------------------------------------------------------------------- */

void hdlc_rxbit(struct demod_state *s, int bit)
{
	s->l2.hdlc.rxbitstream <<= 1;
	s->l2.hdlc.rxbitstream |= !!bit;
	if ((s->l2.hdlc.rxbitstream & 0xff) == 0x7e) {
		if (s->l2.hdlc.rxstate && (s->l2.hdlc.rxptr - s->l2.hdlc.rxbuf) > 2)
			ax25_disp_packet(s, s->l2.hdlc.rxbuf, s->l2.hdlc.rxptr - s->l2.hdlc.rxbuf);
		s->l2.hdlc.rxstate = 1;
		s->l2.hdlc.rxptr = s->l2.hdlc.rxbuf;
		s->l2.hdlc.rxbitbuf = 0x80;
		return;
	}
	if ((s->l2.hdlc.rxbitstream & 0x7f) == 0x7f) {
		s->l2.hdlc.rxstate = 0;
		return;
	}
	if (!s->l2.hdlc.rxstate)
		return;
	if ((s->l2.hdlc.rxbitstream & 0x3f) == 0x3e) /* stuffed bit */
		return;
	if (s->l2.hdlc.rxbitstream & 1)
		s->l2.hdlc.rxbitbuf |= 0x100;
	if (s->l2.hdlc.rxbitbuf & 1) {
		if (s->l2.hdlc.rxptr >= s->l2.hdlc.rxbuf+sizeof(s->l2.hdlc.rxbuf)) {
			s->l2.hdlc.rxstate = 0;
			verbprintf(1, "Error: packet size too large\n");
			return;
		}
		*s->l2.hdlc.rxptr++ = s->l2.hdlc.rxbitbuf >> 1;
		s->l2.hdlc.rxbitbuf = 0x80;
		return;
	}
      	s->l2.hdlc.rxbitbuf >>= 1;
}

/* ---------------------------------------------------------------------- */
