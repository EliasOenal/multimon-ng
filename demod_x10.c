/*
 *      demod_x10.c -- dump data in CSV format
 *
 *      Written and placed into the public domain by
 *      Peter Shipley < peter.shipley@gmail.com >
 */


/*
 *	Useful for piping to gnuplot
 *	format is in time,value
 *	where :
 *	   time is a float in ms
 *	   value is a float from the current data stream
 *
 */

/* ---------------------------------------------------------------------- */

#include "multimon.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <signal.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>


/* ---------------------------------------------------------------------- */

static const char housecode[] = "MECKOGAINFDLPHBJ";

/* ---------------------------------------------------------------------- */

#define SAMPLING_RATE 22050
#define SAMPLE_MS     22.050f

#define SAMPLING_THRESHOLD_HIGH 13000
#define SAMPLING_THRESHOLD_LOW 9000

/* ---------------------------------------------------------------------- */

static void x10_init(struct demod_state *s)
{
	memset(&s->l1.x10, 0, sizeof(s->l1.x10));
}

/* ---------------------------------------------------------------------- */


/*
    struct l1_state_x10 {
	uint32_t current_sequence;
	uint32_t last_rise;
	short current_state;
	short current_phase;
    } x10;

current_state indicates if the last sample was "high" or "low"
current_phase phase indicator :
	0 = waiting for sync header
	1 = sync part 1
	2 = sync part 2
	3 = reading data

*/


char b0;
char b1;
char b2;
char b3;
char bi;
char bstring[42];


void printbits(unsigned char v) {
    int i; // for C89 compatability
    for(i = 7; i >= 0; i--) fputc( ('0' + ((v >> i) & 1)), stderr);
}

static void x10_report(struct demod_state *s, int clr) {
    char h, u;

    if (s->l1.x10.bi == 0) 
	return;

    fprintf(stderr, "bstring = %s\n", s->l1.x10.bstring);

    fprintf(stderr, "bytes = ");

    printbits(s->l1.x10.b[0]);
    fputs(" ", stderr);

    printbits(s->l1.x10.b[1]);
    fputs(" ", stderr);

    printbits(s->l1.x10.b[2]);
    fputs(" ", stderr);

    printbits(s->l1.x10.b[3]);
    fputs("\n", stderr);

    fprintf(stderr, "\t %.2hhX %.2hhX %.2hhX %.2hhX\n", s->l1.x10.b[0], s->l1.x10.b[1], s->l1.x10.b[2], s->l1.x10.b[3]);

    if ( s->l1.x10.bi == 32 ) {
	u = 0;
	h =  housecode[(s->l1.x10.b[0] & 0x0f)];
	if ( s->l1.x10.b[2] & 0x08 )
	    u |= 0x01;
	if ( s->l1.x10.b[2] & 0x10 )
	    u |= 0x02;
	if ( s->l1.x10.b[2] & 0x02 )
	    u |= 0x04;
	if ( s->l1.x10.b[0] & 0x20 )
	    u |= 0x08;

	u++;
	fprintf(stderr, "housecode = %c %d\n", h, u);
    }


    if ( clr || s->l1.x10.bi == 32 ) {
	s->l1.x10.bi = 0;
	memset(s->l1.x10.bstring, 0, sizeof(s->l1.x10.bstring));
	memset(s->l1.x10.b, 0, sizeof(s->l1.x10.b));
    }


}

static void x10_demod(struct demod_state *s, buffer_t buffer, int length)
{
    short *src;
    float div, f;
    int i;
    int bits = 0;

    verbprintf(2, "x10_demod length=%d, current_sequence=%d\n", length, s->l1.x10.current_sequence);


    div =  (SAMPLING_RATE/ 1000.0f );
    src = buffer.sbuffer;
    for ( i=0 ; i < length ; i++, src++) {
	uint32_t ptop;
	float ptopf;

	// Start of 9ms high preable (part 1)
	if ( s->l1.x10.current_phase == 0 ) {
	    if ( *src >=  SAMPLING_THRESHOLD_HIGH ) {
		s->l1.x10.last_rise = i + s->l1.x10.current_sequence;
		s->l1.x10.current_state = 1;
		s->l1.x10.current_phase = 1;
	    }
	    continue;

	// Start of 4.5ms low preable (part 2)
	} else if ( s->l1.x10.current_phase == 1 ) {

	    if ( *src <=  SAMPLING_THRESHOLD_LOW ) {
		int j;

		s->l1.x10.current_state = 0;

		j = i + s->l1.x10.current_sequence - s->l1.x10.last_rise;
		/*
		fprintf(stderr, "phase 1->2 drop (%d) %0.4f ms\n",
			j, (float) (j / SAMPLE_MS) );
		*/

		if (  j >= 176 && j <= 210 ) {
		    s->l1.x10.current_phase = 2;
		    s->l1.x10.last_rise = i + s->l1.x10.current_sequence;
		} else {
		    fprintf(stderr, "phase 1 fail1\n");
		    s->l1.x10.current_phase = 0;
		}
		continue;

	    } else {
		continue;
	    }

		
	// End of preable? start of data
	} else if ( s->l1.x10.current_phase == 2 ) {
	    if ( *src >=  SAMPLING_THRESHOLD_HIGH ) {
		int j;

		s->l1.x10.current_state = 1;

		j = i + s->l1.x10.current_sequence -  s->l1.x10.last_rise;
		/*
		fprintf(stderr, "phase 2->3 drop (%d) %0.4f ms\n",
			j, (float) (j / SAMPLE_MS) );
		*/

		if (  j >= 90 && j <= 104 ) {
		    // fprintf(stderr, "phase 3 drop\n");
		    s->l1.x10.current_phase = 3;
		    s->l1.x10.last_rise = i + s->l1.x10.current_sequence;
		} else {
		    fprintf(stderr, "phase 2 fail1\n");
		    s->l1.x10.current_phase = 0;
		}

	    } 
	    continue;

	// Data phase
	} else if ( s->l1.x10.current_phase == 3 ) {

	    if ( s->l1.x10.current_state == 0 ) {
		int j;

		j = (i + s->l1.x10.current_sequence) - s->l1.x10.last_rise;

		if ( *src >= SAMPLING_THRESHOLD_HIGH ) {

		    s->l1.x10.current_state = 1;
		    bits++;
		    // fprintf(stderr, "phase 3 rise (%d) %0.4f ms\n", j, (float) (j / SAMPLE_MS) );

		    // fprintf(stderr, "phase 3 b %d %d %x\n", ( s->l1.x10.bi / 8 ), ( s->l1.x10.bi % 8 ), ( 1<< ( s->l1.x10.bi % 8 )  ) );


		    s->l1.x10.last_rise = i + s->l1.x10.current_sequence;

	           if ( j > 40 ) {
		       s->l1.x10.bstring[(int)s->l1.x10.bi] = '1';
		       s->l1.x10.b[ ( s->l1.x10.bi / 8 ) ] |= ( 1<< ( s->l1.x10.bi % 8 ) );
		   } else {
		       s->l1.x10.bstring[(int)s->l1.x10.bi] = '0';
		   }
		   s->l1.x10.bi++;


		} else {
		    if ( j > 220 ) {  // if low for more then 10ms (appox)
			fprintf(stderr, "phase 3 end ( timeout )\n");
			s->l1.x10.current_phase = 0;
			fprintf(stderr, "bits = %d\n", bits);
		        x10_report(s, 1);
		    }
		}

	    } else if ( s->l1.x10.current_state == 1 ) {
		if  ( *src <  SAMPLING_THRESHOLD_LOW ) { 
		    s->l1.x10.current_state = 0;
		}
		continue;

	    } else {
		fprintf(stderr, "bad state = %d\n", s->l1.x10.current_state );
		 s->l1.x10.current_phase = 0;
		continue;
	    }

	}
    }

   // Save current count
   s->l1.x10.current_sequence = s->l1.x10.current_sequence + i;
   if ( bits ) {
       fprintf(stderr, "Bits = %d\n", bits);
       x10_report(s, 0);
   }

}


/* ---------------------------------------------------------------------- */

const struct demod_param demod_x10 = {
    "X10", false, SAMPLING_RATE, 0, x10_init, x10_demod, NULL
};


/* ---------------------------------------------------------------------- */
