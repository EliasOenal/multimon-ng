/*
 *      demod_dumpcsv.c -- dump data in CSV format
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

#define SAMPLING_RATE 22050
#define SAMPLE_MS     22.050f

/* ---------------------------------------------------------------------- */

static void dumpcsv_init(struct demod_state *s)
{
	memset(&s->l1.dumpcsv, 0, sizeof(s->l1.dumpcsv));
}

/* ---------------------------------------------------------------------- */


static void dumpcsv_demod(struct demod_state *s, buffer_t buffer, int length)
{
	short p;
	short *src;
	float f;
	int i;

	verbprintf(2, "dump_demod length=%d, current_sequence=%d\n", length, s->l1.dumpcsv.current_sequence);


	src = buffer.sbuffer;
	for ( i=0 ; i < length ; i++, src++) {
            f = (float) ( (i + s->l1.dumpcsv.current_sequence)  / SAMPLE_MS );

	    /* cut back on superfluous plot points
	    if ( abs(p - *src ) < 40 )
		continue;
	    */

	    fprintf(stdout, "%.6f,%hd\n", f, *src);
	    p = *src;
	}

       // Save current count
       s->l1.dumpcsv.current_sequence = s->l1.dumpcsv.current_sequence + i;

}

/* ---------------------------------------------------------------------- */

const struct demod_param demod_dumpcsv = {
    "DUMPCSV", false, SAMPLING_RATE, 0, dumpcsv_init, dumpcsv_demod, NULL
};


/* ---------------------------------------------------------------------- */
