/*
 *      clip.c -- clip decoder and packet dump
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

static inline int check_sum256(const unsigned char *buf, int cnt)
{
        unsigned int sum = 0;

        for (; cnt > 0; cnt--)
                sum += *buf++;
        return (sum % 256);
}

/* ---------------------------------------------------------------------- */

unsigned char disp_parm(unsigned char *bp, unsigned char param_len)
{
        unsigned char i, len;

        len=param_len;

	while (len) {
        	i = *bp++;
		if ((i >= 32) && (i < 128)) 
                        verbprintf(0, "%c",i);
		else 
		        verbprintf(0, ".");
                len--;
	}

return param_len;
}

/*
 *  As specified in ETSI EN 300 659-3
 */

static void clip_disp_packet(struct demod_state *s, unsigned char *bp, unsigned int len)
{
        unsigned char i;
        unsigned char msg_len, param_len;
        unsigned char *ptr;

        if (!bp || len < 5) 
		return;
#if 1
	i=check_sum256(bp, len);
//	if (i != 0 && i != 0xf6) {
	if (i != 0) {
//		verbprintf(0, " CHKSUM=0x%02X SUM=0x%02X PLEN=%d\n", i, *(bp+len-1), len);
		return;
        }
#endif
	len -= 1;
        i = *bp++;
	switch(i) {
		case 0x80:	/* Call Setup */
	                verbprintf(0, "%s: CS", s->dem_par->name);
			break;

		case 0x82:	/* Message Waiting Indicator */
	                verbprintf(0, "%s: MWI", s->dem_par->name);
			break;

		case 0x04:	/* Reserved */
		case 0x84:	/* Reserved */
		case 0x85:	/* Reserved */
	                verbprintf(0, "%s: RVD len=%d", s->dem_par->name, len+1);
			break;

		case 0x86:	/* Advice of Charge */
	                verbprintf(0, "%s: AOC len=%d", s->dem_par->name, len+1);
			break;

		case 0x89:	/* Short Message Service */
	                verbprintf(0, "%s: AOC len=%d", s->dem_par->name, len+1);
			break;

		default:
	                verbprintf(0, "%s: UNKNOWN Message type (0x%02x) len=%d ", s->dem_par->name, i, len+1);
//			verbprintf(0, "\n");
			return;
	}

        if(!len) 
                return;

        msg_len = *bp++;
        if (msg_len > len)
		verbprintf(0, " broken packet len=%d\n", msg_len);

	while (msg_len > 2) {
	        i = *bp++;
                msg_len--;
		switch (i) {
                	case 0x01:	/* Date and Time */
		        	param_len = *bp++;
				msg_len -= param_len + 1;
	                        verbprintf(0, " DATE=");
				bp += disp_parm(bp, param_len);
	                        break;

        	        case 0x02:	/* Calling Line Identity */
		        	param_len = *bp++;
				msg_len -= param_len + 1;
                	        verbprintf(0, " CID=");
				bp += disp_parm(bp, param_len);
                        	break;

        	        case 0x03:	/* Called Line Identity */
		        	param_len = *bp++;
				msg_len -= param_len + 1;
                	        verbprintf(0, " CDN=");
				bp += disp_parm(bp, param_len);
                        	break;

        	        case 0x04:	/* Reason for Absence of Calling Line Identity */
		        	param_len = *bp++;
				msg_len -= param_len + 1;
                	        verbprintf(0, " RACLI=");
		        	i = *bp;
				bp += disp_parm(bp, param_len);
				switch (i) {
					case 'O':
						verbprintf(0, " Unavailable");
						break;
					case 'P':
		    				verbprintf(0, " Private (CLIR involved)");
						break;
					default:
						verbprintf(0, " (0x%02x indicator unknown)", i);
						break;
				}
                        	break;

        	        case 0x07:	/* Calling Party Name */
		        	param_len = *bp++;
				msg_len -= param_len + 1;
                	        verbprintf(0, " CNT=");
				bp += disp_parm(bp, param_len);
                        	break;

        	        case 0x08:	/* Reason for Absence of Calling Line Identity */
		        	param_len = *bp++;
				msg_len -= param_len + 1;
                	        verbprintf(0, " RACNT=");
		        	i = *bp;
				bp += disp_parm(bp, param_len);
				switch (i) {
					case 'O':
						verbprintf(0, " Unavailable");
						break;
					case 'P':
		    				verbprintf(0, " Private (CLIR involved)");
						break;
					default:
						verbprintf(0, " (0x%02x indicator unknown)", i);
						break;
				}
                        	break;

        	        case 0x0B:	/* Visual Indicator parameter type */
		        	param_len = *bp++;
				msg_len -= param_len + 1;
                	        verbprintf(0, " VI=");
		        	i = *bp;
				bp += disp_parm(bp, param_len);
				switch (i) {
					case 0:
						verbprintf(0, " Deactivation (indicator off)");
						break;
					case 0xff:
						verbprintf(0, " Activation (indicator on)");
						break;
					default:
						verbprintf(0, " (0x%02x indicator unknown)", i);
						break;
				}
				break;

        	        case 0x0D:	/* Message Identification */
				ptr = bp;
		        	param_len = *bp++;
				msg_len -= param_len + 1;
                	        verbprintf(0, " MI=");
		        	i = *bp;
				bp += disp_parm(bp, param_len);
				switch (i) {
					case 0:
		                        	verbprintf(0, " Removed Message");
						break;
					case 0x55:
		                        	verbprintf(0, " Message Reference only");
						break;
					case 0xff:
			                        verbprintf(0, " Added Message");
						break;
					default:
						verbprintf(0, " (0x%02x unknown)", i);
						break;
				}
				verbprintf(0, " Message Reference:", *(ptr+3) * 0x100 + *(ptr+4));
                        	break;

        	        case 0x11:	/* Call type */
		        	param_len = *bp++;
				msg_len -= param_len + 1;
                	        verbprintf(0, " CT=");
		        	i = *bp;
				bp += disp_parm(bp, param_len);
				switch (i) {
					case 0:
			                        verbprintf(0, " Voice call");
						break;
					case 0x02:
						verbprintf(0, " Ring-back-when-free call");
						break;
					case 0x81:
						verbprintf(0, " Message waiting call");
						break;
					default:
						verbprintf(0, " (0x%02x indicator unknown)", i);
						break;
				}
				break;

        	        case 0x13:	/* Number of Messages (Network Message System Status parameter type) */
		        	param_len = *bp++;
				msg_len -= param_len + 1;
                	        verbprintf(0, " NMSS=");
               			i = *bp++;
				verbprintf(0, "%d Number of message waiting in message system",i);
               			param_len--;
                        	break;

        	        case 0x20:	/* Charge */
				ptr = bp;
		        	param_len = *bp++;
				msg_len -= param_len + 1;
                	        verbprintf(0, " CH=");
		        	i = *(ptr+6);
				bp += disp_parm(bp, param_len);
				verbprintf(0, " CUR=%c%c%c", *ptr, *(ptr+3), *(ptr+4) );
				verbprintf(0, (i & 1)?" Free of Charge":" Normal charge" );
				verbprintf(0, (i & 2)?" Subtotal (AOC-D)":" Total (AOC-E)" );
				verbprintf(0, (i & 4)?" Credit/Debit Card Charging":" Normal charging" );
				verbprintf(0, (i & 8)?" Charging information not available":" Charging information available" );
				verbprintf(0, (i & 0x10)?" Charged units or, charged units and price per unit":" Currency amount" );
				if ( (i & 0x60) == 0 ) {
					verbprintf(0, " Current call charge");
				}
				else if ( (i & 0x60) == 1 ) {
					verbprintf(0, " Accumulated charge (last call included)");
				}
				else if ( (i & 0x60) == 2 ) {
					verbprintf(0, " Extra charge cumulated charging, e.g. call forwarded calls.");
				}
				else {
					verbprintf(0, " (for future use)");
				}
				/* byte 7-16: Cost (10 digits): Digit 1 (most significant digit) or Units (5 digits): Digit 1 (most significant digit) */
				break;

        	        case 0x21:	/* Additional Charge */
				ptr = bp;
		        	param_len = *bp++;
				msg_len -= param_len + 1;
                	        verbprintf(0, " ACH=");
		        	i = *(ptr+6);
				bp += disp_parm(bp, param_len);
				verbprintf(0, " CUR=%c%c%c", *ptr, *(ptr+3), *(ptr+4) );
				verbprintf(0, (i & 1)?" Free of Charge":" Normal charging" );
				verbprintf(0, (i & 2)?" Subtotal (AOC-D)":" Total (AOC-E)" );
				verbprintf(0, (i & 4)?" Credit/Debit Card Charging":" Normal billing" );
				verbprintf(0, (i & 8)?" Charging information not available":" Charging information available" );
				verbprintf(0, (i & 0x10)?" Charged units or, charged units and price per unit":" Currency amount" );
				if ( (i & 60) == 0 ) {
					verbprintf(0, " Current call charge");
				}
				else if ( (i & 60) == 1 ) {
					verbprintf(0, " Accumulated charge (last call included)");
				}
				else if ( (i & 60) == 2 ) {
					verbprintf(0, " Extra charge cumulated charging, e.g. call forwarded calls.");
				}
				else {
					verbprintf(0, " (for future use)");
				}
				/* byte 7-16: Cost (10 digits): Digit 1 (most significant digit) or Units (5 digits): Digit 1 (most significant digit) */
				break;

        	        case 0x50:	/* Display Information */
				ptr = bp;
		        	param_len = *bp++;
				msg_len -= param_len + 1;
		        	i = *bp++;
				if ( (i & 0x70) == 0) {
					switch ( i & 0xf ) {
						case 0:
							verbprintf(0, " Unknown or other");
							break;
						case 1:
							verbprintf(0, " Positive acknowledgement");
							break;
						case 3:
							verbprintf(0, " Negative acknowledgement");
							break;
						case 4:
							verbprintf(0, " Advertisement");
							break;
						case 5:
							verbprintf(0, " Network Provider Information");
							break;
						case 6:
							verbprintf(0, " Remote User Provided information");
							break;
						default:
							verbprintf(0, " unknown (0x%02x)", i);
							break;
					}
				}
				else if ( (i & 0x70) == 7) {
					verbprintf(0, " Reserved for network operator use");
				}
				else {
					verbprintf(0, " unknown (0x%02x)", i);
				}

				verbprintf(0, (i & 0x80)?" Stored information":" No stored information" );
                	        verbprintf(0, " SMS=");
				bp += disp_parm(bp, param_len-1);
				break;

        	        case 0x55: /* Service Information */
				msg_len--;
				break; /* This has a problem because on msg errors displays the sync burst as SI messages */
		        	param_len = *bp++;
				msg_len -= param_len + 1;
                	        verbprintf(0, " SI=");
		        	i = *bp;
				bp += disp_parm(bp, param_len);
				switch (i) {
					case 0:
						verbprintf(0, " Service not active");
						break;
					case 0xff:
						verbprintf(0, " Service active");
						break;
					default:
						verbprintf(0, " (0x%02x unknown)", i);
						break;
				}
                        	break;

	                default:
               			msg_len--;
        	                verbprintf(0, " unknown (0x%x)%c",i);
                	        break;
		}
	}
        if (!len) {
//                verbprintf(0, "\n");
                return;
        }
	verbprintf(0, "\n");
}

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

void clip_init(struct demod_state *s)
{
	memset(&s->l2.uart, 0, sizeof(s->l2.uart));
	s->l2.uart.rxptr = s->l2.uart.rxbuf;
}

/* ---------------------------------------------------------------------- */

void clip_rxbit(struct demod_state *s, int bit)
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
					clip_disp_packet(s, s->l2.uart.rxbuf, s->l2.uart.rxptr - s->l2.uart.rxbuf);
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
			clip_disp_packet(s, s->l2.uart.rxbuf, s->l2.uart.rxptr - s->l2.uart.rxbuf);
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
