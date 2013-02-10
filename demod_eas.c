/*
*      demod_eas.c -- Emergency Alert System demodulator
*
*      See http://www.nws.noaa.gov/nwr/nwrsame.htm
*
*      Copyright (C) 2000
*          A. Maitland Bottoms <bottoms@debian.org>
*
*      Licensed under same terms and based upon the
*         demod_afsk12.c -- 1200 baud AFSK demodulator
*
*         Copyright (C) 1996
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
#include "filter.h"
#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------------- */

/*
* Standard TCM3105 clock frequency: 4.4336MHz
* Bit Parameters
* The following definitions of a bit are based on a bit period equaling
* 1920 microseconds (Â± one microsecond).
* a.) The speed is 520.83 bits per second
* b.)  Logic zero is 1562.5 Hz.
* c.)  Logic one is 2083.3 Hz
* 
* preamble is 0xAB sent on wire as LSB first
*     11010101
* start of message header begins with ZCZC
      0101101011000010
* message ends with NNNN
*     01110010
*/


#define FREQ_MARK  2083.3                 // binary 1 freq, in Hz
#define FREQ_SPACE 1562.5                 // binary 0 freq, in Hz
#define FREQ_SAMP  22050                  // req'd input sampling rate, in Hz
#define BAUD       520.83                 // symbol rate, in Hz
#define PREAMBLE   ((unsigned char)0xAB)  // preamble byte, MSB first
#define HEADER_BEGIN "ZCZC"               // message begin
#define EOM "NNNN"                        // message end

// Storage options
#define MAX_MSG_LEN 268                   // maximum length of EAS message
#define MAX_HEADER_LEN 4                  // header length (begin vs end)
#define MAX_STORE_MSG 3                   // # of msgs to store and compare

// Signal processing options
#define SUBSAMP    2                      // downsampling factor
#define DLL_GAIN_UNSYNC 1/2.0             // DLL gain when unsynchronized
#define DLL_GAIN_SYNC 1/2.0               // DLL gain when synchronized
#define DLL_MAX_INC 8192                  // max DLL per-sample shift
#define INTEGRATOR_MAXVAL 10              // sampling integrator bounds
#define MIN_IDENTICAL_MSGS 2              // # of msgs which must be identical

/* ---------------------------------------------------------------------- */

#define CORRLEN ((int)(FREQ_SAMP/BAUD))
#define SPHASEINC (0x10000u*BAUD*SUBSAMP/FREQ_SAMP)

static float eascorr_mark_i[CORRLEN];
static float eascorr_mark_q[CORRLEN];
static float eascorr_space_i[CORRLEN];
static float eascorr_space_q[CORRLEN];

#define MAX(a,b) (((a)>(b))?(a):(b))
#define MIN(a,b) (((a)<(b))?(a):(b))

/* ---------------------------------------------------------------------- */

static void eas_init(struct demod_state *s)
{
    float f;
    int i;

    memset(&s->l1.eas, 0, sizeof(s->l1.eas));
    memset(&s->l2.eas, 0, sizeof(s->l2.eas));
    for (f = 0, i = 0; i < CORRLEN; i++) {
        eascorr_mark_i[i] = cos(f);
        eascorr_mark_q[i] = sin(f);
        f += 2.0*M_PI*FREQ_MARK/FREQ_SAMP;
    }
    for (f = 0, i = 0; i < CORRLEN; i++) {
        eascorr_space_i[i] = cos(f);
        eascorr_space_q[i] = sin(f);
        f += 2.0*M_PI*FREQ_SPACE/FREQ_SAMP;
    }
}

/* ---------------------------------------------------------------------- */

static char eas_allowed(char data)
{
   // determine if a character is allowed in an EAS frame
   // returns true if it is
   
   // high-byte ASCII characters are forbidden
   if (data & 0x80)
      return 0;
   if (data == 13 || data == 10)
      // LF and CR are allowed
      return 1;
   if (data >= 32 || data <= 126)
      // These text and punctuation characters are allowed
      return 1;
   
   // all other characters forbidden
   return 0;
}

static void eas_frame(struct demod_state *s, char data)
{
    int i,j = 0;
    char * ptr = 0;
    
    if (data)
    {
       // if we're idle, now we're looking for a header
       if (s->l2.eas.state == EAS_L2_IDLE)
          s->l2.eas.state = EAS_L2_HEADER_SEARCH;
       
       if (s->l2.eas.state == EAS_L2_HEADER_SEARCH && 
            s->l2.eas.headlen < MAX_HEADER_LEN)
       {
          // put it in the header buffer if we have room
       
          s->l2.eas.head_buf[s->l2.eas.headlen] = data;
          s->l2.eas.headlen++;
       
       }
       
       if (s->l2.eas.state == EAS_L2_HEADER_SEARCH &&
                  s->l2.eas.headlen >= MAX_HEADER_LEN)
       {
          // test first 4 bytes to see if they are a header
          if (!strncmp(s->l2.eas.head_buf, HEADER_BEGIN, s->l2.eas.headlen))
             // have found header. keep reading
             s->l2.eas.state = EAS_L2_READING_MESSAGE;
          else if (!strncmp(s->l2.eas.head_buf, EOM, s->l2.eas.headlen))
             // have found EOM
             s->l2.eas.state = EAS_L2_READING_EOM;
          else
          {
             // not valid, abort and clear buffer
             s->l2.eas.state = EAS_L2_IDLE;
             s->l2.eas.headlen = 0;
          }
       }
       else if (s->l2.eas.state == EAS_L2_READING_MESSAGE &&
                s->l2.eas.msglen <= MAX_MSG_LEN)
       {
          // space is available; store in message buffer
          s->l2.eas.msg_buf[s->l2.eas.msgno][s->l2.eas.msglen] = data;
          s->l2.eas.msglen++;
       }
    }
    else
    {
       // the header has ended
       // fill the rest of the buffer will NULs
       memset(&s->l2.eas.msg_buf[s->l2.eas.msgno][s->l2.eas.msglen], '\0', 
              MAX_MSG_LEN - s->l2.eas.msglen); 
       //s->l2.eas.msg_buf[s->l2.eas.msgno][s->l2.eas.msglen] = '\0';
       if (s->l2.eas.state == EAS_L2_READING_MESSAGE)
       { 
         // All EAS messages should end in a minus sign ("-")
         // trim any trailing characters
         ptr = strrchr(&s->l2.eas.msg_buf[s->l2.eas.msgno], '-');
         if (ptr)
         {
            // found. make the next character zero
            *(ptr+1) = '\0';
         }
          
         // display message if verbosity permits
         verbprintf(7, "\n");
         verbprintf(1, "%s (part): %s%s\n", s->dem_par->name, HEADER_BEGIN,
                    s->l2.eas.msg_buf[s->l2.eas.msgno]);
         
         // increment message number
         s->l2.eas.msgno += 1;
         if (s->l2.eas.msgno >= MAX_STORE_MSG)
            s->l2.eas.msgno = 0;
            
         // check for message agreement; 2 of 3 must agree
         for (i = 0; i < MAX_STORE_MSG; i++)
         {
            // if this message is empty or matches the one we've just 
            // alerted the user to, ignore it.
            if (s->l2.eas.msg_buf[i][0] == '\0' || 
                  !strncmp(s->l2.eas.last_message,
                           s->l2.eas.msg_buf[i], MAX_MSG_LEN))
               continue;
            for (j = i+1; j < MAX_STORE_MSG; j++)
            {
               // test if messages are identical and not a dupe of the
               // last message
               if (!strncmp(s->l2.eas.msg_buf[i], 
                           s->l2.eas.msg_buf[j],
                           MAX_MSG_LEN))
               {
                  // store message to prevent dupes
                  strncpy(s->l2.eas.last_message, s->l2.eas.msg_buf[j],
                        MAX_MSG_LEN);
                  
                  // raise the alert and discontinue processing
                  verbprintf(7, "\n");
                  verbprintf(0, "%s: %s%s\n", s->dem_par->name, HEADER_BEGIN,
                              s->l2.eas.last_message);
                  i = MAX_STORE_MSG;
                  break;
               }
            }
         }
         
       }
       else if (s->l2.eas.state == EAS_L2_READING_EOM)
       {
         // raise the EOM
         verbprintf(0, "%s: %s\n", s->dem_par->name, EOM);
       }
       // go back to idle
       s->l2.eas.state = EAS_L2_IDLE;
       s->l2.eas.msglen = 0;
       s->l2.eas.headlen = 0;
    }
}

static void eas_demod(struct demod_state *s, float *buffer, int length)
{
    float f;
    unsigned char curbit;
    float dll_gain;
    
    if (s->l1.eas.subsamp) {
        int numfill = SUBSAMP - s->l1.eas.subsamp;
        if (length < numfill) {
            s->l1.eas.subsamp += length;
            return;
        }
        buffer += numfill;
        length -= numfill;
        s->l1.eas.subsamp = 0;
    }
    // We use a sliding window correlator which advances by SUBSAMP
    // each time. One correlator sample is output for each SUBSAMP symbols
    for (; length >= SUBSAMP; length -= SUBSAMP, buffer += SUBSAMP) {
        f = fsqr(mac(buffer, eascorr_mark_i, CORRLEN)) +
            fsqr(mac(buffer, eascorr_mark_q, CORRLEN)) -
            fsqr(mac(buffer, eascorr_space_i, CORRLEN)) -
            fsqr(mac(buffer, eascorr_space_q, CORRLEN));
        // f > 0 if a mark (wireline 1) is detected
        // keep the last few correlator samples in s->l1.eas.dcd_shreg
        // when we've synchronized to the bit transitions, the dcd_shreg
        // will have (nearly) a single value per symbol
        s->l1.eas.dcd_shreg <<= 1;
        s->l1.eas.dcd_shreg |= (f > 0);
        // the integrator is positive for 1 bits, and negative for 0 bits
        if (f > 0 && (s->l1.eas.dcd_integrator < INTEGRATOR_MAXVAL))
        {
            s->l1.eas.dcd_integrator += 1;
        }
        else if (f < 0 && s->l1.eas.dcd_integrator > -INTEGRATOR_MAXVAL)
        {
            s->l1.eas.dcd_integrator -= 1;
        }
           
        verbprintf(9, "%c", '0'+(s->l1.afsk12.dcd_shreg & 1));
        
        
        /*
         * check if transition occurred on time
         */
        
        if (s->l2.eas.state != EAS_L2_IDLE)
        dll_gain = DLL_GAIN_SYNC;
        else
        dll_gain = DLL_GAIN_UNSYNC;

        // want transitions to take place near 0 phase
        if ((s->l1.eas.dcd_shreg ^ (s->l1.eas.dcd_shreg >> 1)) & 1) {
            if (s->l1.eas.sphase < (0x8000u-(SPHASEINC/8)))
            {
                // before center; check for decrement
                if (s->l1.eas.sphase > (SPHASEINC/2))
                {
                    s->l1.eas.sphase -= MIN((int)((s->l1.eas.sphase)*dll_gain), DLL_MAX_INC);
                    verbprintf(10,"|-%d|", MIN((int)((s->l1.eas.sphase)*dll_gain), DLL_MAX_INC));
                }
            }
            else
            {
                // after center; check for increment
                
                if (s->l1.eas.sphase < (0x10000u - SPHASEINC/2))
                {
                    s->l1.eas.sphase += MIN((int)((0x10000u - s->l1.eas.sphase)*
                                                dll_gain), DLL_MAX_INC);
                    verbprintf(10,"|+%d|", MIN((int)((0x10000u - s->l1.eas.sphase)*
                                                dll_gain), DLL_MAX_INC));
                }
            }
        }

        s->l1.eas.sphase += SPHASEINC;
        
        if (s->l1.eas.sphase >= 0x10000u) {
            // end of bit period. 
            s->l1.eas.sphase = 1;      //was &= 0xffffu;
            s->l1.eas.lasts >>= 1;
            
            // if at least half of the values in the integrator are 1, 
            // declare a 1 received
            s->l1.afsk12.lasts |= ((s->l1.eas.dcd_integrator >= 0) << 7) & 0x80u;
            
            curbit = (s->l1.eas.lasts >> 7) & 0x1u;
            verbprintf(9, "  ");
            verbprintf(7, "%c", '0'+curbit);
            
            // check for sync sequence
            // do not resync when we're reading a message!
            if (s->l1.eas.lasts == PREAMBLE
                  && s->l2.eas.state != EAS_L2_READING_MESSAGE)
            {
               // sync found; declare current offset as byte sync
               s->l1.eas.state = EAS_L1_SYNC;
               s->l1.eas.byte_counter = 0;
               verbprintf(9, " sync");
            }
            else if (s->l1.eas.state == EAS_L1_SYNC)
            {
               s->l1.eas.byte_counter++;
               if (s->l1.eas.byte_counter == 8)
               {
                  // lasts now contains one full byte
                  if (eas_allowed((char) s->l1.eas.lasts))
                  {
                     eas_frame(s, (char) s->l1.eas.lasts);
                     verbprintf(9, " %c", (char)s->l1.eas.lasts);
                  }
                  else
                  {
                     // character not valid. we have lost our sync
                     s->l1.eas.state = EAS_L1_IDLE;
                     eas_frame(s, 0x00);
                  }
                  s->l1.eas.byte_counter = 0;
               }
            }
            
            
            verbprintf(9, "\n");
        }
    }
    s->l1.eas.subsamp = length;
}

/* ---------------------------------------------------------------------- */

const struct demod_param demod_eas = {
    "EAS", FREQ_SAMP, CORRLEN, eas_init, eas_demod, NULL
};

/* ---------------------------------------------------------------------- */
