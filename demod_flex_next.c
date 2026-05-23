/*
 *      demod_flex.c
 *
 *      Copyright 2004,2006,2010 Free Software Foundation, Inc.
 *      Copyright (C) 2015 Craig Shelley (craig@microtron.org.uk)
 *
 *      FLEX Radio Paging Decoder - Adapted from GNURadio for use with Multimon
 *
 *      GNU Radio is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 3, or (at your option)
 *      any later version.
 *
 *      GNU Radio is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with GNU Radio; see the file COPYING.  If not, write to
 *      the Free Software Foundation, Inc., 51 Franklin Street,
 *      Boston, MA 02110-1301, USA.
 */
/*
 *  Modification (to this file) made by Vasyl Samoilov (vasyl.samoilov@gmail.com)
 *   Decoding:
 *   - Per-word BCH error tracking; damaged words shown as '?' instead of garbage
 *   - Long address decoding: full ARIB STD-43A Table 3.8.2.2-1 (all address sets)
 *   - Address type classification: S/L/N/T/O/I/R with special address handling
 *   - All message types: ALN, NUM/SNUM/NNUM, HEX, SEC:ALN/BIN/VEN/RSV, SMSG, INS
 *   - K checksum and signature (S) verification for ALN, HEX, numeric types
 *   - Fragment reassembly with F sequence tracking, R/M carry-over, partial output
 *   - Word-level deduplication: 32-slot cache, combining on retransmission
 *   - BIW parsing: SSID1/2, Date, Time, SysInfo (timezone/DST/extended seconds)
 *   - BIW101 system message vector decode at end of VF (Section 3.9.2)
 *   - S2 C/inv.C detection with symbol buffer and replay for boundary correction
 *   Flextime (OTA network time):
 *   - Year rollover fix: 5-bit year field (base 1994) corrected for 32-year wrap
 *   - BIW TIME cross-validated against FIW-derived minute (+/-2 min tolerance)
 *   - BIW TIME: 3 consecutive forward-ticking readings required to confirm
 *   - BIW DATE: history ring with majority voting, calendar validation
 *   - BIW SYSINFO TZ: esec FIW sanity gate + history ring (3 identical readings)
 *   - FLEXTIME event output on confirmation (piped and JSON)
 *   Piped output format (9 fields, pipe-separated):
 *   - FLEX_NEXT|timestamp+tz|baud/levels|cycle.frame.phase|capcode|addr|TYPE|flags|message
 *   - Atomic line output (single verbprintf per line, no interleaving)
 *   - Timestamp: ISO 8601 local time with timezone offset
 *   - Consistent 9-field format across all message types
 *   - Type tags: ALN, NUM, SNUM, NNUM, HEX, SEC:ALN/BIN/VEN/RSV, SMSG, INS, FLEXTIME
 *   - INS sub-types: K.GRP (group setup), K.EVT (system event), K.RSV (reserved)
 *   - SMSG sub-types: K.NUM, K.SRC, K.TON, K.NID, K.RSV
 *   - Flags: frag.index/seq, N, R, M, K+/-, SIG+/-, DUP/DUP+, G<slot>
 *   - Temp group: slot reuse detection, fragment grace period for reassembly
 *   - JSON output (--json): all message types, BIW system info, flextime, fragments
 *  Modification (to this file) made by Ryan Farley (rfarley3@github)
 *   - Issue #139 !160 handle edge cases for start and end offsets (long vs short, single vs group)
 *   - Resolve type ambiguity to improve stability after Raspberry Pi compile
 *   - Compare algorithms to other open source libraries to reconcile group bit, frag bit, and capcode decode
 *   - Refactor message printing to single line, only printables, encoded % fmtstr directives
 *  Version 0.9.3v (28 Jan 2020)
 *  Modification made by bierviltje and implemented by Bruce Quinton (Zanoroy@gmail.com)
 *   - Issue #123 created by bierviltje (https://github.com/bierviltje) - Feature request: FLEX: put group messages in an array/list
 *   - This also changed the delimiter to a | rather than a space
 *  Version 0.9.2v (03 Apr 2019)
 *  Modification made by Bruce Quinton (Zanoroy@gmail.com)
 *   - Issue #120 created by PimHaarsma - Flex Tone-Only messages with short numeric body Bug fixed using code documented in the ticket system
 *  Version 0.9.1v (10 Jan 2019)
 *  Modification (to this file) made by Rob0101
 *   Fixed marking messages with K,F,C - One case had a 'C' marked as a 'K' 
 *  Version 0.9.0v (22 May 2018)
 *  Modification (to this file) made by Bruce Quinton (zanoroy@gmail.com)
 *    - Addded Define at top of file to modify the way missed group messages are reported in the debug output (default is 1; report missed capcodes on the same line)
 *                           REPORT_GROUP_CODES   1             // Report each cleared faulty group capcode : 0 = Each on a new line; 1 = All on the same line;
 *  Version 0.8.9 (20 Mar 2018)
 *  Modification (to this file) made by Bruce Quinton (zanoroy@gmail.com)
 *     - Issue #101 created by bertinhollan (https://github.com/bertinholland): Bug flex: Wrong split up group message after a data corruption frame.
 *     - Added logic to the FIW decoding that checks for any 'Group Messages' and if the frame has past them remove the group message and log output
 *     - The following settings (at the top of this file, just under these comments) have changed from:
 *                              PHASE_LOCKED_RATE    0.150
 *                              PHASE_UNLOCKED_RATE  0.150
 *       these new settings appear to work better when attempting to locate the Sync lock in the message preamble.
 *  Version 0.8.8v (20 APR 2018)
 *  Modification (to this file) made by Bruce Quinton (zanoroy@gmail.com)
 *     - Issue #101 created by bertinhollan (https://github.com/bertinholland): Bug flex: Wrong split up group message after a data corruption frame. 
 *  Version 0.8.7v (11 APR 2018)
 *  Modification (to this file) made by Bruce Quinton (zanoroy@gmail.com) and Rob0101 (as seen on github: https://github.com/rob0101)
 *     - Issue *#95 created by rob0101: '-a FLEX dropping first character of some message on regular basis'
 *     - Implemented Rob0101's suggestion of K, F and C flags to indicate the message fragmentation: 
 *         'K' message is complete and O'K' to display to the world.
 *         'F' message is a 'F'ragment and needs a 'C'ontinuation message to complete it. Message = Fragment + Continuation
 *         'C' message is a 'C'ontinuation of another fragmented message
 *  Version 0.8.6v (18 Dec 2017)
 *  Modification (to this file) made by Bruce Quinton (Zanoroy@gmail.com) on behalf of bertinhollan (https://github.com/bertinholland)
 *     - Issue #87 created by bertinhollan: Reported issue is that the flex period timeout was too short and therefore some group messages were not being processed correctly
 *                                          After some testing bertinhollan found that increasing the timeout period fixed the issue in his area. I have done further testing in my local
 *                                          area and found the change has not reduced my success rate. I think the timeout is a localisation setting and I have added "DEMOD_TIMEOUT" 
 *                                          to the definitions in the top of this file (the default value is 100 bertinhollan's prefered value, changed up from 50)
 *  Version 0.8.5v (08 Sep 2017)
 *  Modification made by Bruce Quinton (Zanoroy@gmail.com)
 *     - Issue #78 - Found a problem in the length detection sequence, modified the if statement to ensure the message length is 
 *       only checked for Aplha messages, the other types calculate thier length while decoding
 *  Version 0.8.4v (05 Sep 2017)
 *  Modification made by Bruce Quinton (Zanoroy@gmail.com)
 *     - Found a bug in the code that was not handling multiple group messages within the same frame, 
 *       and the long address bit was being miss treated in the same cases. Both issue have been fixed but further testing will help.
 *  Version 0.8.3v (22 Jun 2017)
 *  Modification made by Bruce Quinton (Zanoroy@gmail.com)
 *     - I had previously tagged Group Messages as GPN message types, 
 *       this was my own identification rather than a Flex standard type. 
 *       Now that I have cleaned up all identified (so far) issues I have changed back to the correct Flex message type of ALN (Alpha).
 *  Version 0.8.2v (21 Jun 2017)
 *  Modification made by Bruce Quinton (Zanoroy@gmail.com)
 *     - Fixed group messaging capcode issue - modified the Capcode Array to be int64_t rather than int (I was incorrectly casting the long to an int) 
 *  Version 0.8.1v (16 Jun 2017)
 *  Modification made by Bruce Quinton (Zanoroy@gmail.com)
 *     - Added Debugging to help track the group messaging issues
 *     - Improved Alpha output and removed several loops to improve CPU cycles
 *  Version 0.8v (08 Jun 2017)
 *  Modification made by Bruce Quinton (Zanoroy@gmail.com)
 *     - Added Group Messaging
 *     - Fixed Phase adjustments (phasing as part of Symbol identification)
 *     - Fixed Alpha numeric length adjustments to stop "Invalid Vector" errors
 *     - Fixed numeric message treatment
 *     - Fixed invalid identification of "unknown" messages
 *     - Added 3200 2 fsk identification to all more message types to be processed (this was a big deal for NZ)
 *     - Changed uint to int variables
 *      
 */

/* ---------------------------------------------------------------------- */

#include "multimon.h"
#include "filter.h"
#include "bch.h"
#include "cJSON.h"
#include <math.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

extern int json_mode;

/* ---------------------------------------------------------------------- */

#define FREQ_SAMP            22050
#define FILTLEN              1
#define REPORT_GROUP_CODES   1       // Report each cleared faulty group capcode : 0 = Each on a new line; 1 = All on the same line;

#define FLEX_SYNC_MARKER     0xA6C6AAAAul  // Synchronisation code marker for FLEX
#define SLICE_THRESHOLD      0.667         // For 4 level code, levels 0 and 3 have 3 times the amplitude of levels 1 and 2, so quantise at 2/3
#define SLICE_THRESHOLD_IAD  0.667         // Integrate-and-dump alternate slicer threshold
#define DC_OFFSET_FILTER     0.010         // DC Offset removal IIR filter response (seconds)
#define PHASE_LOCKED_RATE    0.045         // Correction factor for locked state
#define PHASE_UNLOCKED_RATE  0.050         // Correction factor for unlocked state
#define LOCK_LEN             24            // Number of symbols to check for phase locking (max 32)
#define IDLE_THRESHOLD       0             // Number of idle codewords allowed in data section
#define CAPCODES_INDEX       0
#define DEMOD_TIMEOUT        100           // Maximum number of periods with no zero crossings before we decide that the system is not longer within a Timing lock.
#define FLEX_GROUP_ADDR_MIN  2029568
#define FLEX_GROUP_ADDR_MAX  2029583
#define GROUP_BITS           (FLEX_GROUP_ADDR_MAX - FLEX_GROUP_ADDR_MIN + 1) // Centralized maximum of group msg cache
#define GROUP_CODE_COUNT     1000
#define GROUP_MAX_CODES      (GROUP_CODE_COUNT - 1)
#define PHASE_WORDS          88            // per spec, there are 88 4B words per frame

// S2 C-pattern per ARIB STD-43A Table 3.2-6
// S2 = BS2 + C(16 bits) + inv.BS2 + inv.C(16 bits)
// C and inv.C are used for timing verification after baud rate switch
#define FLEX_S2_C            0xED84u       // C pattern (16 bits)
#define FLEX_S2_C_INV        0x127Bu       // inv.C pattern (16 bits)
// there are 3 chars per message word (mw)
// there are at most 88 words per frame's phase buffer of a page
//   but at least 1 BIW 1 AW 1 VW, so max 85 data words (dw) for text
// each dw is 3 chars of 7b ASCII (21 bits of text, 11 bits of checksum)
// this is 256, BUT each char could need to be escaped (%, \n, \r, \t), so double it
#define MAX_ALN              512           // max possible ALN characters


enum Flex_PageTypeEnum {
  FLEX_PAGETYPE_SECURE,
  FLEX_PAGETYPE_SHORT_INSTRUCTION,
  FLEX_PAGETYPE_SHORT_MESSAGE,
  FLEX_PAGETYPE_STANDARD_NUMERIC,
  FLEX_PAGETYPE_SPECIAL_NUMERIC,
  FLEX_PAGETYPE_ALPHANUMERIC,
  FLEX_PAGETYPE_BINARY,
  FLEX_PAGETYPE_NUMBERED_NUMERIC,
  FLEX_PAGETYPE_TONE_ONLY          // synthetic: address with no vector word
};


enum Flex_StateEnum {
  FLEX_STATE_SYNC1,
  FLEX_STATE_FIW,
  FLEX_STATE_SYNC2,
  FLEX_STATE_DATA
};

struct Flex_Demodulator {
  unsigned int                sample_freq;
  double                      sample_last;
  int                         locked;
  int                         phase;
  unsigned int                sample_count;
  unsigned int                symbol_count;
  double                      envelope_sum;
  int                         envelope_count;
  uint64_t                    lock_buf;
  int                         symcount[4];
  double                      sym_sum;         // integrate-and-dump: sample sum over symbol period
  int                         sym_n;           // integrate-and-dump: sample count over symbol period
  int                         timeout;
  int                         nonconsec;
  unsigned int                baud;          // Current baud rate
};

struct Flex_GroupHandler {
  int64_t                     GroupCodes[GROUP_BITS][GROUP_CODE_COUNT];
  int                         GroupCycle[GROUP_BITS];
  int                         GroupFrame[GROUP_BITS];
  int                         GroupDelivering[GROUP_BITS]; // 1 = delivery in progress (fragments)
  unsigned int                GroupLastFragFrame[GROUP_BITS]; // abs frame of last fragment received
  int                         GroupTimeoutCount[GROUP_BITS]; // consecutive FIWs past deadline
};

struct Flex_Modulation {
  double                      symbol_rate;
  double                      envelope;
  double                      zero;
};


struct Flex_State {
  unsigned int                sync2_count;
  unsigned int                data_count;
  unsigned int                fiwcount;
  enum Flex_StateEnum         Current;
  enum Flex_StateEnum         Previous;
  // S2 C-pattern correlation (Section 3.2, Table 3.2-6)
  // C = 0xED84 (16 bits), inv.C = 0x127B
  // Detect both independently, cross-validate for boundary correction.
  uint16_t                    sync2_shiftreg;  // 16-bit shift register for C detection
  int                         sync2_c_found;   // 1 = C found
  int                         sync2_c_pos;     // symbol position where C was found
  int                         sync2_c_errs;    // bit errors in C detection
  int                         sync2_cinv_found; // 1 = inv.C found
  int                         sync2_cinv_pos;  // symbol position where inv.C was found
  int                         sync2_cinv_errs; // bit errors in inv.C detection
  unsigned char               sync2_sym_buf[4]; // buffered symbols near S2/DATA boundary
  int                         sync2_sym_buf_count;
  int                         sync2_sym_buf_start; // symbol index of first buffered symbol
};


struct Flex_Sync {
  unsigned int                sync;          // Outer synchronization code
  unsigned int                baud;          // Baudrate of SYNC2 and DATA
  unsigned int                levels;        // FSK encoding of SYNC2 and DATA
  unsigned int                polarity;      // 0=Positive (Normal) 1=Negative (Inverted)
  uint64_t                    syncbuf;
};


struct Flex_FIW {
  unsigned int                rawdata;
  unsigned int                checksum;
  unsigned int                cycleno;
  unsigned int                frameno;
  unsigned int                fix3;
  unsigned int                roaming;       // n bit: 1=roaming allowed
  unsigned int                repeat;        // r bit: 1=multiple transmission
  unsigned int                traffic;       // t3-t0 field
  unsigned int                num_tx;        // derived: 1,2,3,4
  unsigned int                td_collapse;   // t3t2 when r=1: collapse for repeat interval
};


struct Flex_Phase {
  unsigned int                buf[PHASE_WORDS];
  int                         bch_err[PHASE_WORDS]; // 0=ok, 1=uncorrectable
  int                         idle_count;
  /* Per-phase BCH stats (reset each frame) */
  int                         bch_0err;
  int                         bch_1err;
  int                         bch_2err;
  int                         bch_uncorr;
};


struct Flex_Data {
  int                         phase_toggle;
  unsigned int                data_bit_counter;
  struct Flex_Phase           PhaseA;
  struct Flex_Phase           PhaseB;
  struct Flex_Phase           PhaseC;
  struct Flex_Phase           PhaseD;
  /* Alternate phase buffers from integrate-and-dump slicer */
  struct Flex_Phase           AltA;
  struct Flex_Phase           AltB;
  struct Flex_Phase           AltC;
  struct Flex_Phase           AltD;
};


struct Flex_Decode {
  enum Flex_PageTypeEnum      type;
  int                         long_address;
  int64_t                     capcode;
  char                        addr_type;     // S=short, L=long, N=network, T=temporary, O=operator, I=info, R=reserved
  char                        phase;         // A/B/C/D - set by decode_phase
  int                         is_group;      // 1 if temporary group address
  int                         is_priority;   // 1 if in priority address range
  const char                 *sec_subtype;   // SEC sub-type string (NULL if not SEC)
  const char                 *opr_category;  // OPR category string (NULL if not OPR)
};


// Fragment reassembly buffer for multi-part messages (K/F/C flags)
#define FLEX_FRAG_MAX_SLOTS  64
#define FLEX_FRAG_MAX_LEN    2048
#define FLEX_FRAG_TIMEOUT    128  // frames before expiry (Section 4.2: up to 128 frames for shared channels)

// Deduplication and word-level combining for complete (K) messages.
// Stores raw 21-bit words + BCH status so retransmissions can be
// compared at the word level. If a retransmission has fewer errors
// than the cached copy, the better words are merged in and the
// message is re-decoded from the improved word set.
#define FLEX_DEDUP_SLOTS     32
#define FLEX_DEDUP_TIMEOUT   128  // frames before cache entry expires
#define FLEX_DEDUP_MAX_WORDS 88   // max words per message (hdr + body)

struct Flex_Fragment {
  int                         active;
  int64_t                     capcode;
  int                         type;          // page type
  int                         msg_n;         // N field (message number, 0-63) from header
  int                         msg_r;         // R field (retrieval, from initial fragment, -1 if unknown)
  int                         msg_m;         // M field (maildrop, from initial fragment, -1 if unknown)
  unsigned char               data[FLEX_FRAG_MAX_LEN];
  unsigned int                data_len;
  unsigned int                frame_received; // absolute frame when first fragment arrived
  uint32_t                    sig_sum;       // accumulated signature sum across fragments
  uint32_t                    rx_sig;        // received signature from initial fragment
  int                         sig_valid;     // 1 if all fragment words were clean
  int                         k_fail;        // 1 if any fragment had K checksum failure
  int                         expected_f;    // next expected F value (mod 3 sequence: 11->00->01->10->00...)
  int                         frag_index;    // fragment counter (0=initial, 1=first cont, ...)
  int                         f_mismatch;    // 1 if any F sequence mismatch detected (missing fragment)
  char                        phase;         // phase letter (A/B/C/D) from first fragment
  char                        addr_type;     // address type char from first fragment
  // HEX hdr2 fields (from initial fragment, carried through reassembly)
  int                         hex_blocking;  // B field: bits per char (0=16)
  int                         hex_display_rtl; // D field: 0=LTR, 1=RTL
  int                         hex_header_msg;  // H field: 1=header message
  int                         hex_status_info; // I field: 1=encoding in data
};

struct Flex_FragStore {
  struct Flex_Fragment         slots[FLEX_FRAG_MAX_SLOTS];
};

struct Flex_DedupEntry {
  int                         active;
  int64_t                     capcode;
  int                         type;          // page type (V field)
  int                         msg_n;
  unsigned int                hdr_off;       // header word offset within words[]
  unsigned int                mw1_off;       // first body word offset within words[]
  unsigned int                word_count;    // total words stored (hdr + body)
  unsigned int                body_len;      // number of body words (len)
  uint32_t                    words[FLEX_DEDUP_MAX_WORDS];
  int                         errs[FLEX_DEDUP_MAX_WORDS]; // 0=clean, 1=uncorrectable
  unsigned int                frame_seen;
};

struct Flex_DedupStore {
  struct Flex_DedupEntry      entries[FLEX_DEDUP_SLOTS];
  unsigned int                next_slot;     // round-robin insertion index
};

/* Timezone offset in minutes, indexed by 5-bit zone code (Z4-Z0).
 * Per ARIB STD-43A / Flex G1.9b specification.
 * Codes 0-15: whole-hour offsets.  Codes 16-31: fractional offsets.
 * Code 16 (10000) is reserved. */
static const int flex_tz_table[32] = {
  /*  0 (-0h)    */    0,  /*  1 (+1h)    */   60,
  /*  2 (+2h)    */  120,  /*  3 (+3h)    */  180,
  /*  4 (+4h)    */  240,  /*  5 (+5h)    */  300,
  /*  6 (+6h)    */  360,  /*  7 (+7h)    */  420,
  /*  8 (+8h)    */  480,  /*  9 (+9h)    */  540,  /* Japan */
  /* 10 (+10h)   */  600,  /* 11 (+11h)   */  660,
  /* 12 (+12h)   */  720,  /* 13 (+3h30m) */  210,
  /* 14 (+4h30m) */  270,  /* 15 (+5h30m) */  330,
  /* 16 (reserved) */  0,  /* 17 (+5h45m) */  345,
  /* 18 (+6h30m) */  390,  /* 19 (+9h30m) */  570,
  /* 20 (-3h30m) */ -210,  /* 21 (-11h)   */ -660,
  /* 22 (-10h)   */ -600,  /* 23 (-9h)    */ -540,
  /* 24 (-8h)    */ -480,  /* 25 (-7h)    */ -420,
  /* 26 (-6h)    */ -360,  /* 27 (-5h)    */ -300,
  /* 28 (-4h)    */ -240,  /* 29 (-3h)    */ -180,
  /* 30 (-2h)    */ -120,  /* 31 (-1h)    */  -60,
};

/* Over-the-air time from FLEX network BIW system info words ("flextime").
 * Each component is updated independently as BIW words arrive.
 * has_* flags track which components have been received at least once.
 * received_at_* stores gettimeofday() timestamp (microsecond precision)
 * when each component was last received.
 * frame_* stores the absolute frame number (cycle*128+frame) for age calc.
 *
 * Consistency voting: each component uses a history ring of recent BIW
 * readings.  Garbage from corrupted BCH words is random and won't cluster,
 * while the real value will appear repeatedly.  A component is "confirmed"
 * when FLEX_VOTE_THRESHOLD out of the last FLEX_VOTE_RING entries agree.
 *
 * - Time: validated against FIW-derived minute first (rejects obvious
 *         garbage), then must appear forward-ticking in the ring.
 * - Date: must be a valid calendar date, then must match or advance
 *         relative to the majority in the ring.  Garbage dates are
 *         random and won't form a cluster.
 * - Timezone: zone+DST must be identical across FLEX_VOTE_THRESHOLD
 *         entries in the ring.  Extended seconds (esec) may tick but
 *         zone+DST must agree.
 */
#define FLEX_VOTE_THRESHOLD 3
#define FLEX_VOTE_RING      8  /* history ring depth -- must be >= threshold */

struct Flex_OTA_Time {
  /* BIW type 001: Date -- confirmed (promoted after voting) */
  int has_date;
  unsigned int year;       // 1994 + raw 5-bit value, corrected for 32-year rollover
  unsigned int month;      // 1-12
  unsigned int day;        // 1-31
  unsigned int frame_date; // cycle*128+frame when date was received (0-1919)

  /* BIW type 010: Time (coarse, 7.5s resolution) -- confirmed */
  int has_time;
  unsigned int hour;       // 0-23
  unsigned int min;        // 0-59
  unsigned int sec_coarse; // 0-7 (S2-S0, each unit = 7.5 seconds)
  unsigned int frame_time; // cycle*128+frame when time was received (0-1919)

  /* BIW type 101 A=4/8: Timezone, DST, extended seconds -- confirmed */
  int has_tz;
  unsigned int tz_zone;    // 0-31 (5-bit zone code)
  int tz_offset_min;       // UTC offset in minutes (from lookup table)
  int tz_dst;              // 0=DST active, 1=standard time (per spec)
  unsigned int sec_ext;    // 0-7 (S5-S3, extends seconds to 0.9375s resolution)
  unsigned int frame_tz;   // cycle*128+frame when tz was received (0-1919)

  /* --- Voting state for time --- */
  int          vote_time_count;    // consecutive valid forward-ticking readings
  unsigned int vote_time_hour;     // last candidate hour
  unsigned int vote_time_min;      // last candidate minute
  unsigned int vote_time_sec;      // last candidate sec_coarse

  /* --- Voting state for date: history ring ---
   * Stores recent BIW date readings.  The real date is either static
   * or transitions day -> day+1 at midnight.  Garbage is random. */
  int          date_ring_count;                // entries stored (0..FLEX_VOTE_RING)
  int          date_ring_idx;                  // next write position
  unsigned int date_ring_year[FLEX_VOTE_RING];
  unsigned int date_ring_month[FLEX_VOTE_RING];
  unsigned int date_ring_day[FLEX_VOTE_RING];

  /* --- Voting state for timezone: history ring ---
   * Zone and DST must be identical to count as agreeing.
   * esec ticks but zone+DST identify the timezone. */
  int          tz_ring_count;                  // entries stored (0..FLEX_VOTE_RING)
  int          tz_ring_idx;                    // next write position
  unsigned int tz_ring_zone[FLEX_VOTE_RING];
  int          tz_ring_dst[FLEX_VOTE_RING];
  unsigned int tz_ring_esec[FLEX_VOTE_RING];   // stored for promotion

  /* --- Time mode detection (standard vs direct) ---
   * Standard: BIW TIME is constant for all frames in a cycle.
   * Direct: second field changes every 4 frames.
   * Heuristic: compare TIME across frames >= 5 apart in same cycle.
   * Same -> standard vote.  Different -> direct vote.
   * Anchor only updates after conclusive comparison or new cycle. */
  int          tmode_detected;                 // 0=standard (default), 1=direct
  int          tmode_votes_std;
  int          tmode_votes_dir;
  unsigned int tmode_anchor_min;
  unsigned int tmode_anchor_sec;
  unsigned int tmode_anchor_frame;
  unsigned int tmode_anchor_cycle;
  int          tmode_anchor_valid;
};

/* Frame space: 15 cycles x 128 frames = 1920 frames per full hour cycle.
 * Wrap-aware forward distance between two frame positions. */
#define FLEX_FRAME_SPACE 1920
/* Expire flextime components after one full cycle + 5 frames margin.
 * If age exceeds this, the data is stale (we've gone around the full
 * cycle without seeing a fresh update). */
#define FLEX_FLEXTIME_EXPIRE (FLEX_FRAME_SPACE + 5)

static unsigned int flextime_age_frames(unsigned int cur, unsigned int stored) {
  return (cur - stored + FLEX_FRAME_SPACE) % FLEX_FRAME_SPACE;
}

/* Expire stale flextime components.  Called before emitting JSON.
 * If a component's age exceeds FLEX_FLEXTIME_EXPIRE, invalidate it.
 * Also resets the corresponding vote streak so stale data doesn't
 * carry over if the component reappears later. */
static void flextime_expire(struct Flex_OTA_Time *ot, unsigned int cur_frame) {
  if (ot->has_date && flextime_age_frames(cur_frame, ot->frame_date) >= FLEX_FLEXTIME_EXPIRE) {
    ot->has_date = 0;
    ot->date_ring_count = 0;
    ot->date_ring_idx = 0;
  }
  if (ot->has_time && flextime_age_frames(cur_frame, ot->frame_time) >= FLEX_FLEXTIME_EXPIRE) {
    ot->has_time = 0;
    ot->vote_time_count = 0;
  }
  if (ot->has_tz && flextime_age_frames(cur_frame, ot->frame_tz) >= FLEX_FLEXTIME_EXPIRE) {
    ot->has_tz = 0;
    ot->tz_ring_count = 0;
    ot->tz_ring_idx = 0;
  }
}

/* Convert hour:minute to minutes-since-midnight for easy comparison. */
static inline int flextime_hhmm_to_min(unsigned int h, unsigned int m) {
  return (int)(h * 60 + m);
}

/* Validate that a date is a real calendar date (days-in-month, leap year).
 * Basic range checks (mon 1-12, day 1-31) are done by the caller before
 * this function is reached.  This catches things like Feb 30, Apr 31, etc. */
static int flextime_date_valid(unsigned int year, unsigned int mon, unsigned int day) {
  static const unsigned int days_in_month[13] = {
    0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
  };
  if (mon < 1 || mon > 12 || day < 1)
    return 0;
  unsigned int max_day = days_in_month[mon];
  if (mon == 2) {
    /* Leap year: divisible by 4, except centuries unless divisible by 400 */
    if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))
      max_day = 29;
  }
  return day <= max_day;
}

/* Format local system time as ISO 8601 with timezone offset.
 * Output: "YYYY-MM-DD HH:MM:SS+HH:MM" (25 chars + NUL).
 * buf must be at least 32 bytes. */
static void flex_local_timestamp(char *buf, size_t bufsz) {
  time_t now = time(NULL);
  struct tm lt;
#ifdef _WIN32
  localtime_s(&lt, &now);
#else
  localtime_r(&now, &lt);
#endif
  /* Compute UTC offset in seconds.
   * tm_gmtoff is a GNU/BSD extension; on Windows we compute it
   * from the difference between local and UTC representations. */
  int off;
#if defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  off = (int)lt.tm_gmtoff;
#else
  {
    struct tm gt;
#ifdef _WIN32
    gmtime_s(&gt, &now);
#else
    gmtime_r(&now, &gt);
#endif
    int local_min = lt.tm_hour * 60 + lt.tm_min;
    int utc_min = gt.tm_hour * 60 + gt.tm_min;
    int day_diff = lt.tm_yday - gt.tm_yday;
    if (day_diff > 1) day_diff = -1;
    if (day_diff < -1) day_diff = 1;
    off = (local_min - utc_min + day_diff * 1440) * 60;
  }
#endif
  char sign = off >= 0 ? '+' : '-';
  if (off < 0) off = -off;
  int off_h = off / 3600;
  int off_m = (off % 3600) / 60;
  snprintf(buf, bufsz, "%04d-%02d-%02d %02d:%02d:%02d%c%02d:%02d",
           lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
           lt.tm_hour, lt.tm_min, lt.tm_sec,
           sign, off_h, off_m);
}

/* Cross-validate a single BIW reading against the FIW-derived minute.
 * The FIW minute is computed fresh from cycle/frame -- it's the ground truth.
 * The BIW minute is the incoming reading we're checking.
 * Returns 1 if they agree within +/-tolerance, handling the :59/:00 wrap. */
#define FLEX_TIME_CROSS_TOLERANCE 2  /* minutes */

static int flextime_fiw_validate(unsigned int biw_min,
                                 unsigned int cycleno, unsigned int frameno) {
  (void)frameno;
  /* Accept BIW minute within the cycle's 4-minute span [cycle*4..cycle*4+3]
   * mod 60.  This covers both standard (Frame 0 time) and direct (current
   * frame time) conventions. */
  int base_min = (int)(cycleno * 4) % 60;
  int bmin = (int)biw_min;
  int diff = bmin - base_min;
  if (diff < -30) diff += 60;
  if (diff > 30) diff -= 60;
  return diff >= 0 && diff <= 3;
}

/* Vote on a BIW TIME reading.  Processing order:
 *
 *  1. Validate the new BIW reading against the FIW-derived minute.
 *     The FIW is computed from cycle/frame and is the independent
 *     ground truth.  At hour boundaries the BIW minute can be off
 *     by up to +/-FLEX_TIME_CROSS_TOLERANCE from the FIW minute
 *     (e.g. BIW=59 while FIW wrapped to 0, or BIW=0 while FIW
 *     is still at 59).  If it fails, reject and reset.
 *
 *  2. Check the new BIW reading against the BIW history for
 *     forward-ticking consistency (time must stay same or advance).
 *     A backward jump across midnight (23:xx -> 0:xx) is allowed.
 *     If it goes backward otherwise, reset the vote streak.
 *
 *  3. Store the reading in the BIW history ring and advance the
 *     vote count.  Promote to confirmed after FLEX_VOTE_THRESHOLD
 *     consecutive valid readings.
 *
 * Returns 1 if the reading was promoted (vote count reached threshold). */
static int flextime_vote_time(struct Flex_OTA_Time *ot,
                              unsigned int hour, unsigned int min, unsigned int sec,
                              unsigned int cycleno, unsigned int frameno,
                              unsigned int cur_abs_frame) {
  /* Step 1: validate new BIW reading against FIW-derived minute */
  if (!flextime_fiw_validate(min, cycleno, frameno)) {
    int fiw_seconds = (int)(cycleno * 240 + frameno * 240 / 128);
    verbprintf(3, "FLEX_NEXT: flextime TIME FIW cross-check failed: BIW %02u:%02u vs FIW min %d, resetting vote\n",
               hour, min, fiw_seconds / 60);
    ot->vote_time_count = 0;
    return 0;
  }

  /* Step 2: check forward-ticking against previous BIW candidate */
  if (ot->vote_time_count > 0) {
    int prev = flextime_hhmm_to_min(ot->vote_time_hour, ot->vote_time_min);
    int curr = flextime_hhmm_to_min(hour, min);
    int delta = curr - prev;
    /* Midnight wrap: prev=23:5x curr=0:0x -> delta ~ -1430, treat as forward */
    if (delta < -720) delta += 1440;
    if (delta < 0) {
      /* Time went backward (not a midnight wrap) -- reset streak */
      verbprintf(3, "FLEX_NEXT: flextime TIME went backward: %02u:%02u -> %02u:%02u, resetting vote\n",
                 ot->vote_time_hour, ot->vote_time_min, hour, min);
      ot->vote_time_count = 0;
      /* Fall through to start a new streak with this reading */
    }
  }

  /* Step 3: record candidate and advance vote */
  ot->vote_time_hour = hour;
  ot->vote_time_min = min;
  ot->vote_time_sec = sec;
  ot->vote_time_count++;

  /* Step 4: time mode detection (standard vs direct).
   * Compare with anchor from same cycle, >= 5 frames apart.
   * In direct mode, second field changes every 4 frames, so
   * frames >= 5 apart in different coarse groups MUST differ.
   * In standard mode, TIME is constant for the entire cycle. */
  if (ot->tmode_anchor_valid && ot->tmode_anchor_cycle == cycleno) {
    int fdiff = (int)frameno - (int)ot->tmode_anchor_frame;
    if (fdiff < 0) fdiff = -fdiff;
    if (fdiff >= 5) {
      int same = (min == ot->tmode_anchor_min &&
                  sec == ot->tmode_anchor_sec);
      if (same) {
        ot->tmode_votes_std++;
        ot->tmode_votes_dir = 0;
      } else {
        ot->tmode_votes_dir++;
        ot->tmode_votes_std = 0;
      }
      if (ot->tmode_votes_std >= 3 && ot->tmode_detected != 0) {
        ot->tmode_detected = 0;
        verbprintf(1, "FLEX_NEXT: flextime mode detected: STANDARD (Frame 0 time)\n");
      }
      if (ot->tmode_votes_dir >= 3 && ot->tmode_detected != 1) {
        ot->tmode_detected = 1;
        verbprintf(1, "FLEX_NEXT: flextime mode detected: DIRECT (current frame time)\n");
      }
      /* Update anchor after comparison */
      ot->tmode_anchor_min = min;
      ot->tmode_anchor_sec = sec;
      ot->tmode_anchor_frame = frameno;
    }
  } else {
    /* New cycle or first observation: set anchor */
    ot->tmode_anchor_min = min;
    ot->tmode_anchor_sec = sec;
    ot->tmode_anchor_frame = frameno;
    ot->tmode_anchor_cycle = cycleno;
    ot->tmode_anchor_valid = 1;
  }

  if (ot->vote_time_count >= FLEX_VOTE_THRESHOLD) {
    /* Promote to confirmed */
    ot->hour = hour;
    ot->min = min;
    ot->sec_coarse = sec;
    ot->has_time = 1;
    ot->frame_time = cur_abs_frame;
    verbprintf(3, "FLEX_NEXT: flextime TIME confirmed after %d votes: %02u:%02u:%04.1f\n",
               FLEX_VOTE_THRESHOLD, hour, min, sec * 7.5);
    return 1;
  }
  return 0;
}

/* Compute the next calendar day from a given date.
 * Handles month-end rollover (incl. leap year) and Dec 31 -> Jan 1. */
static void flextime_next_day(unsigned int y, unsigned int m, unsigned int d,
                              unsigned int *ny, unsigned int *nm, unsigned int *nd) {
  static const unsigned int dim[13] = {
    0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
  };
  unsigned int max_d = dim[m];
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)))
    max_d = 29;
  if (d < max_d) {
    *ny = y; *nm = m; *nd = d + 1;
  } else if (m < 12) {
    *ny = y; *nm = m + 1; *nd = 1;
  } else {
    *ny = y + 1; *nm = 1; *nd = 1;
  }
}

/* Check if two dates "agree" -- they are the same date, or one is the
 * next calendar day of the other (handles day/month/year rollover). */
static int flextime_dates_agree(unsigned int y1, unsigned int m1, unsigned int d1,
                                unsigned int y2, unsigned int m2, unsigned int d2) {
  /* Same date */
  if (y1 == y2 && m1 == m2 && d1 == d2)
    return 1;
  /* Check if (y2,m2,d2) is the next day after (y1,m1,d1) */
  unsigned int ny, nm, nd;
  flextime_next_day(y1, m1, d1, &ny, &nm, &nd);
  if (ny == y2 && nm == m2 && nd == d2)
    return 1;
  /* Check the reverse: (y1,m1,d1) is the next day after (y2,m2,d2) */
  flextime_next_day(y2, m2, d2, &ny, &nm, &nd);
  if (ny == y1 && nm == m1 && nd == d1)
    return 1;
  return 0;
}

/* Vote on a BIW DATE reading using a history ring.
 * The real date is either static or transitions to the next calendar day
 * at midnight.  Garbage from corrupted BCH words is random and won't
 * cluster.  We push every valid calendar date into the ring, then count
 * how many entries agree with the new reading.  If >= threshold, promote.
 * Returns 1 if the reading was promoted. */
static int flextime_vote_date(struct Flex_OTA_Time *ot,
                              unsigned int year, unsigned int mon, unsigned int day,
                              unsigned int cur_abs_frame) {
  /* Step 1: calendar validation */
  if (!flextime_date_valid(year, mon, day)) {
    verbprintf(3, "FLEX_NEXT: flextime DATE invalid calendar: %04u-%02u-%02u, discarding\n",
               year, mon, day);
    return 0;  /* don't push garbage into the ring */
  }

  /* Step 2: push into ring (even before counting -- it participates) */
  ot->date_ring_year[ot->date_ring_idx] = year;
  ot->date_ring_month[ot->date_ring_idx] = mon;
  ot->date_ring_day[ot->date_ring_idx] = day;
  ot->date_ring_idx = (ot->date_ring_idx + 1) % FLEX_VOTE_RING;
  if (ot->date_ring_count < FLEX_VOTE_RING)
    ot->date_ring_count++;

  /* Step 3: count how many ring entries agree with this reading */
  int agree = 0;
  for (int i = 0; i < ot->date_ring_count; i++) {
    if (flextime_dates_agree(year, mon, day,
                             ot->date_ring_year[i], ot->date_ring_month[i], ot->date_ring_day[i]))
      agree++;
  }

  if (agree >= FLEX_VOTE_THRESHOLD) {
    /* Promote the newest value (at a day boundary, prefer the later date) */
    ot->year = year;
    ot->month = mon;
    ot->day = day;
    ot->has_date = 1;
    ot->frame_date = cur_abs_frame;
    verbprintf(3, "FLEX_NEXT: flextime DATE confirmed (%d/%d agree): %04u-%02u-%02u\n",
               agree, ot->date_ring_count, year, mon, day);
    return 1;
  }
  return 0;
}

/* Vote on a BIW SYSINFO timezone reading using a history ring.
 * Zone, DST, and esec must all be identical to agree.  The timezone
 * doesn't change -- if the ring confirms it, the esec is valid too.
 *
 * Before entering the ring, esec (S5-S3) is sanity-checked against the
 * FIW-derived expected value.  Each sec_ext tick = 7.5s, and the FIW
 * frame position tells us which 7.5s window we're in:
 *   expected_ext = ((cycleno * 128 + frameno) % 32) / 4
 * With +/-1 tolerance this rejects ~62% of random garbage and prevents
 * corrupted sysinfo words from polluting the ring.
 *
 * Returns 1 if the reading was promoted. */
#define FLEX_ESEC_TOLERANCE 1
static int flextime_vote_tz(struct Flex_OTA_Time *ot,
                            unsigned int zone, int dst, unsigned int esec,
                            int tz_offset_min,
                            unsigned int cycleno, unsigned int frameno,
                            unsigned int cur_abs_frame) {
  /* Gate: sanity-check esec against FIW-derived expected value */
  {
    int expected_ext = (int)(((cycleno * 128 + frameno) % 32) / 4);
    int diff = (int)esec - expected_ext;
    /* Handle wrap at the minute boundary (esec=7 vs expected=0, or vice versa) */
    if (diff > 4) diff -= 8;
    if (diff < -4) diff += 8;
    if (diff < 0) diff = -diff;
    if (diff > FLEX_ESEC_TOLERANCE) {
      verbprintf(3, "FLEX_NEXT: flextime TZ esec FIW sanity failed: esec=%u expected=%d (c=%u f=%u), discarding\n",
                 esec, expected_ext, cycleno, frameno);
      return 0;  /* don't pollute the ring */
    }
  }

  /* Push into ring */
  ot->tz_ring_zone[ot->tz_ring_idx] = zone;
  ot->tz_ring_dst[ot->tz_ring_idx] = dst;
  ot->tz_ring_esec[ot->tz_ring_idx] = esec;
  ot->tz_ring_idx = (ot->tz_ring_idx + 1) % FLEX_VOTE_RING;
  if (ot->tz_ring_count < FLEX_VOTE_RING)
    ot->tz_ring_count++;

  /* Count how many ring entries are identical (zone + DST + esec) */
  int agree = 0;
  for (int i = 0; i < ot->tz_ring_count; i++) {
    if (ot->tz_ring_zone[i] == zone && ot->tz_ring_dst[i] == dst &&
        ot->tz_ring_esec[i] == esec)
      agree++;
  }

  if (agree >= FLEX_VOTE_THRESHOLD) {
    ot->tz_zone = zone;
    ot->tz_offset_min = tz_offset_min;
    ot->tz_dst = dst;
    ot->sec_ext = esec;
    ot->has_tz = 1;
    ot->frame_tz = cur_abs_frame;
    verbprintf(3, "FLEX_NEXT: flextime TZ confirmed (%d/%d agree): zone=%u (%+dmin) DST=%d esec=%u\n",
               agree, ot->tz_ring_count, zone, tz_offset_min, dst, esec);
    return 1;
  }
  return 0;
}


struct Flex_Next {
  struct Flex_Demodulator     Demodulator;
  struct Flex_Modulation      Modulation;
  struct Flex_State           State;
  struct Flex_Sync            Sync;
  struct Flex_FIW             FIW;
  struct Flex_Data            Data;
  struct Flex_Decode          Decode;
        struct Flex_GroupHandler    GroupHandler;
  struct Flex_FragStore       FragStore;
  struct Flex_DedupStore     DedupStore;
  int                         biw_sysmsg_a_type;  // BIW101 A-type (-1 = not present)
  struct Flex_OTA_Time        ota_time;            // Last known good OTA time
  char                        last_flextime[80];   // Last emitted FLEXTIME (change detection)
  /* Piped output line prefix, built by main loop before calling parse functions.
   * Format: "FLEX_NEXT|timestamp|baud/levels|cycle.frame.phase|capcodes|addr"
   * Parse functions append "|TYPE|flags|message\n" to form a complete line. */
  char                        line_prefix[1200];
  const char                 *line_type_tag;       // Current message type tag for piped output
};

static int flex_group_endpoint(struct Flex_GroupHandler *handler, int groupbit) {
  if (handler==NULL || groupbit < 0 || groupbit >= GROUP_BITS) return 0;

  int endpoint = handler->GroupCodes[groupbit][CAPCODES_INDEX];
  if (endpoint < 0) return 0;
  if (endpoint > GROUP_MAX_CODES) return GROUP_MAX_CODES;
  return endpoint;
}

/* Emit a FLEXTIME line at level 0 with the current confirmed OTA state.
 * Called each time a flextime component is promoted (vote threshold reached).
 * Shows the full snapshot: whatever date/time/tz components are confirmed. */
static void flextime_emit(struct Flex_Next *flex, char phase) {
  struct Flex_OTA_Time *ot = &flex->ota_time;
  if (!ot->has_date && !ot->has_time && !ot->has_tz)
    return;

  char flex_ts[32];
  flex_local_timestamp(flex_ts, sizeof(flex_ts));

  /* Build flags: D+/D-.T+/T-.TZ+/TZ-[.P75/.P09] */
  char flags[32];
  int fpos = 0;
  fpos += snprintf(flags + fpos, sizeof(flags) - fpos, "D%c.T%c.TZ%c",
                   ot->has_date ? '+' : '-',
                   ot->has_time ? '+' : '-',
                   ot->has_tz ? '+' : '-');
  if (ot->has_time)
    fpos += snprintf(flags + fpos, sizeof(flags) - fpos, ".P%s.%s",
                     ot->has_tz ? "09" : "75",
                     ot->tmode_detected == 0 ? "STD" : "DIR");

  /* Build OTA timestamp value */
  char ota[80];
  int pos = 0;

  if (ot->has_date) {
    pos += snprintf(ota + pos, sizeof(ota) - pos, "%04u-%02u-%02u",
                    ot->year, ot->month, ot->day);
  }
  if (ot->has_time) {
    /* Compute seconds: coarse*7.5 + ext*0.9375 (ext adds to coarse).
     * In standard mode, also add frame*1.875s offset. */
    double base_sec = (double)ot->min * 60.0 + (double)ot->sec_coarse * 7.5;
    if (ot->has_tz)
      base_sec += (double)ot->sec_ext * 0.9375;
    if (ot->tmode_detected == 0)
      base_sec += (double)flex->FIW.frameno * 1.875;

    unsigned int out_hour = ot->hour;
    unsigned int out_min = (unsigned int)(base_sec / 60.0);
    double out_sec = base_sec - (double)out_min * 60.0;
    if (out_min >= 60) { out_min -= 60; out_hour = (out_hour + 1) % 24; }

    if (pos > 0) ota[pos++] = ' ';
    pos += snprintf(ota + pos, sizeof(ota) - pos, "%02u:%02u:%05.2f",
                    out_hour, out_min, out_sec);
  }
  if (ot->has_tz) {
    int off = ot->tz_offset_min;
    const char *dst_str = ot->tz_dst ? "" : " DST";
    int abs_off = off < 0 ? -off : off;
    if (pos > 0) ota[pos++] = ' ';
    if (abs_off % 60 == 0)
      pos += snprintf(ota + pos, sizeof(ota) - pos, "%+dh%s", off / 60, dst_str);
    else
      pos += snprintf(ota + pos, sizeof(ota) - pos, "%+dh%02dm%s", off / 60, abs_off % 60, dst_str);
  }

  /* Only emit if the OTA string changed since last emission */
  if (strcmp(ota, flex->last_flextime) == 0)
    return;
  memcpy(flex->last_flextime, ota, sizeof(flex->last_flextime));

  verbprintf(0, "FLEX_NEXT|%s|%i/%i|%02u.%03u.%c|0000000000|B|FLEXTIME|%s|%s\n",
             flex_ts,
             flex->Sync.baud, flex->Sync.levels,
             flex->FIW.cycleno, flex->FIW.frameno, phase,
             flags, ota);
}


// Classify address type from the raw 21-bit address word (NOT capcode).
// For special addresses: capcode = aw - 0x8000 (FLEX_SHORT_ADDR_OFFSET).
// Returns a single character:
//   S = Short individual   L = Long individual
//   R = Reserved           I = Info Service (capcode 2,009,088-2,025,471)
//   N = Network/NID        T = Temporary (group)
//   O = Operator Message
static char addr_type_char(uint32_t aiw, int is_long) {
  if (is_long) return 'L';
  // Special address word ranges per ARIB STD-43A Table 3.8.1-1:
  //   aw 0x1F0001-0x1F27FF (capcode 1,998,849-2,009,087): Reserved Short 1
  //   aw 0x1F2800-0x1F67FF (capcode 2,009,088-2,025,471): Info Service
  //   aw 0x1F6800-0x1F77FF (capcode 2,025,472-2,029,567): Network (NID)
  //   aw 0x1F7800-0x1F780F (capcode 2,029,568-2,029,583): Temporary (group)
  //   aw 0x1F7810-0x1F781F (capcode 2,029,584-2,029,599): Operator Message
  //   aw 0x1F7820-0x1F7FFE (capcode 2,029,600-2,031,614): Reserved Short 2
  if (aiw >= 0x1F0001L && aiw <= 0x1F27FFL) return 'R';  // Reserved Short 1
  if (aiw >= 0x1F2800L && aiw <= 0x1F67FFL) return 'I';  // Info Service
  if (aiw >= 0x1F6800L && aiw <= 0x1F77FFL) return 'N';  // Network (NID)
  if (aiw >= 0x1F7800L && aiw <= 0x1F780FL) return 'T';  // Temporary (group)
  if (aiw >= 0x1F7810L && aiw <= 0x1F781FL) return 'O';  // Operator Message
  if (aiw >= 0x1F7820L && aiw <= 0x1F7FFEL) return 'R';  // Reserved Short 2
  return 'S';  // Normal short address
}

// JSON output helper: emit a complete JSON message object.
// Called from decode_phase after message parsing is complete.
// All fields are optional (pass NULL/negative to omit).
static void flex_next_json_emit(struct Flex_Next *flex, char phase,
                                int64_t capcode, char addr_type,
                                int is_group, int msg_type,
                                const char *type_tag,
                                const char *fragment,
                                int msg_n, int msg_r, int msg_m,
                                int k_ok, int sig_ok,
                                const char *message,
                                int64_t *group_capcodes, int group_count,
                                cJSON *extra)
{
  cJSON *json = cJSON_CreateObject();
  if (!json) return;

  time_t now = time(NULL);
  struct tm *gmt = gmtime(&now);
  char ts[64];
  snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
           gmt->tm_year+1900, gmt->tm_mon+1, gmt->tm_mday,
           gmt->tm_hour, gmt->tm_min, gmt->tm_sec);
  cJSON_AddStringToObject(json, "timestamp", ts);
  cJSON_AddNumberToObject(json, "baud", flex->Sync.baud);
  cJSON_AddNumberToObject(json, "level", flex->Sync.levels);
  {
    char ph[2] = { phase, '\0' };
    cJSON_AddStringToObject(json, "phase", ph);
  }
  cJSON_AddNumberToObject(json, "cycle", flex->FIW.cycleno);
  cJSON_AddNumberToObject(json, "frame", flex->FIW.frameno);
  cJSON_AddNumberToObject(json, "capcode", (double)capcode);
  {
    char at[2] = { addr_type, '\0' };
    cJSON_AddStringToObject(json, "addr_type", at);
  }
  cJSON_AddBoolToObject(json, "is_group", is_group ? 1 : 0);
  if (flex->Decode.is_priority)
    cJSON_AddBoolToObject(json, "is_priority", 1);
  if (msg_type >= 0) {
    // Human-readable message type name
    const char *mt_name = "unknown";
    switch (msg_type) {
      case 0: mt_name = "secure"; break;
      case 1: mt_name = "instruction"; break;
      case 2: mt_name = "short_msg"; break;
      case 3: mt_name = "numeric"; break;
      case 4: mt_name = "special_numeric"; break;
      case 5: mt_name = "alphanumeric"; break;
      case 6: mt_name = "binary"; break;
      case 7: mt_name = "numbered_numeric"; break;
      case 8: mt_name = "tone_only"; break;
    }
    cJSON_AddStringToObject(json, "msg_type", mt_name);
  }
  if (type_tag)
    cJSON_AddStringToObject(json, "type_tag", type_tag);
  // Add group_slot for temporary group address messages
  if (is_group && capcode >= 2029568 && capcode <= 2029583)
    cJSON_AddNumberToObject(json, "group_slot", (int)(capcode - 2029568));
  if (fragment)
    cJSON_AddStringToObject(json, "fragment", fragment);
  if (msg_n >= 0)
    cJSON_AddNumberToObject(json, "msg_number", msg_n);
  if (msg_r >= 0)
    cJSON_AddNumberToObject(json, "retrieval", msg_r);
  if (msg_m >= 0)
    cJSON_AddNumberToObject(json, "maildrop", msg_m);
  if (k_ok >= 0)
    cJSON_AddBoolToObject(json, "k_ok", k_ok ? 1 : 0);
  if (sig_ok >= 0)
    cJSON_AddBoolToObject(json, "sig_ok", sig_ok ? 1 : 0);
  if (message)
    cJSON_AddStringToObject(json, "message", message);
  if (group_capcodes && group_count > 0) {
    cJSON *arr = cJSON_CreateArray();
    for (int gi = 0; gi < group_count; gi++)
      cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)group_capcodes[gi]));
    cJSON_AddItemToObject(json, "group_capcodes", arr);
  }
  // Merge caller-provided extra fields
  if (extra) {
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, extra) {
      cJSON_AddItemToObject(json, item->string, cJSON_Duplicate(item, 1));
    }
    cJSON_Delete(extra);
  }
  // Add sec_subtype and opr_category from Decode context if set
  if (flex->Decode.sec_subtype)
    cJSON_AddStringToObject(json, "sec_subtype", flex->Decode.sec_subtype);
  if (flex->Decode.opr_category)
    cJSON_AddStringToObject(json, "opr_category", flex->Decode.opr_category);

  // flextime: over-the-air timestamp from FLEX network BIW system info.
  // Structured object with per-component status, age, and received_at.
  // Only included when at least one component has been received.
  if (flex->ota_time.has_date || flex->ota_time.has_time || flex->ota_time.has_tz) {
    unsigned int cur_frame = flex->FIW.cycleno * 128 + flex->FIW.frameno;
    // Expire stale components before emitting
    flextime_expire(&flex->ota_time, cur_frame);
    // Re-check after expiry - might have nothing left
    if (flex->ota_time.has_date || flex->ota_time.has_time || flex->ota_time.has_tz) {
    cJSON *ft = cJSON_CreateObject();

    // Combined timestamp string (only when date+time both available)
    if (flex->ota_time.has_date && flex->ota_time.has_time) {
      /* Compute corrected seconds: coarse*7.5 + ext*0.9375.
       * In standard mode, add frame*1.875s offset. */
      double base_sec = (double)flex->ota_time.min * 60.0 +
                        (double)flex->ota_time.sec_coarse * 7.5;
      if (flex->ota_time.has_tz)
        base_sec += (double)flex->ota_time.sec_ext * 0.9375;
      if (flex->ota_time.tmode_detected == 0)
        base_sec += (double)flex->FIW.frameno * 1.875;
      unsigned int j_hour = flex->ota_time.hour;
      unsigned int j_min = (unsigned int)(base_sec / 60.0);
      double j_sec = base_sec - (double)j_min * 60.0;
      if (j_min >= 60) { j_min -= 60; j_hour = (j_hour + 1) % 24; }

      char ota_ts[80];
      int ota_pos = snprintf(ota_ts, sizeof(ota_ts), "%04u-%02u-%02u %02u:%02u:%05.2f",
                             flex->ota_time.year, flex->ota_time.month, flex->ota_time.day,
                             j_hour, j_min, j_sec);
      if (flex->ota_time.has_tz) {
        int off = flex->ota_time.tz_offset_min;
        const char *dst_str = flex->ota_time.tz_dst ? "" : " DST";
        int abs_off = off < 0 ? -off : off;
        if (abs_off % 60 == 0)
          snprintf(ota_ts + ota_pos, sizeof(ota_ts) - ota_pos, " %+dh%s", off / 60, dst_str);
        else
          snprintf(ota_ts + ota_pos, sizeof(ota_ts) - ota_pos, " %+dh%02dm%s", off / 60, abs_off % 60, dst_str);
      }
      cJSON_AddStringToObject(ft, "timestamp", ota_ts);
      cJSON_AddNumberToObject(ft, "precision_seconds", flex->ota_time.has_tz ? 0.9375 : 7.5);
    }

    // Date component
    if (flex->ota_time.has_date) {
      cJSON *d = cJSON_CreateObject();
      char dval[16];
      snprintf(dval, sizeof(dval), "%04u-%02u-%02u", flex->ota_time.year, flex->ota_time.month, flex->ota_time.day);
      cJSON_AddStringToObject(d, "value", dval);
      int af = (int)flextime_age_frames(cur_frame, flex->ota_time.frame_date);
      cJSON_AddNumberToObject(d, "age_frames", af);
      cJSON_AddNumberToObject(d, "age_seconds", ((int)(af * 187.5)) / 100.0);
      cJSON_AddItemToObject(ft, "date", d);
    }

    // Time component
    if (flex->ota_time.has_time) {
      cJSON *t = cJSON_CreateObject();
      char tval[16];
      snprintf(tval, sizeof(tval), "%02u:%02u", flex->ota_time.hour, flex->ota_time.min);
      cJSON_AddStringToObject(t, "value", tval);
      cJSON_AddNumberToObject(t, "sec_coarse", flex->ota_time.sec_coarse);
      int af = (int)flextime_age_frames(cur_frame, flex->ota_time.frame_time);
      cJSON_AddNumberToObject(t, "age_frames", af);
      cJSON_AddNumberToObject(t, "age_seconds", ((int)(af * 187.5)) / 100.0);
      cJSON_AddItemToObject(ft, "time", t);
    }

    // Timezone component (includes extended seconds)
    if (flex->ota_time.has_tz) {
      cJSON *tz = cJSON_CreateObject();
      char tzval[32];
      int off = flex->ota_time.tz_offset_min;
      const char *dst_str = flex->ota_time.tz_dst ? "" : " DST";
      int abs_off = off < 0 ? -off : off;
      if (abs_off % 60 == 0)
        snprintf(tzval, sizeof(tzval), "%+dh%s", off / 60, dst_str);
      else
        snprintf(tzval, sizeof(tzval), "%+dh%02dm%s", off / 60, abs_off % 60, dst_str);
      cJSON_AddStringToObject(tz, "value", tzval);
      cJSON_AddNumberToObject(tz, "offset_min", flex->ota_time.tz_offset_min);
      cJSON_AddBoolToObject(tz, "dst", flex->ota_time.tz_dst ? 0 : 1);
      cJSON_AddNumberToObject(tz, "zone_code", flex->ota_time.tz_zone);
      cJSON_AddNumberToObject(tz, "sec_ext", flex->ota_time.sec_ext);
      // sec_combined: precise seconds when both time and tz available
      if (flex->ota_time.has_time) {
        double sec_combined = (double)flex->ota_time.sec_coarse * 7.5 +
                              (double)flex->ota_time.sec_ext * 0.9375;
        cJSON_AddNumberToObject(tz, "sec_combined", sec_combined);
      }
      int af = (int)flextime_age_frames(cur_frame, flex->ota_time.frame_tz);
      cJSON_AddNumberToObject(tz, "age_frames", af);
      cJSON_AddNumberToObject(tz, "age_seconds", ((int)(af * 187.5)) / 100.0);
      cJSON_AddItemToObject(ft, "tz", tz);
    }

    cJSON_AddStringToObject(ft, "time_mode",
                            flex->ota_time.tmode_detected == 0 ? "standard" : "direct");
    cJSON_AddItemToObject(json, "flextime", ft);
    } // end re-check after expiry
  }
  char *out = cJSON_PrintUnformatted(json);
  if (out) {
    fprintf(stdout, "%s\n", out);
    free(out);
  }
  cJSON_Delete(json);
}


static unsigned int count_bits(struct Flex_Next * flex, unsigned int data) {
  if (flex==NULL) return 0;
#ifdef USE_BUILTIN_POPCOUNT
  return __builtin_popcount(data);
#else
  unsigned int n = (data >> 1) & 0x77777777;
  data = data - n;
  n = (n >> 1) & 0x77777777;
  data = data - n;
  n = (n >> 1) & 0x77777777;
  data = data - n;
  data = (data + (data >> 4)) & 0x0f0f0f0f;
  data = data * 0x01010101;
  return data >> 24;
#endif
}


static int bch3121_fix_errors(struct Flex_Next * flex, uint32_t * data_to_fix, char PhaseNo) {
  if (flex==NULL) return -1;

  unsigned int original = *data_to_fix;
  unsigned int data = original;
  
  /*Decode and correct using bch library with even parity check*/
  int result = bch_flex_next_correct(&data);

  /*Decode successful?*/
  if (result >= 0) {
    /*Count the number of fixed errors*/
    if (result > 0) {
      verbprintf(3, "FLEX_NEXT: Phase %c Fixed %i errors @ 0x%08x  (0x%08x -> 0x%08x)\n", PhaseNo, result, original ^ data, original, data );
    }

    /*Write the fixed data back to the caller*/
    *data_to_fix = data;
    return result;  /* 0=clean, 1=1-bit fix, 2=2-bit fix */

  } else {
    verbprintf(3, "FLEX_NEXT: Phase %c Data corruption - Unable to fix errors.\n", PhaseNo);
    return -1;
  }
}

static unsigned int flex_sync_check(struct Flex_Next * flex, uint64_t buf) {
  if (flex==NULL) return 0;
  // 64-bit FLEX sync code:
  // AAAA:BBBBBBBB:CCCC
  //
  // Where BBBBBBBB is always 0xA6C6AAAA
  // and AAAA^CCCC is 0xFFFF
  //
  // Specific values of AAAA determine what bps and encoding the
  // packet is beyond the frame information word
  //
  // First we match on the marker field with a hamming distance < 4
  // Then we match on the outer code with a hamming distance < 4

  unsigned int marker =      (buf & 0x0000FFFFFFFF0000ULL) >> 16;
  unsigned short codehigh =  (buf & 0xFFFF000000000000ULL) >> 48;
  unsigned short codelow  = ~(buf & 0x000000000000FFFFULL);

  int retval=0;
  if (count_bits(flex, marker ^ FLEX_SYNC_MARKER) < 4  && count_bits(flex, codelow ^ codehigh) < 4 ) {
    retval=codehigh;
  } else {
    retval=0;
  }

  return retval;
}


static unsigned int flex_sync(struct Flex_Next * flex, unsigned char sym) {
  if (flex==NULL) return 0;
  int retval=0;
  flex->Sync.syncbuf = (flex->Sync.syncbuf << 1) | ((sym < 2)?1:0);

  retval=flex_sync_check(flex, flex->Sync.syncbuf);
  if (retval!=0) {
    flex->Sync.polarity=0;
  } else {
    /*If a positive sync pattern was not found, look for a negative (inverted) one*/
    retval=flex_sync_check(flex, ~flex->Sync.syncbuf);
    if (retval!=0) {
      flex->Sync.polarity=1;
    }
  }

  return retval;
}


static void decode_mode(struct Flex_Next * flex, unsigned int sync_code) {
  if (flex==NULL) return;

  // Sync codes per ARIB STD-43A Table 3.2-5:
  //   A1 (0x870C): 1600bps / 2-level FSK (1600 baud)
  //   A2 (0x7B18): 3200bps / 2-level FSK (3200 baud)
  //   A3 (0xB068): 3200bps / 4-level FSK (1600 baud, 2 phases)
  //   A4 (0xDEA0): 6400bps / 4-level FSK (3200 baud, 4 phases)
  //   A7 (0x4C7C): ReFLEX / 6400bps / 4-level FSK (same as A4)
  struct {
    int sync;
    unsigned int baud;
    unsigned int levels;
  } flex_modes[] = {
    { 0x870C, 1600, 2 },   // A1: 1600bps/2FSK
    { 0xB068, 1600, 4 },   // A3: 3200bps/4FSK (1600 baud symbol rate)
    { 0x7B18, 3200, 2 },   // A2: 3200bps/2FSK
    { 0xDEA0, 3200, 4 },   // A4: 6400bps/4FSK (3200 baud symbol rate)
    { 0x4C7C, 3200, 4 },   // A7: ReFLEX (same physical layer as A4)
    {0,0,0}
  };
  
  int x=0;
  int i=0;
  for (i=0; flex_modes[i].sync!=0; i++) {
    if (count_bits(flex, flex_modes[i].sync ^ sync_code) < 4) {
      flex->Sync.sync   = sync_code;
      flex->Sync.baud   = flex_modes[i].baud;
      flex->Sync.levels = flex_modes[i].levels;
      x = 1;
      break;
    }
  }
  
  if(x==0){
    verbprintf(3, "FLEX_NEXT: Sync Code not found, defaulting to 1600bps 2FSK\n");
  }
}


static void read_2fsk(struct Flex_Next * flex, unsigned int sym, unsigned int * dat) {
  if (flex==NULL) return;
  *dat = (*dat >> 1) | ((sym > 1)?0x80000000:0);
}


static int decode_fiw(struct Flex_Next * flex) {
  if (flex==NULL) return -1;
  unsigned int fiw = flex->FIW.rawdata;
  int decode_error = bch3121_fix_errors(flex, &fiw, 'F');

  if (decode_error < 0) {
    verbprintf(3, "FLEX_NEXT: Unable to decode FIW, too much data corruption\n");
    return 1;
  }

  // The only relevant bits in the FIW word for the purpose of this function
  // are those masked by 0x001FFFFF.
  flex->FIW.checksum = fiw & 0xF;
  flex->FIW.cycleno = (fiw >> 4) & 0xF;
  flex->FIW.frameno = (fiw >> 8) & 0x7F;
  flex->FIW.roaming = (fiw >> 15) & 0x1;
  flex->FIW.repeat  = (fiw >> 16) & 0x1;
  flex->FIW.traffic = (fiw >> 17) & 0xF;
  flex->FIW.fix3 = (fiw >> 15) & 0x3F;  // kept for backward compat

  // Derive number of transmissions from r and t fields
  if (flex->FIW.repeat) {
    unsigned int t10 = flex->FIW.traffic & 0x3;
    unsigned int t32 = (flex->FIW.traffic >> 2) & 0x3;
    flex->FIW.td_collapse = t32;  // collapse cycle for repeat interval
    switch (t10) {
      case 0x01: flex->FIW.num_tx = 2; break;
      case 0x02: flex->FIW.num_tx = 3; break;
      case 0x03: flex->FIW.num_tx = 4; break;
      default:   flex->FIW.num_tx = 1; break; // reserved
    }
  } else {
    flex->FIW.num_tx = 1;
    flex->FIW.td_collapse = 0;
  }

  unsigned int checksum = (fiw & 0xF);
  checksum += ((fiw >> 4) & 0xF);
  checksum += ((fiw >> 8) & 0xF);
  checksum += ((fiw >> 12) & 0xF);
  checksum += ((fiw >> 16) & 0xF);
  checksum += ((fiw >> 20) & 0x01);

  checksum &= 0xF;

  if (checksum == 0xF) {
    int timeseconds = flex->FIW.cycleno*4*60 + flex->FIW.frameno*4*60/128;
    if (flex->FIW.repeat) {
      verbprintf(2, "FLEX_NEXT: FrameInfoWord: cycleno=%02i frameno=%03i roaming=%u repeat=%ux time=%02i:%02i\n",
          flex->FIW.cycleno,
          flex->FIW.frameno,
          flex->FIW.roaming,
          flex->FIW.num_tx,
          timeseconds/60,
          timeseconds%60);
    } else {
      verbprintf(2, "FLEX_NEXT: FrameInfoWord: cycleno=%02i frameno=%03i roaming=%u low_traffic=0x%x time=%02i:%02i\n",
          flex->FIW.cycleno,
          flex->FIW.frameno,
          flex->FIW.roaming,
          flex->FIW.traffic,
          timeseconds/60,
          timeseconds%60);
    }
    // Lets check the FrameNo against the expected group message frames, if we have 'Missed a group message' tell the user and clear the Cap Codes
    for(int g = 0; g < GROUP_BITS ;g++) {
      // Do we have a group message pending for this groupbit?
      if(flex->GroupHandler.GroupFrame[g] >= 0)
      {
        /* Check delivering groups for fragment timeout.
         * Require 2 consecutive FIWs past the deadline to avoid
         * false timeouts from a single erroneous FIW. */
        if (flex->GroupHandler.GroupDelivering[g]) {
          unsigned int cur_abs = flex->FIW.cycleno * 128 + flex->FIW.frameno;
          unsigned int last_frag = flex->GroupHandler.GroupLastFragFrame[g];
          unsigned int age = (cur_abs - last_frag) & 0x7FF; /* wrap-safe */
          if (age > FLEX_FRAG_TIMEOUT) {
            flex->GroupHandler.GroupTimeoutCount[g]++;
            if (flex->GroupHandler.GroupTimeoutCount[g] >= 2) {
              verbprintf(3, "FLEX_NEXT: Group %d fragment delivery timeout (age=%u frames)\n", g, age);
              flex->GroupHandler.GroupCodes[g][CAPCODES_INDEX] = 0;
              flex->GroupHandler.GroupFrame[g] = -1;
              flex->GroupHandler.GroupCycle[g] = -1;
              flex->GroupHandler.GroupDelivering[g] = 0;
              flex->GroupHandler.GroupTimeoutCount[g] = 0;
            }
          } else {
            flex->GroupHandler.GroupTimeoutCount[g] = 0;
          }
          continue;
        }
        int Reset = 0;
        verbprintf(4, "FLEX_NEXT: GroupBit %i, FrameNo: %i, Cycle No: %i target Cycle No: %i\n", g, flex->GroupHandler.GroupFrame[g], flex->GroupHandler.GroupCycle[g], (int)flex->FIW.cycleno); 
        // Now lets check if its expected in this frame..
        if((int)flex->FIW.cycleno == flex->GroupHandler.GroupCycle[g])
        {
          if(flex->GroupHandler.GroupFrame[g] < (int)flex->FIW.frameno)
          {
            Reset = 1;
          }
        }
                                // Check if we should have sent a group message in the previous cycle 
        else if(flex->FIW.cycleno == 0) 
        {
          if(flex->GroupHandler.GroupCycle[g] == 15)
          {
            Reset = 1;
          }
        }
                                // If we are waiting for the cycle to roll over then move onto the next for loop item 
        else if(flex->FIW.cycleno == 15 && flex->GroupHandler.GroupCycle[g] == 0)
        {
          continue;
        } 
        // Otherwise if the target cycle is less than the current cycle, reset the data
        else if(flex->GroupHandler.GroupCycle[g] < (int)flex->FIW.cycleno)
        {
          Reset = 1;
        }
      

        if(Reset == 1)
        {
                              
                      int endpoint = flex_group_endpoint(&flex->GroupHandler, g);
          if(REPORT_GROUP_CODES > 0)
          {
            verbprintf(3,"FLEX_NEXT: Group messages seem to have been missed; Groupbit: %i; Total Capcodes: %i; Clearing Data; Capcodes: ", g, endpoint);
          }
          
                      for(int capIndex = 1; capIndex <= endpoint; capIndex++)
          {
            if(REPORT_GROUP_CODES == 0)
            {
              verbprintf(3,"FLEX_NEXT: Group messages seem to have been missed; Groupbit: %i; Clearing data; Capcode: [%010" PRId64 "]\n", g, flex->GroupHandler.GroupCodes[g][capIndex]);
            }
            else
            {
              if(capIndex > 1)
              {
                verbprintf(3,",");
              }
              verbprintf(3,"[%010" PRId64 "]", flex->GroupHandler.GroupCodes[g][capIndex]);
            }
          }

          if(REPORT_GROUP_CODES > 0)
                                        {
                                                verbprintf(3,"\n");
                                        }

                      // reset the value
                      flex->GroupHandler.GroupCodes[g][CAPCODES_INDEX] = 0;
                      flex->GroupHandler.GroupFrame[g] = -1;
                      flex->GroupHandler.GroupCycle[g] = -1;
                      flex->GroupHandler.GroupDelivering[g] = 0;
        }
      }
                }
    return 0;
  } else {
    verbprintf(3, "FLEX_NEXT: Bad Checksum 0x%x\n", checksum);

    return 1;
  }
}


/* Add a character to ALN messages, but avoid buffer overflows and special characters */
static unsigned int add_ch(unsigned char ch, unsigned char* buf, unsigned int idx) {
    // avoid buffer overflow that has been happening
    if (idx >= MAX_ALN) {
        verbprintf(3, "FLEX_NEXT: idx %u >= MAX_ALN %u\n", idx, MAX_ALN);
        return 0;
    }
    // TODO sanitize % or you will have uncontrolled format string vuln
    // Originally, this only avoided storing ETX (end of text, 0x03).
    // At minimum you'll also want to avoid storing NULL (str term, 0x00),
    // otherwise verbprintf will truncate the message.
    //   ex: if (ch != 0x03 && ch != 0x00) { buf[idx] = ch; return 1; }
    // But while we are here, make it print friendly and get it onto a single line
    //   * remove awkward ctrl chars (del, bs, bell, vertical tab, etc)
    //   * encode valuable ctrl chars (new line/line feed, carriage ret, tab)
    // NOTE: if you post process FLEX ALN output by sed/grep/awk etc on non-printables
    //   then double check this doesn't mess with your pipeline
    if (ch == 0x09 && idx < (MAX_ALN - 2)) {  // '\t'
        buf[idx] = '\\';
        buf[idx + 1] = 't';
        return 2;
    }
    if (ch == 0x0a && idx < (MAX_ALN - 2)) {  // '\n'
        buf[idx] = '\\';
        buf[idx + 1] = 'n';
        return 2;
    }
    if (ch == 0x0d && idx < (MAX_ALN - 2)) {  // '\r'
        buf[idx] = '\\';
        buf[idx + 1] = 'r';
        return 2;
    }
    // unixinput.c::_verbprintf uses this output as a format string
    // which introduces an uncontrolled format string vulnerability
    // and also, generally, risks stack corruption
    if (ch == '%') {
        if (idx < (MAX_ALN - 2)) {
            buf[idx] = '%';
            buf[idx + 1] = '%';
            return 2;
        }
        return 0;
    }
    // only store ASCII printable
    if (ch >= 32 && ch <= 126) {
        buf[idx] = ch;
        return 1;
    }
    // if you want all non-printables, show as hex, but also make MAX_ALN 1024
    /* if (idx < (MAX_ALN - 4)) {
        sprintf(buf + idx, "\\x%02x", ch);
        return 4;
    }*/
    return 0;
}


/* ---------------------------------------------------------------------- */
/* Fragment reassembly helpers                                             */
/* ---------------------------------------------------------------------- */

// Find an existing fragment slot for a capcode/type/msg_n, or -1 if not found.
// msg_n identifies the fragment stream (Section 3.10.1.3 bits 13-18).
static int frag_find(struct Flex_Next * flex, int64_t capcode, int type, int msg_n) {
  int i;
  for (i = 0; i < FLEX_FRAG_MAX_SLOTS; i++) {
    if (flex->FragStore.slots[i].active &&
        flex->FragStore.slots[i].capcode == capcode &&
        flex->FragStore.slots[i].type == type &&
        flex->FragStore.slots[i].msg_n == msg_n)
      return i;
  }
  return -1;
}

// Allocate a new fragment slot, evicting the oldest if full
static int frag_alloc(struct Flex_Next * flex, int64_t capcode, int type, int msg_n, unsigned int frame) {
  int i;
  // Find a free slot
  for (i = 0; i < FLEX_FRAG_MAX_SLOTS; i++) {
    if (!flex->FragStore.slots[i].active) {
      flex->FragStore.slots[i].active = 1;
      flex->FragStore.slots[i].capcode = capcode;
      flex->FragStore.slots[i].type = type;
      flex->FragStore.slots[i].msg_n = msg_n;
      flex->FragStore.slots[i].msg_r = -1;
      flex->FragStore.slots[i].msg_m = -1;
      flex->FragStore.slots[i].data_len = 0;
      flex->FragStore.slots[i].frame_received = frame;
      flex->FragStore.slots[i].sig_sum = 0;
      flex->FragStore.slots[i].rx_sig = 0;
      flex->FragStore.slots[i].sig_valid = 0;
      flex->FragStore.slots[i].k_fail = 0;
      flex->FragStore.slots[i].expected_f = 0;  // after F=11 (initial), next expected is F=00
      flex->FragStore.slots[i].frag_index = 0;
      flex->FragStore.slots[i].f_mismatch = 0;
      flex->FragStore.slots[i].phase = '?';
      flex->FragStore.slots[i].addr_type = '?';
      return i;
    }
  }
  // Evict oldest
  {
    int oldest = 0;
    unsigned int oldest_frame = flex->FragStore.slots[0].frame_received;
    for (i = 1; i < FLEX_FRAG_MAX_SLOTS; i++) {
      if (flex->FragStore.slots[i].frame_received < oldest_frame) {
        oldest = i;
        oldest_frame = flex->FragStore.slots[i].frame_received;
      }
    }
    verbprintf(3, "FLEX_NEXT: Fragment store full, evicting slot %d (cap=%" PRId64 ")\n", oldest, flex->FragStore.slots[oldest].capcode);
    flex->FragStore.slots[oldest].active = 1;
    flex->FragStore.slots[oldest].capcode = capcode;
    flex->FragStore.slots[oldest].type = type;
    flex->FragStore.slots[oldest].msg_n = msg_n;
    flex->FragStore.slots[oldest].msg_r = -1;
    flex->FragStore.slots[oldest].msg_m = -1;
    flex->FragStore.slots[oldest].data_len = 0;
    flex->FragStore.slots[oldest].frame_received = frame;
    flex->FragStore.slots[oldest].sig_sum = 0;
    flex->FragStore.slots[oldest].rx_sig = 0;
    flex->FragStore.slots[oldest].sig_valid = 0;
    flex->FragStore.slots[oldest].k_fail = 0;
    flex->FragStore.slots[oldest].expected_f = 0;
    flex->FragStore.slots[oldest].frag_index = 0;
    flex->FragStore.slots[oldest].f_mismatch = 0;
    flex->FragStore.slots[oldest].phase = '?';
    flex->FragStore.slots[oldest].addr_type = '?';
    return oldest;
  }
}

// Append data to a fragment slot
static void frag_append(struct Flex_Next * flex, int slot, const unsigned char *data, unsigned int len) {
  unsigned int space = FLEX_FRAG_MAX_LEN - flex->FragStore.slots[slot].data_len;
  if (len > space) len = space;
  if (len > 0) {
    memcpy(flex->FragStore.slots[slot].data + flex->FragStore.slots[slot].data_len, data, len);
    flex->FragStore.slots[slot].data_len += len;
  }
}

// Release a fragment slot
static void frag_release(struct Flex_Next * flex, int slot) {
  flex->FragStore.slots[slot].active = 0;
  flex->FragStore.slots[slot].data_len = 0;
  flex->FragStore.slots[slot].msg_n = -1;
  flex->FragStore.slots[slot].msg_r = -1;
  flex->FragStore.slots[slot].msg_m = -1;
  flex->FragStore.slots[slot].sig_sum = 0;
  flex->FragStore.slots[slot].rx_sig = 0;
  flex->FragStore.slots[slot].sig_valid = 0;
  flex->FragStore.slots[slot].k_fail = 0;
  flex->FragStore.slots[slot].expected_f = 0;
  flex->FragStore.slots[slot].frag_index = 0;
  flex->FragStore.slots[slot].f_mismatch = 0;
}

// Expire old fragment slots
static void frag_expire(struct Flex_Next * flex, unsigned int current_frame) {
  int i;
  for (i = 0; i < FLEX_FRAG_MAX_SLOTS; i++) {
    if (flex->FragStore.slots[i].active) {
      /* abs_frame is cycleno*128+frameno and wraps at 2048 (4-bit cycle, 7-bit frame).
         Mask the subtraction so a wrap-around gives the correct forward distance. */
      unsigned int age = (current_frame - flex->FragStore.slots[i].frame_received) & 0x7FF;
      if (age > FLEX_FRAG_TIMEOUT) {
        verbprintf(3, "FLEX_NEXT: Fragment expired slot %d cap=%" PRId64 " age=%u\n", i, flex->FragStore.slots[i].capcode, age);
        /* Emit partial message if slot has data */
        if (flex->FragStore.slots[i].data_len > 0) {
          if (json_mode) {
            flex_next_json_emit(flex, flex->FragStore.slots[i].phase,
                                flex->FragStore.slots[i].capcode,
                                flex->FragStore.slots[i].addr_type, 0,
                                flex->FragStore.slots[i].type,
                                "ALN", "reassembled_partial",
                                flex->FragStore.slots[i].msg_n, -1, -1,
                                flex->FragStore.slots[i].k_fail ? 0 : -1, -1,
                                (const char *)flex->FragStore.slots[i].data,
                                NULL, 0, NULL);
          } else {
            char flex_ts[32];
            flex_local_timestamp(flex_ts, sizeof(flex_ts));
            verbprintf(0, "FLEX_NEXT|%s|%u/%u|%02u.%03u.%c|%010" PRId64 "|%c|ALN|PARTIAL.N%d%s|%.*s\n",
                       flex_ts,
                       flex->Sync.baud, flex->Sync.levels,
                       current_frame / 128, current_frame % 128,
                       flex->FragStore.slots[i].phase,
                       flex->FragStore.slots[i].capcode,
                       flex->FragStore.slots[i].addr_type,
                       flex->FragStore.slots[i].msg_n,
                       flex->FragStore.slots[i].k_fail ? ".K-" : ".K+",
                       (int)flex->FragStore.slots[i].data_len,
                       flex->FragStore.slots[i].data);
          }
        }
        frag_release(flex, i);
      }
    }
  }
}

/* ---------------------------------------------------------------------- */
/* Message deduplication helpers                                           */
/* ---------------------------------------------------------------------- */

// Check a complete (K) message against the dedup cache.
// Returns:
//   0 = new message, not seen before. Cached and caller should decode+output.
//   1 = exact duplicate (all clean words match). Caller should suppress.
//   2 = improved retransmission (some previously-errored words now clean).
//       Cache updated with merged words. Caller should re-decode from
//       the cache entry's words[] and output the improved version.
//
// The cache entry index is written to *out_slot on return 2 so the
// caller can access the merged words.
//
// word_src/err_src point into the phase's phaseptr/bch_err arrays.
// word_off is the index of the first word to store (header word).
// n_words is the total number of words (1 header + body_len body words).
static int dedup_check_words(struct Flex_Next * flex, int64_t capcode, int type,
                             int msg_n, const uint32_t *word_src, const int *err_src,
                             unsigned int word_off, unsigned int n_words,
                             unsigned int hdr_off_in_msg, unsigned int mw1_off_in_msg,
                             unsigned int body_len, int *out_slot) {
  unsigned int frame = (flex->FIW.cycleno * 128 + flex->FIW.frameno);
  int i;

  if (n_words > FLEX_DEDUP_MAX_WORDS)
    n_words = FLEX_DEDUP_MAX_WORDS;

  // Expire old entries and search for a match
  for (i = 0; i < FLEX_DEDUP_SLOTS; i++) {
    if (!flex->DedupStore.entries[i].active)
      continue;
    unsigned int age = (frame - flex->DedupStore.entries[i].frame_seen) & 0x7FF;
    if (age > FLEX_DEDUP_TIMEOUT) {
      flex->DedupStore.entries[i].active = 0;
      continue;
    }
    if (flex->DedupStore.entries[i].capcode != capcode ||
        flex->DedupStore.entries[i].type != type ||
        flex->DedupStore.entries[i].msg_n != msg_n ||
        flex->DedupStore.entries[i].word_count != n_words)
      continue;

    // Same key and word count - compare at the word level.
    // For each word position:
    //   both clean, same value  -> match
    //   both clean, diff value  -> different message, not a dup
    //   cached clean, new error -> still a match (new is worse)
    //   cached error, new clean -> improvement, merge
    //   both error              -> match (both unknown)
    int all_match = 1;
    int improved = 0;
    unsigned int w;
    for (w = 0; w < n_words; w++) {
      int cached_err = flex->DedupStore.entries[i].errs[w];
      int new_err = err_src[word_off + w];
      if (!cached_err && !new_err) {
        // Both clean - must have same value
        if (flex->DedupStore.entries[i].words[w] != word_src[word_off + w]) {
          all_match = 0;
          break;
        }
      } else if (cached_err && !new_err) {
        // Cached was bad, new is clean - improvement
        improved = 1;
      }
      // Other cases (cached clean + new error, both error): compatible
    }

    if (!all_match)
      continue;

    if (!improved) {
      // Exact duplicate or no improvement
      verbprintf(3, "FLEX_NEXT: Dedup suppressed cap=%" PRId64 " type=%d N=%d words=%u\n",
                 capcode, type, msg_n, n_words);
      return 1;
    }

    // Merge improved words into cache
    for (w = 0; w < n_words; w++) {
      if (flex->DedupStore.entries[i].errs[w] && !err_src[word_off + w]) {
        flex->DedupStore.entries[i].words[w] = word_src[word_off + w];
        flex->DedupStore.entries[i].errs[w] = 0;
      }
    }
    flex->DedupStore.entries[i].frame_seen = frame;
    *out_slot = i;
    verbprintf(3, "FLEX_NEXT: Dedup merged improved words for cap=%" PRId64 " type=%d N=%d\n",
               capcode, type, msg_n);
    return 2;
  }

  // Not found - cache it
  int slot = (int)flex->DedupStore.next_slot;
  flex->DedupStore.entries[slot].active = 1;
  flex->DedupStore.entries[slot].capcode = capcode;
  flex->DedupStore.entries[slot].type = type;
  flex->DedupStore.entries[slot].msg_n = msg_n;
  flex->DedupStore.entries[slot].hdr_off = hdr_off_in_msg;
  flex->DedupStore.entries[slot].mw1_off = mw1_off_in_msg;
  flex->DedupStore.entries[slot].word_count = n_words;
  flex->DedupStore.entries[slot].body_len = body_len;
  flex->DedupStore.entries[slot].frame_seen = frame;
  for (i = 0; i < (int)n_words; i++) {
    flex->DedupStore.entries[slot].words[i] = word_src[word_off + i];
    flex->DedupStore.entries[slot].errs[i] = err_src[word_off + i];
  }
  flex->DedupStore.next_slot = (slot + 1) % FLEX_DEDUP_SLOTS;
  *out_slot = slot;
  return 0;
}


static void parse_alphanumeric(struct Flex_Next * flex, unsigned int * phaseptr, int * bch_err, unsigned int hdr_idx, unsigned int mw1, unsigned int len, int frag, int cont, int msg_n, int msg_r, int msg_m, int dedup_flag, int flex_groupmessage, int flex_groupbit) {
        if (flex==NULL) return;

        // Header word is at mw1-1 (short addr) or in the vector field (long addr).
        // The caller already extracted frag (F) and cont (C) from the header.
        // Header layout per Section 3.10.1.3:
        //   bits 0-9:   K (10-bit fragment checksum)
        //   bit  10:    C (continuation: 1=more fragments, 0=last/only)
        //   bits 11-12: F (fragment number: 11=initial, 00/01/10=continuation)
        //   bits 13-18: N (message number, 6 bits, identifies fragment stream)
        //   bit  19:    R (retrieval flag, initial only) / U0 (continuation)
        //   bit  20:    M (mail drop flag, initial only) / V0 (continuation)

        char frag_flag = '?';
        if (cont == 0 && frag == 3) frag_flag = 'K'; // complete, ready to send
        if (cont == 0 && frag != 3) frag_flag = 'C'; // last fragment, completes the message
        if (cont == 1             ) frag_flag = 'F'; // fragment, more coming
        int  is_initial = (frag == 0x03);  // F=11 means initial/only fragment
        // Frag flags output is deferred to each branch (F/C/K) so that
        // reassembled messages can carry R/M from the initial fragment.

        // Group slot tag for temp group messages (empty for non-group)
        char grp_tag[8] = "";
        if (flex_groupmessage && flex_groupbit >= 0 && flex_groupbit < 16)
          snprintf(grp_tag, sizeof(grp_tag), ".G%d", flex_groupbit);

        unsigned char message[MAX_ALN];
        memset(message, '\0', MAX_ALN);
        int  currentChar = 0;

        // K checksum verification (Section 3.10.1.3):
        // 1's complement of binary sum of all message words in 3 groups:
        //   group1 = bits 0-7, group2 = bits 8-15, group3 = bits 16-20
        // K field (bits 0-9 of header) is zeroed before summing.
        // Covers: header word (at hdr_idx) + all content words (mw1..mw1+len-1).
        int k_fail = 0;
        {
          uint32_t k_sum = 0;
          int k_ok = 1;
          unsigned int ki;
          // Include header word with K field zeroed
          if (hdr_idx < PHASE_WORDS && !bch_err[hdr_idx]) {
            uint32_t dw = phaseptr[hdr_idx] & ~0x3FFu;
            k_sum += dw & 0xFFu;
            k_sum += (dw >> 8) & 0xFFu;
            k_sum += (dw >> 16) & 0x1Fu;
          } else {
            k_ok = 0;
          }
          // Include content words
          for (ki = 0; ki < len; ki++) {
            unsigned int wi = mw1 + ki;
            if (wi >= PHASE_WORDS || bch_err[wi]) {
              k_ok = 0;
              continue;
            }
            uint32_t dw = phaseptr[wi];
            k_sum += dw & 0xFFu;
            k_sum += (dw >> 8) & 0xFFu;
            k_sum += (dw >> 16) & 0x1Fu;
          }
          if (k_ok && hdr_idx < PHASE_WORDS) {
            uint32_t rx_k = phaseptr[hdr_idx] & 0x3FFu;
            uint32_t expected_k = (~k_sum) & 0x3FFu;
            if (rx_k != expected_k) {
              verbprintf(3, "FLEX_NEXT: Alpha K checksum FAIL: rx=0x%03X expected=0x%03X\n", rx_k, expected_k);
              k_fail = 1;
            }
          } else {
            /* BCH errors prevented K verification - treat as fail */
            k_fail = 1;
          }
        }

        // Extract 7-bit ASCII characters, 3 per word.
        //
        // By the time we get here, the caller (decode_phase) has already
        // adjusted mw1 and len:
        //   Short addr: mw1 = first content word (past header), len = content words
        //   Long addr:  mw1 = MF start (body[1]), len = MF words (body[0] is in Vy)
        //
        // Initial fragment (F=11): first content word (phaseptr[mw1]) has
        //   bits 0-6 = Signature S, bits 7-13 = char1, bits 14-20 = char2
        // Continuation fragments (F!=11): all three 7-bit slots are characters.
        //
        // Signature S (Section 3.10.1.3): 7-bit, 1's complement of binary sum
        // of all 7-bit character slots (excluding signature itself).
        // ETX (0x03) fill characters are excluded from the sum per spec.
        // (Standard Fragmentation mode - Enhanced Fragmentation also
        // excludes NUL, but that's not implemented.)
        uint32_t sig_sum = 0;
        uint32_t rx_sig = 0;
        int sig_valid = 1;

        for (unsigned int i = 0; i < len; i++) {
            if ((mw1 + i) >= PHASE_WORDS || bch_err[mw1 + i]) {
                currentChar += add_ch('?', message, currentChar);
                currentChar += add_ch('?', message, currentChar);
                currentChar += add_ch('?', message, currentChar);
                sig_valid = 0;
                continue;
            }
            unsigned int dw = phaseptr[mw1 + i];
            unsigned char ch;

            if (i == 0 && is_initial) {
                // First content word on initial fragment:
                // bits 0-6 = signature (extract but don't output)
                // bits 7-13 = char1, bits 14-20 = char2
                rx_sig = dw & 0x7Fu;
                ch = (dw >> 7) & 0x7Fu;
                if (ch != 0x03) sig_sum += ch;
                currentChar += add_ch(ch, message, currentChar);
                ch = (dw >> 14) & 0x7Fu;
                if (ch != 0x03) sig_sum += ch;
                currentChar += add_ch(ch, message, currentChar);
            } else {
                // Normal word or continuation fragment: 3 chars per word
                ch = dw & 0x7Fu;
                if (ch != 0x03) sig_sum += ch;
                currentChar += add_ch(ch, message, currentChar);
                ch = (dw >> 7) & 0x7Fu;
                if (ch != 0x03) sig_sum += ch;
                currentChar += add_ch(ch, message, currentChar);
                ch = (dw >> 14) & 0x7Fu;
                if (ch != 0x03) sig_sum += ch;
                currentChar += add_ch(ch, message, currentChar);
            }
        }
        message[currentChar] = '\0';

        // Verify signature on complete (non-fragmented) messages
        int sig_fail = 0;
        if (is_initial && cont == 0 && len > 0) {
          if (sig_valid) {
            uint32_t expected_sig = (~sig_sum) & 0x7Fu;
            if (rx_sig != expected_sig) {
              verbprintf(3, "FLEX_NEXT: Alpha signature FAIL: rx=0x%02X expected=0x%02X\n", rx_sig, expected_sig);
              sig_fail = 1;
            }
          } else {
            /* BCH errors prevented signature verification - treat as fail */
            sig_fail = 1;
          }
        }

        // Fragment reassembly
        if (frag_flag == 'F') {
          // First/middle fragment: buffer it
          unsigned int abs_frame = flex->FIW.cycleno * 128 + flex->FIW.frameno;
          int slot = frag_find(flex, flex->Decode.capcode, flex->Decode.type, msg_n);
          if (slot >= 0 && is_initial) {
            /* New initial fragment for same capcode: emit old partial, then release */
            if (flex->FragStore.slots[slot].data_len > 0) {
              if (json_mode) {
                flex_next_json_emit(flex, flex->FragStore.slots[slot].phase,
                                    flex->FragStore.slots[slot].capcode,
                                    flex->FragStore.slots[slot].addr_type, 0,
                                    flex->FragStore.slots[slot].type,
                                    "ALN", "reassembled_partial",
                                    flex->FragStore.slots[slot].msg_n, -1, -1,
                                    flex->FragStore.slots[slot].k_fail ? 0 : -1, -1,
                                    (const char *)flex->FragStore.slots[slot].data,
                                    NULL, 0, NULL);
              } else {
                char flex_ts[32];
                flex_local_timestamp(flex_ts, sizeof(flex_ts));
                verbprintf(0, "FLEX_NEXT|%s|%u/%u|%02u.%03u.%c|%010" PRId64 "|%c|ALN|PARTIAL.N%d%s|%.*s\n",
                           flex_ts,
                           flex->Sync.baud, flex->Sync.levels,
                           flex->FIW.cycleno, flex->FIW.frameno,
                           flex->FragStore.slots[slot].phase,
                           flex->FragStore.slots[slot].capcode,
                           flex->FragStore.slots[slot].addr_type,
                           flex->FragStore.slots[slot].msg_n,
                           flex->FragStore.slots[slot].k_fail ? ".K-" : ".K+",
                           (int)flex->FragStore.slots[slot].data_len,
                           flex->FragStore.slots[slot].data);
              }
            }
            frag_release(flex, slot);
            slot = -1;
          }
          if (slot < 0)
            slot = frag_alloc(flex, flex->Decode.capcode, flex->Decode.type, msg_n, abs_frame);
          if (slot >= 0) {
            /* Store phase/addr_type from first fragment seen in this slot */
            if (flex->FragStore.slots[slot].phase == '?') {
              flex->FragStore.slots[slot].phase = flex->Decode.phase;
              flex->FragStore.slots[slot].addr_type = flex->Decode.addr_type;
            }
            if (is_initial) {
              frag_append(flex, slot, message, (unsigned int)currentChar);
              flex->FragStore.slots[slot].rx_sig = rx_sig;
              flex->FragStore.slots[slot].sig_sum = sig_sum;
              flex->FragStore.slots[slot].sig_valid = sig_valid;
              flex->FragStore.slots[slot].k_fail = k_fail;
              flex->FragStore.slots[slot].msg_r = msg_r;
              flex->FragStore.slots[slot].msg_m = msg_m;
              // F=11 (initial): next expected is F=00
              flex->FragStore.slots[slot].expected_f = 0;
              flex->FragStore.slots[slot].frag_index = 0;
            } else {
              flex->FragStore.slots[slot].sig_sum += sig_sum;
              if (!sig_valid) flex->FragStore.slots[slot].sig_valid = 0;
              if (k_fail) flex->FragStore.slots[slot].k_fail = 1;
              // Check F sequence: expected_f should match received frag
              if (frag != flex->FragStore.slots[slot].expected_f) {
                verbprintf(3, "FLEX_NEXT: F sequence mismatch for cap %" PRId64 ": expected F=%d got F=%d (missing fragment?)\n",
                           flex->Decode.capcode, flex->FragStore.slots[slot].expected_f, frag);
                flex->FragStore.slots[slot].f_mismatch = 1;
                // Insert gap marker for missing fragment(s)
                frag_append(flex, slot, (const unsigned char *)"<...>", 5);
              }
              frag_append(flex, slot, message, (unsigned int)currentChar);
              // Advance expected_f: mod 3 cycle (0->1->2->0->1->2...)
              flex->FragStore.slots[slot].expected_f = (frag + 1) % 3;
              flex->FragStore.slots[slot].frag_index++;
            }
          }
          // Output complete level-0 line FIRST (before any debug)
          if (!json_mode) {
            int out_r = is_initial ? msg_r : (slot >= 0 ? flex->FragStore.slots[slot].msg_r : -1);
            int out_m = is_initial ? msg_m : (slot >= 0 ? flex->FragStore.slots[slot].msg_m : -1);
            const char *k_str = k_fail ? ".K-" : ".K+";
            if (out_r >= 0)
              verbprintf(0, "%s|%s|%c.%d/%d.N%d.R%d%s%s%s|%s\n", flex->line_prefix, flex->line_type_tag, frag_flag, slot >= 0 ? flex->FragStore.slots[slot].frag_index : 0, frag, msg_n, out_r, out_m ? ".M" : "", k_str, grp_tag, message);
            else
              verbprintf(0, "%s|%s|%c.%d/%d.N%d%s%s|%s\n", flex->line_prefix, flex->line_type_tag, frag_flag, slot >= 0 ? flex->FragStore.slots[slot].frag_index : 0, frag, msg_n, k_str, grp_tag, message);
          } else {
            int out_r = is_initial ? msg_r : (slot >= 0 ? flex->FragStore.slots[slot].msg_r : -1);
            int out_m = is_initial ? msg_m : (slot >= 0 ? flex->FragStore.slots[slot].msg_m : -1);
            cJSON *extra = cJSON_CreateObject();
            cJSON_AddNumberToObject(extra, "frag_seq", frag);
            if (slot >= 0) {
              cJSON_AddNumberToObject(extra, "frag_index", flex->FragStore.slots[slot].frag_index);
              cJSON_AddBoolToObject(extra, "frag_seq_error", flex->FragStore.slots[slot].f_mismatch ? 1 : 0);
            }
            cJSON_AddNumberToObject(extra, "frag_words", (int)len);
            cJSON_AddNumberToObject(extra, "frag_chars", currentChar);
            flex_next_json_emit(flex, flex->Decode.phase, flex->Decode.capcode,
                                flex->Decode.addr_type, flex_groupmessage, flex->Decode.type,
                                "ALN", is_initial ? "fragment_start" : "fragment_mid",
                                msg_n, out_r, out_m, k_fail ? 0 : -1, -1,
                                (const char *)message, NULL, 0, extra);
          }
          // Debug logging AFTER the level-0 line is complete
          if (is_initial)
            verbprintf(3, "FLEX_NEXT: Frag F initial: rx_sig=0x%02X sig_sum=0x%04X chars=%d len=%u\n",
                       rx_sig, sig_sum, currentChar, len);
          else
            verbprintf(3, "FLEX_NEXT: Frag F cont: sig_sum=0x%04X chars=%d len=%u\n",
                       sig_sum, currentChar, len);
          if (slot >= 0)
            verbprintf(3, "FLEX_NEXT: Buffered fragment for cap %" PRId64 " (%d bytes in slot %d)\n",
                       flex->Decode.capcode, flex->FragStore.slots[slot].data_len, slot);
          /* Mark group as delivering so the FIW timeout check skips it.
           * Cleared on teardown when the message completes (C=0). */
          if (flex_groupmessage && flex_groupbit >= 0 && flex_groupbit < GROUP_BITS &&
              flex->GroupHandler.GroupFrame[flex_groupbit] >= 0) {
            flex->GroupHandler.GroupDelivering[flex_groupbit] = 1;
            flex->GroupHandler.GroupLastFragFrame[flex_groupbit] = flex->FIW.cycleno * 128 + flex->FIW.frameno;
            flex->GroupHandler.GroupTimeoutCount[flex_groupbit] = 0;
          }
          return;
        }

        if (frag_flag == 'C') {
          // Continuation/last fragment: combine with buffered F fragments
          int slot = frag_find(flex, flex->Decode.capcode, flex->Decode.type, msg_n);
          if (slot >= 0 && flex->FragStore.slots[slot].data_len > 0) {
            // Validate signature across all fragments
            int reassembled_sig_fail = 0;
            if (flex->FragStore.slots[slot].sig_valid && sig_valid) {
              uint32_t total_sig_sum = flex->FragStore.slots[slot].sig_sum + sig_sum;
              uint32_t expected_sig = (~total_sig_sum) & 0x7Fu;
              if (flex->FragStore.slots[slot].rx_sig != expected_sig)
                reassembled_sig_fail = 1;
            } else {
              /* BCH errors prevented signature verification - treat as fail */
              reassembled_sig_fail = 1;
            }
            // Check F sequence on final fragment
            if (frag != flex->FragStore.slots[slot].expected_f) {
              verbprintf(3, "FLEX_NEXT: F sequence mismatch on final frag for cap %" PRId64 ": expected F=%d got F=%d\n",
                         flex->Decode.capcode, flex->FragStore.slots[slot].expected_f, frag);
              flex->FragStore.slots[slot].f_mismatch = 1;
              // Insert gap marker for missing fragment(s) before final piece
              frag_append(flex, slot, (const unsigned char *)"<...>", 5);
            }
            int total_frags = flex->FragStore.slots[slot].frag_index + 2; // +1 for initial, +1 for this final
            int total_chars = (int)flex->FragStore.slots[slot].data_len + currentChar;
            // Output complete level-0 line atomically (message + group capcodes in prefix)
            if (!json_mode) {
              int out_r = flex->FragStore.slots[slot].msg_r;
              int out_m = flex->FragStore.slots[slot].msg_m;
              int combined_k_fail = k_fail || flex->FragStore.slots[slot].k_fail;
              const char *k_str = combined_k_fail ? ".K-" : ".K+";
              const char *sig_str = reassembled_sig_fail ? ".SIG-" : ".SIG+";
              if (out_r >= 0)
                verbprintf(0, "%s|%s|%c.%d/%d.N%d.R%d%s%s%s%s|%.*s%s\n", flex->line_prefix, flex->line_type_tag, frag_flag, flex->FragStore.slots[slot].frag_index + 1, frag, msg_n, out_r, out_m ? ".M" : "", k_str, sig_str, grp_tag,
                           (int)flex->FragStore.slots[slot].data_len, flex->FragStore.slots[slot].data, message);
              else
                verbprintf(0, "%s|%s|%c.%d/%d.N%d%s%s%s|%.*s%s\n", flex->line_prefix, flex->line_type_tag, frag_flag, flex->FragStore.slots[slot].frag_index + 1, frag, msg_n, k_str, sig_str, grp_tag,
                           (int)flex->FragStore.slots[slot].data_len, flex->FragStore.slots[slot].data, message);
            } else {
              char reassembled[MAX_ALN * 2];
              snprintf(reassembled, sizeof(reassembled), "%.*s%s",
                       (int)flex->FragStore.slots[slot].data_len,
                       flex->FragStore.slots[slot].data, message);
              int out_r = flex->FragStore.slots[slot].msg_r;
              int out_m = flex->FragStore.slots[slot].msg_m;
              cJSON *extra = cJSON_CreateObject();
              cJSON_AddNumberToObject(extra, "total_fragments", total_frags);
              cJSON_AddNumberToObject(extra, "total_chars", total_chars);
              cJSON_AddBoolToObject(extra, "frag_seq_error", flex->FragStore.slots[slot].f_mismatch ? 1 : 0);
              int combined_k_fail = k_fail || flex->FragStore.slots[slot].k_fail;
              flex_next_json_emit(flex, flex->Decode.phase, flex->Decode.capcode,
                                  flex->Decode.addr_type, flex_groupmessage, flex->Decode.type,
                                  "ALN", "reassembled",
                                  msg_n, out_r, out_m,
                                  combined_k_fail ? 0 : 1, reassembled_sig_fail ? 0 : 1,
                                  reassembled,
                                  flex_groupmessage ? &flex->GroupHandler.GroupCodes[flex_groupbit][1] : NULL,
                                  flex_groupmessage ? (int)flex->GroupHandler.GroupCodes[flex_groupbit][CAPCODES_INDEX] : 0, extra);
            }
            // Debug logging AFTER level-0 line
            verbprintf(3, "FLEX_NEXT: Reassembled %u + %d bytes for cap %" PRId64 "\n",
                       flex->FragStore.slots[slot].data_len, currentChar, flex->Decode.capcode);
            frag_release(flex, slot);
            if (!json_mode) goto group_output;
            if (flex_groupmessage) {
              flex->GroupHandler.GroupCodes[flex_groupbit][CAPCODES_INDEX] = 0;
              flex->GroupHandler.GroupFrame[flex_groupbit] = -1;
              flex->GroupHandler.GroupCycle[flex_groupbit] = -1;
              flex->GroupHandler.GroupDelivering[flex_groupbit] = 0;
            }
            return;
          }
          // No buffered fragment found, output what we have
          if (!json_mode) {
            verbprintf(0, "%s|%s|%c.0/%d.N%d%s%s|%s\n", flex->line_prefix, flex->line_type_tag, frag_flag, frag, msg_n, k_fail ? ".K-" : ".K+", grp_tag, message);
          } else {
            flex_next_json_emit(flex, flex->Decode.phase, flex->Decode.capcode,
                                flex->Decode.addr_type, flex_groupmessage, flex->Decode.type,
                                "ALN", "fragment_end",
                                msg_n, -1, -1, k_fail ? 0 : -1, -1,
                                (const char *)message, NULL, 0, NULL);
          }
          if (!json_mode) goto group_output;
          if (flex_groupmessage) {
            flex->GroupHandler.GroupCodes[flex_groupbit][CAPCODES_INDEX] = 0;
            flex->GroupHandler.GroupFrame[flex_groupbit] = -1;
            flex->GroupHandler.GroupCycle[flex_groupbit] = -1;
            flex->GroupHandler.GroupDelivering[flex_groupbit] = 0;
          }
          return;
        }

        // K (complete) or unknown: output directly
        if (!json_mode) {
          const char *dup_str = (dedup_flag == 2) ? ".DUP+" : (dedup_flag == 1) ? ".DUP" : "";
          const char *k_str = k_fail ? ".K-" : ".K+";
          const char *sig_str = sig_fail ? ".SIG-" : ".SIG+";
          const char *prio_str = flex->Decode.is_priority ? ".P" : "";
          verbprintf(0, "%s|%s|%c.0/%d.N%d.R%d%s%s%s%s%s%s|%s\n", flex->line_prefix, flex->line_type_tag, frag_flag, frag, msg_n, msg_r, msg_m ? ".M" : "", prio_str, k_str, sig_str, dup_str, grp_tag, message);
        } else {
          flex_next_json_emit(flex, flex->Decode.phase, flex->Decode.capcode,
                              flex->Decode.addr_type, flex_groupmessage, flex->Decode.type,
                              "ALN", "complete",
                              msg_n, msg_r, msg_m,
                              k_fail ? 0 : 1, sig_fail ? 0 : 1,
                              (const char *)message,
                              flex_groupmessage ? &flex->GroupHandler.GroupCodes[flex_groupbit][1] : NULL,
                              flex_groupmessage ? (int)flex->GroupHandler.GroupCodes[flex_groupbit][CAPCODES_INDEX] : 0, NULL);
        }

group_output:
// Implemented bierviltje code from ticket: https://github.com/EliasOenal/multimon-ng/issues/123# 
        if(flex_groupmessage == 1) {
                if(flex_groupbit < 0 || flex_groupbit >= GROUP_BITS) return;

                int endpoint = flex_group_endpoint(&flex->GroupHandler, flex_groupbit);
                for(int g = 1; g <= endpoint;g++)
                {
                        verbprintf(3, "FLEX_NEXT: Group message: Groupbit: %i Total Capcodes; %i; index %i; Capcode: [%010" PRId64 "]\n", flex_groupbit, endpoint, g, flex->GroupHandler.GroupCodes[flex_groupbit][g]);
                }

                // reset the value
                flex->GroupHandler.GroupCodes[flex_groupbit][CAPCODES_INDEX] = 0;
                flex->GroupHandler.GroupFrame[flex_groupbit] = -1;
                flex->GroupHandler.GroupCycle[flex_groupbit] = -1;
                flex->GroupHandler.GroupDelivering[flex_groupbit] = 0;
                verbprintf(3, "FLEX_NEXT: Group teardown after delivery: groupbit=%d\n", flex_groupbit);
        } 
}

static void parse_numeric(struct Flex_Next * flex, unsigned int * phaseptr, int * bch_err, int j, int frag, int cont, int msg_n, int msg_r, int msg_m, int dedup_flag) {
  if (flex==NULL) return;
  (void)frag; (void)cont;  /* Numeric messages cannot be fragmented */
  // BCD table per ARIB STD-43A Section 3.10.1.1:
  //   0-9 = digits, A = spare('.'), B = urgency('U'),
  //   C = space(' '), D = hyphen('-'), E = ']', F = '['
  unsigned const char flex_bcd[17] = "0123456789.U -][";

  // Frag flags output is deferred until after K checksum verification.
  int num_k_fail = 0;

  // Numeric vector layout (Section 3.9.5):
  //   bits 7-13:  b (message start word)
  //   bits 14-16: n (word count - 1, 3 bits, max 7 = 8 words)
  //   bits 17-20: K3-K0 (lower 4 bits of 6-bit K checksum)
  int w1 = phaseptr[j] >> 7;
  int w2 = w1 >> 7;
  w1 = w1 & 0x7f;
  w2 = (w2 & 0x07) + w1;  // w2 = start + word_count - 1

  // Validate w1 and w2 against frame bounds
  if (w1 >= PHASE_WORDS || w2 >= PHASE_WORDS) {
    verbprintf(3, "FLEX_NEXT: Numeric w1=%d w2=%d out of frame bounds (%d), skipping\n", w1, w2, PHASE_WORDS);
    return;
  }

  // K3-K0 from vector word
  unsigned int vec_k30 = (phaseptr[j] >> 17) & 0x0F;

  // Get first dataword from message field or from second
  // vector word if long address
  int dw;
  int dw_bad = 0;
  if(!flex->Decode.long_address) {
    dw_bad = (w1 < PHASE_WORDS && bch_err[w1]);
    dw = phaseptr[w1];
    w1++;
    w2++;
  } else {
    dw_bad = ((j+1) < PHASE_WORDS && bch_err[j+1]);
    dw = phaseptr[j+1];
  }

  // For numbered numeric (V=111), extract header fields before BCD digits
  int num_special_format = -1;  // S flag: 0=standard, 1=ID-ROM display (-1=N/A)
  if(flex->Decode.type == FLEX_PAGETYPE_NUMBERED_NUMERIC && !dw_bad) {
    // bits 0-1: K5K4 (part of checksum)
    // bits 2-7: N (message number, 6 bits)
    // bit 8:    R (retrieval: 1=new, 0=retransmit)
    // bit 9:    S (special format: 0=standard, 1=ID-ROM)
    unsigned int nnum_n = (dw >> 2) & 0x3F;
    unsigned int nnum_r = (dw >> 8) & 0x01;
    num_special_format = (dw >> 9) & 0x01;
    verbprintf(3, "FLEX_NEXT: Numbered numeric: N=%u R=%u S=%u\n", nnum_n, nnum_r, num_special_format);
  }

  // K checksum verification (Section 3.10.1.1.1):
  // K3-K0 from vector word bits 17-20, K5K4 from body word 0 bits 0-1.
  // Recompute: sum 3 groups per word (bits 0-7, 8-15, 16-20),
  // with K5K4 bits zeroed in the body word that contains them.
  // Fold: take lower 8 bits, add (low6) + (high2), 1's complement lower 6.
  {
    uint32_t k_sum = 0;
    int k_ok = 1;
    int ki;
    // body0 is the first body word: j+1 for long addr, w1-1 for short addr
    int body0 = flex->Decode.long_address ? (j + 1) : (w1 - 1);

    // Include body0 with K5K4 zeroed
    if (body0 >= 0 && body0 < PHASE_WORDS && !bch_err[body0]) {
      uint32_t dw = phaseptr[body0] & ~0x03u;  // zero K5K4 (bits 0-1)
      k_sum += dw & 0xFFu;
      k_sum += (dw >> 8) & 0xFFu;
      k_sum += (dw >> 16) & 0x1Fu;
    } else {
      k_ok = 0;
    }

    // Include remaining body words w1..w2-1 (w2 was incremented with w1,
    // so ki < w2 covers all message words after body0).
    for (ki = w1; ki < w2 && ki < PHASE_WORDS; ki++) {
      if (bch_err[ki]) { k_ok = 0; continue; }
      k_sum += phaseptr[ki] & 0xFFu;
      k_sum += (phaseptr[ki] >> 8) & 0xFFu;
      k_sum += (phaseptr[ki] >> 16) & 0x1Fu;
    }

    if (k_ok && !dw_bad) {
      k_sum &= 0xFFu;
      uint32_t k6 = (~((k_sum & 0x3F) + (k_sum >> 6))) & 0x3Fu;
      unsigned int rx_k54 = dw_bad ? 0 : (phaseptr[body0] & 0x03u);
      unsigned int rx_k = (rx_k54 << 4) | vec_k30;
      unsigned int expected_k = k6;
      if (rx_k != expected_k) {
        verbprintf(3, "FLEX_NEXT: Numeric K checksum FAIL: rx=0x%02X exp=0x%02X\n",
                   rx_k, expected_k);
        num_k_fail = 1;
      }
    } else {
      /* BCH errors prevented K verification - treat as fail */
      num_k_fail = 1;
    }
  }

  // Output frag flags with N/R/M/K-/DUP
  // Numeric messages are always complete per spec.

  // BCD digit extraction
  char num_msg[256];
  int num_pos = 0;
  unsigned char digit = 0;
  int count = 4;
  if(flex->Decode.type == FLEX_PAGETYPE_NUMBERED_NUMERIC) {
    count += 10;        // Skip 10 header bits (K5K4 + N + R + S)
  } else {
    count += 2;         // Skip 2 header bits (K5K4)
  }
  int i;
  for(i = w1; i <= w2; i++) {
    int k;
    for(k = 0; k < 21; k++) {
      digit = (digit >> 1) & 0x0F;
      if(dw & 0x01) {
        digit ^= 0x08;
      }
      dw >>= 1;
      if(--count == 0) {
        if(dw_bad) {
          if (num_pos < 255) num_msg[num_pos++] = '?';
        } else {
          // Output all BCD digits including space fill (0x0C).
          // Do NOT skip any digits here - the K checksum covers
          // all BCD positions in the message words, so dropping
          // characters would cause the checksum to appear wrong.
          if (num_pos < 255) num_msg[num_pos++] = flex_bcd[digit];
        }
        count = 4;
      }
    }
    // Load next word for the next iteration, check BCH status.
    // Guard: only load when another iteration follows (i+1 <= w2)
    // to avoid reading one word past the message.
    // Note: phaseptr[i] is correct here because w1 was incremented
    // after pre-loading the first body word into dw.
    if (i + 1 <= w2 && i < PHASE_WORDS) {
      dw_bad = bch_err[i];
      dw = phaseptr[i];
    }
  }

  if (!json_mode) {
    const char *dup_str = (dedup_flag == 2) ? ".DUP+" : (dedup_flag == 1) ? ".DUP" : "";
    const char *k_str = num_k_fail ? ".K-" : ".K+";
    const char *prio_str = flex->Decode.is_priority ? ".P" : "";
    num_msg[num_pos] = '\0';
    verbprintf(0, "%s|%s|K.0/3.N%d.R%d%s%s%s%s|%s\n", flex->line_prefix, flex->line_type_tag, msg_n, msg_r, msg_m ? ".M" : "", prio_str, k_str, dup_str, num_msg);
  }

  if (json_mode) {
    num_msg[num_pos] = '\0';
    const char *tag = (flex->Decode.type == FLEX_PAGETYPE_SPECIAL_NUMERIC) ? "SNUM" :
                      (flex->Decode.type == FLEX_PAGETYPE_NUMBERED_NUMERIC) ? "NNUM" : "NUM";
    const char *frag_str;
    /* Numeric messages cannot be fragmented (ARIB STD-43A Section 4.2). */
    frag_str = "complete";
    cJSON *extra = NULL;
    if (num_special_format >= 0) {
      extra = cJSON_CreateObject();
      cJSON_AddStringToObject(extra, "special_format", num_special_format ? "id_rom" : "standard");
    }
    flex_next_json_emit(flex, flex->Decode.phase, flex->Decode.capcode, flex->Decode.addr_type,
                        0, flex->Decode.type, tag, frag_str,
                        msg_n, msg_r, msg_m, num_k_fail ? 0 : 1, -1,
                        num_msg, NULL, 0, extra);
  }
}


static void parse_short_message(struct Flex_Next * flex, unsigned int * phaseptr, int * bch_err, int j) {
  if (flex==NULL) return;
  // BCD table per ARIB STD-43A Section 3.10.1.1
  unsigned const char flex_bcd[17] = "0123456789.U -][";

  // Short Message Vector layout (Section 3.9.2, Table 3.9.2-1):
  //   bits 4-6:   V = 010 (tone/short message type)
  //   bits 7-8:   t1,t0 (sub-type)
  //   bits 9-20:  d (data field, meaning depends on sub-type)
  //
  // Sub-types:
  //   t=00: Numeric - 3 BCD digits (short) or 8 digits (long)
  //         All-space digits = tone-only alert (no data content)
  //   t=01: Source - S2S1S0 source code (0-7)
  //   t=10: Numbered - S2S1S0 + N5-N0 + R0
  //   t=11: Reserved

  unsigned int sub_type = (phaseptr[j] >> 7) & 0x03;

  switch (sub_type) {
  case 0: {
    // t=00: Short Numeric Message or Network ID
    // When address is a Network Address (Section 3.9.2, Table 3.9.2-1 (1)):
    //   d0-d4 = A0-A4 (Service Area Identifier)
    //   d5-d7 = M0-M2 (Multiplier)
    //   d8-d11 = F0-F3 (Traffic Management Flags)
    // When address is other than Network (Table 3.9.2-1 (2)):
    //   Short addr: 3 BCD digits in d0-d11
    //   Long addr: 3 digits in 1st vector + 5 digits in 2nd vector = 8 digits
    if (flex->Decode.addr_type == 'N') {
      unsigned int area_id = (phaseptr[j] >> 9) & 0x1F;
      unsigned int multiplier = (phaseptr[j] >> 14) & 0x07;
      unsigned int tmf = (phaseptr[j] >> 17) & 0x0F;
      if (!json_mode) {
        verbprintf(0, "%s|SMSG|K.NID|service_area=%u multiplier=%u traffic_flags=0x%X\n", flex->line_prefix, area_id, multiplier, tmf);
      } else {
        cJSON *extra = cJSON_CreateObject();
        cJSON_AddStringToObject(extra, "smsg_sub_type", "network_id");
        cJSON_AddNumberToObject(extra, "area_id", area_id);
        cJSON_AddNumberToObject(extra, "multiplier", multiplier);
        cJSON_AddNumberToObject(extra, "tmf", tmf);
        char nid_msg[64];
        snprintf(nid_msg, sizeof(nid_msg), "NID area=%u mult=%u tmf=0x%X", area_id, multiplier, tmf);
        flex_next_json_emit(flex, flex->Decode.phase, flex->Decode.capcode, flex->Decode.addr_type,
                            0, FLEX_PAGETYPE_SHORT_MESSAGE, "SMSG", "complete",
                            -1, -1, -1, -1, -1, nid_msg, NULL, 0, extra);
      }
      break;
    }

    // Non-network: Short Numeric Message
    char digits[9];
    int ndigits = 0;
    int all_space = 1;
    int i;

    for (i = 9; i <= 17; i += 4) {
      unsigned char d = (phaseptr[j] >> i) & 0x0F;
      digits[ndigits++] = flex_bcd[d];
      if (d != 0x0C) all_space = 0;
    }
    if (flex->Decode.long_address) {
      if ((j+1) < PHASE_WORDS && !bch_err[j+1]) {
        for (i = 0; i <= 16; i += 4) {
          unsigned char d = (phaseptr[j+1] >> i) & 0x0F;
          digits[ndigits++] = flex_bcd[d];
          if (d != 0x0C) all_space = 0;
        }
      } else {
        // 2nd vector word uncorrectable
        for (i = 0; i < 5; i++)
          digits[ndigits++] = '?';
        all_space = 0;
      }
    }
    digits[ndigits] = '\0';

    if (all_space) {
      // Pure tone-only: all digits are space fill, no data content
      if (!json_mode) {
        verbprintf(0, "%s|SMSG|K.TON|\n", flex->line_prefix);
      } else {
        flex_next_json_emit(flex, flex->Decode.phase, flex->Decode.capcode, flex->Decode.addr_type,
                            0, FLEX_PAGETYPE_SHORT_MESSAGE, "TON", "complete",
                            -1, -1, -1, -1, -1, NULL, NULL, 0, NULL);
      }
    } else {
      // Short numeric message with actual digit content
      if (!json_mode) {
        verbprintf(0, "%s|SMSG|K.NUM|%s\n", flex->line_prefix, digits);
      } else {
        cJSON *extra = cJSON_CreateObject();
        cJSON_AddStringToObject(extra, "smsg_sub_type", "numeric");
        flex_next_json_emit(flex, flex->Decode.phase, flex->Decode.capcode, flex->Decode.addr_type,
                            0, FLEX_PAGETYPE_SHORT_MESSAGE, "SMSG", "complete",
                            -1, -1, -1, -1, -1, digits, NULL, 0, extra);
      }
    }
    break;
  }
  case 1: {
    // t=01: Source Code
    // d0-d2 = S2S1S0 (source code 0-7)
    unsigned int source = (phaseptr[j] >> 9) & 0x07;
    if (!json_mode) {
      verbprintf(0, "%s|SMSG|K.SRC|SRC=%u\n", flex->line_prefix, source);
    } else {
      cJSON *extra = cJSON_CreateObject();
      cJSON_AddStringToObject(extra, "smsg_sub_type", "source");
      cJSON_AddNumberToObject(extra, "source_code", source);
      char src_msg[32];
      snprintf(src_msg, sizeof(src_msg), "SRC=%u", source);
      flex_next_json_emit(flex, flex->Decode.phase, flex->Decode.capcode, flex->Decode.addr_type,
                          0, FLEX_PAGETYPE_SHORT_MESSAGE, "SMSG", "complete",
                          -1, -1, -1, -1, -1, src_msg, NULL, 0, extra);
    }
    break;
  }
  case 2: {
    // t=10: Numbered Short Message
    // d0-d2 = S2S1S0 (source code 0-7)
    // d3-d8 = N5-N0 (message number 0-63)
    // d9    = R0 (retrieval flag: 1=new, 0=retransmit)
    unsigned int source = (phaseptr[j] >> 9) & 0x07;
    unsigned int msg_n  = (phaseptr[j] >> 12) & 0x3F;
    unsigned int msg_r  = (phaseptr[j] >> 18) & 0x01;
    if (!json_mode) {
      verbprintf(0, "%s|SMSG|K.SRC.N%u.R%u|SRC=%u\n", flex->line_prefix, msg_n, msg_r, source);
    } else {
      cJSON *extra = cJSON_CreateObject();
      cJSON_AddStringToObject(extra, "smsg_sub_type", "numbered");
      cJSON_AddNumberToObject(extra, "source_code", source);
      char num_msg[64];
      snprintf(num_msg, sizeof(num_msg), "S=%u N=%u R=%u", source, msg_n, msg_r);
      flex_next_json_emit(flex, flex->Decode.phase, flex->Decode.capcode, flex->Decode.addr_type,
                          0, FLEX_PAGETYPE_SHORT_MESSAGE, "SMSG", "complete",
                          (int)msg_n, (int)msg_r, -1, -1, -1, num_msg, NULL, 0, extra);
    }
    break;
  }
  default:
    // t=11: Reserved
    if (!json_mode) {
      verbprintf(0, "%s|SMSG|K.RSV|subtype=%u data=0x%03X\n", flex->line_prefix, sub_type, (phaseptr[j] >> 9) & 0xFFF);
    } else {
      cJSON *extra = cJSON_CreateObject();
      cJSON_AddStringToObject(extra, "smsg_sub_type", "reserved");
      char rsvd_msg[64];
      snprintf(rsvd_msg, sizeof(rsvd_msg), "RSVD t=%u d=0x%03X", sub_type, (phaseptr[j] >> 9) & 0xFFF);
      flex_next_json_emit(flex, flex->Decode.phase, flex->Decode.capcode, flex->Decode.addr_type,
                          0, FLEX_PAGETYPE_SHORT_MESSAGE, "SMSG", "complete",
                          -1, -1, -1, -1, -1, rsvd_msg, NULL, 0, extra);
    }
    break;
  }
}

static void parse_binary(struct Flex_Next * flex, unsigned int * phaseptr, int * bch_err, unsigned int mw1, unsigned int len, int frag, int cont, int msg_n, int msg_r, int msg_m, int dedup_flag) {
  if (flex==NULL) return;

  // HEX/Binary message body layout (Section 3.10.1.2, Fig 3.10.1.2-1):
  //
  // Initial fragment (F=11):
  //   Word at mw1 = Header word 2 (hdr2):
  //     bit  0:     R (retrieval: 1=new, 0=retransmit)
  //     bit  1:     M (maildrop)
  //     bit  2:     D (display direction: 0=LTR, 1=RTL)
  //     bit  3:     H (header message flag)
  //     bits 4-7:   B (blocking length, 0=16 bits/char)
  //     bit  8:     I (status info field enabler)
  //     bits 9-12:  s (reserved, default 0000)
  //     bits 13-20: S (8-bit signature)
  //   Words mw1+1 .. mw1+len-1 = data words (21 bits each)
  //
  // Continuation fragment (F!=11):
  //   All words mw1 .. mw1+len-1 are data words (21 bits each)
  //
  // Data is a continuous bit stream packed across 21-bit words.
  // Nibbles (4 bits each) are extracted LSB-first within each word.
  // Total data bits = number_of_data_words * 21 + partial_bits_from_hdr2.
  //
  // Termination fill (Section 3.10.1.2):
  //   After the last data nibble, remaining bits in the last word
  //   are filled with the inverse of the last data bit.
  //   If the last data nibble is all-0 or all-1, an extra fill word
  //   (all-0 or all-1) is appended per rule (3).
  //   We detect fill by scanning backwards from bit 20 of the last
  //   word: consecutive identical bits from the top are fill.

  int is_initial = (frag == 3);
  unsigned int data_start = mw1;

  int hex_blocking = 0;   // B field (4 bits): bits per character/data unit.
                          //   0000=16 bits, 0001=1 bit (default), ..., 1111=15 bits.
                          //   Defines character boundaries for termination fill stripping.
  int hex_display_rtl = 0; // D field (1 bit): display direction.
                          //   0=left-to-right, 1=right-to-left.
                          //   Only meaningful when B != 0001 (multi-bit characters).
  int hex_header_msg = 0;  // H field (1 bit): header message flag.
                          //   1=this is a displayable header; a transparent data message
                          //   with the same N will follow. Both complete independently.
  int hex_status_info = 0; // I field (1 bit): status information field enabler.
                          //   0=standard HEX/Binary data.
                          //   1=first 8 bits of data field indicate encoding method
                          //   (encoding method definition currently reserved).
  uint32_t hex_rx_sig = 0; // S field (8 bits): message signature.
                          //   1's complement of binary sum of all data bytes
                          //   across all fragments (excluding termination fill).
  int hex_has_sig = 0;
  int hex_sig_fail = 0;

  // Parse hdr2 on initial fragments and extract its 8 data bits
  // HEX hdr2 layout (Section 3.10.1.2, Fig 3.10.1.2-1):
  //   bit  0:     R (retrieval) - already extracted by caller
  //   bit  1:     M (maildrop) - already extracted by caller
  //   bit  2:     D (display direction: 0=LTR, 1=RTL)
  //   bit  3:     H (header message flag)
  //   bits 4-7:   B (blocking length, bits per char; 0000=16)
  //   bit  8:     I (status info field enabler)
  //   bits 9-12:  s (reserved, default 0000)
  //   bits 13-20: S (8-bit signature)
  if (is_initial && len > 0) {
    if (mw1 < PHASE_WORDS && !bch_err[mw1]) {
      unsigned int hdr2 = phaseptr[mw1];
      hex_blocking = (hdr2 >> 4) & 0xF;
      hex_display_rtl = (hdr2 >> 2) & 1;
      hex_header_msg = (hdr2 >> 3) & 1;
      hex_status_info = (hdr2 >> 8) & 1;
      hex_rx_sig = (hdr2 >> 13) & 0xFF;
      hex_has_sig = 1;
      verbprintf(3, "FLEX_NEXT: HEX hdr2=0x%05X R=%u M=%u D=%u H=%u B=%u(%d bits/char) I=%u S=0x%02X\n",
                 hdr2, hdr2 & 1, (hdr2 >> 1) & 1, hex_display_rtl, hex_header_msg,
                 hex_blocking, hex_blocking ? hex_blocking : 16,
                 hex_status_info, hex_rx_sig);
    }
    data_start = mw1 + 1;
    len--;
    // hdr2 contains only control fields (R/M/D/H/B/I/s/S).
    // Data starts in the next word. No data bits in hdr2.
  }

  char hex[512];
  int hi = 0;
  int bit_count = 0;
  unsigned char nibble_acc = 0;
  int total_bits = 0;

  // Extract data bits from data words (all 21 bits each)
  // Extract data bits from data words (all 21 bits each)
  for (unsigned int w = data_start; w < data_start + len && w < PHASE_WORDS; w++) {
    if (bch_err[w]) {
      // Lost word: emit '?' placeholders for 5 nibbles (21 bits / 4 rounded)
      for (int b = 0; b < 21; b++) {
        int nibble_pos = bit_count % 4;
        if (nibble_pos == 0) nibble_acc = 0;
        if (nibble_pos == 3 && hi < (int)sizeof(hex) - 1)
          hex[hi++] = '?';
        bit_count++;
        total_bits++;
      }
      continue;
    }
    unsigned int dw = phaseptr[w];
    for (int b = 0; b < 21; b++) {
      int nibble_pos = bit_count % 4;
      if (nibble_pos == 0)
        nibble_acc = 0;
      nibble_acc |= ((dw >> b) & 1) << nibble_pos;
      if (nibble_pos == 3 && hi < (int)sizeof(hex) - 1)
        hex[hi++] = "0123456789ABCDEF"[nibble_acc & 0xF];
      bit_count++;
      total_bits++;
    }
  }

  // Strip termination fill from last/complete fragment (C=0).
  //
  // Per Section 3.10.1.2, termination fill rules:
  //   (2) When data ends mid-word, remaining bits are filled with
  //       the inverse of the last data bit.
  //   (3) When data ends exactly at a word boundary AND the last
  //       character is all-0s or all-1s, an extra fill word is
  //       appended (all bits = inverse of last data bit).
  //
  // Stripping: either strip a whole extra fill word, or strip
  // partial fill bits from the last word.  Never both.
  if (cont == 0 && len > 0) {
    unsigned int last_w = data_start + len - 1;

    // Rule (3): if the last word is entirely fill (0x000000 or
    // 0x1FFFFF), discard it and truncate the hex output to the
    // nibble count before that word.
    if (last_w > data_start && last_w < PHASE_WORDS && !bch_err[last_w]) {
      unsigned int lw = phaseptr[last_w] & 0x1FFFFF;
      if (lw == 0x000000 || lw == 0x1FFFFF) {
        int bits_before = (int)(last_w - data_start) * 21;
        int nibs_before = bits_before / 4;
        if (nibs_before > 0 && nibs_before <= hi) {
          hi = nibs_before;
          last_w = (unsigned int)-1;  // done, skip step 2
        }
      }
    }

    // Rule (2): partial fill in last word.  Scan backwards from
    // bit 20 - consecutive identical bits from the top are fill.
    // The first differing bit marks the end of real data.
    if (last_w != (unsigned int)-1 && last_w < PHASE_WORDS && !bch_err[last_w]) {
      unsigned int lw = phaseptr[last_w];
      unsigned int top_bit = (lw >> 20) & 1;
      int fill_start = 21;
      for (int b = 20; b >= 0; b--) {
        if (((lw >> b) & 1) != top_bit)
          break;
        fill_start = b;
      }
      if (fill_start < 21 && fill_start > 0) {
        int bits_before = (int)(last_w - data_start) * 21;
        int real_bits = bits_before + fill_start;
        int real_nibbles = (real_bits + 3) / 4;
        if (real_nibbles < hi)
          hi = real_nibbles;
      }
    }
  }

  hex[hi] = '\0';

  // HEX signature validation (Section 3.10.1.2):
  // S is the 1's complement of the binary sum of all data bytes
  // (8 bits at a time), starting after the S field.  Termination
  // bits are NOT included.  Only validate on complete messages (K).
  if (is_initial && cont == 0) {
    if (hex_has_sig) {
    int total_data_bits = hi * 4;
    uint32_t sig_sum = 0;
    int bit_pos = 0;
    uint8_t accum = 0;
    int accum_bits = 0;
    // Sum data bits from data words only (hdr2 contains S, not data)
    for (unsigned int w = data_start; w < data_start + len && w < PHASE_WORDS && bit_pos < total_data_bits; w++) {
      if (bch_err[w]) break;
      unsigned int dw = phaseptr[w];
      for (int b = 0; b < 21 && bit_pos < total_data_bits; b++) {
        accum |= ((dw >> b) & 1) << accum_bits;
        accum_bits++;
        bit_pos++;
        if (accum_bits == 8) { sig_sum += accum; accum = 0; accum_bits = 0; }
      }
    }
    // Include partial last byte if any
    if (accum_bits > 0) sig_sum += accum;
    uint32_t expected_sig = (~sig_sum) & 0xFF;
    if (hex_rx_sig != expected_sig) {
      verbprintf(3, "FLEX_NEXT: HEX signature FAIL: rx=0x%02X expected=0x%02X\n",
                 hex_rx_sig, expected_sig);
      hex_sig_fail = 1;
    }
    } else {
      /* BCH errors prevented signature verification - treat as fail */
      hex_sig_fail = 1;
    }
  }

  // Fragment reassembly for HEX/Binary messages.
  // Uses the same F/K/C flags as alpha (from hdr1):
  //   frag==3, cont==0: 'K' - complete message, output directly
  //   cont==1:          'F' - first/middle fragment, buffer it
  //   cont==0, frag!=3: 'C' - final fragment, combine and output
  char frag_flag = '?';
  if (cont == 0 && frag == 3) frag_flag = 'K';
  else if (cont == 1)         frag_flag = 'F';
  else if (cont == 0)         frag_flag = 'C';

  int is_initial_hex = (frag == 0x03);

  if (frag_flag == 'F') {
    // First/middle fragment: buffer it
    unsigned int abs_frame = flex->FIW.cycleno * 128 + flex->FIW.frameno;
    int fslot = frag_find(flex, flex->Decode.capcode, flex->Decode.type, msg_n);
    // R=1 on initial fragment means new message; discard stale slot
    if (fslot >= 0 && is_initial_hex && msg_r == 1) {
      verbprintf(3, "FLEX_NEXT: HEX R=1 new message, releasing stale slot %d for cap %" PRId64 " N=%d\n",
                 fslot, flex->Decode.capcode, msg_n);
      frag_release(flex, fslot);
      fslot = -1;
    }
    if (fslot < 0)
      fslot = frag_alloc(flex, flex->Decode.capcode, flex->Decode.type, msg_n, abs_frame);
    if (fslot >= 0) {
      frag_append(flex, fslot, (const unsigned char *)hex, (unsigned int)hi);
      if (is_initial_hex) {
        flex->FragStore.slots[fslot].msg_r = msg_r;
        flex->FragStore.slots[fslot].msg_m = msg_m;
        flex->FragStore.slots[fslot].hex_blocking = hex_blocking;
        flex->FragStore.slots[fslot].hex_display_rtl = hex_display_rtl;
        flex->FragStore.slots[fslot].hex_header_msg = hex_header_msg;
        flex->FragStore.slots[fslot].hex_status_info = hex_status_info;
      }
      verbprintf(3, "FLEX_NEXT: HEX buffered fragment for cap %" PRId64 " (%d nibbles in slot %d)\n",
                 flex->Decode.capcode, flex->FragStore.slots[fslot].data_len, fslot);
    }
    // Output frag flags
    if (!json_mode) {
      int blk = is_initial_hex ? (hex_blocking ? hex_blocking : 16) : (fslot >= 0 ? (flex->FragStore.slots[fslot].hex_blocking ? flex->FragStore.slots[fslot].hex_blocking : 16) : 0);
      const char *dir = is_initial_hex ? (hex_display_rtl ? "RTL" : "LTR") : (fslot >= 0 ? (flex->FragStore.slots[fslot].hex_display_rtl ? "RTL" : "LTR") : "LTR");
      int out_r = is_initial_hex ? msg_r : (fslot >= 0 ? flex->FragStore.slots[fslot].msg_r : -1);
      int out_m = is_initial_hex ? msg_m : (fslot >= 0 ? flex->FragStore.slots[fslot].msg_m : -1);
      const char *hdr = is_initial_hex ? (hex_header_msg ? ".HDR" : "") : (fslot >= 0 && flex->FragStore.slots[fslot].hex_header_msg ? ".HDR" : "");
      const char *sti = is_initial_hex ? (hex_status_info ? ".STI" : "") : (fslot >= 0 && flex->FragStore.slots[fslot].hex_status_info ? ".STI" : "");
      if (out_r >= 0)
        verbprintf(0, "%s|%s|%c.%d/%d.N%d.R%d%s.B=%d.%s%s%s|%s\n", flex->line_prefix, flex->line_type_tag, frag_flag, fslot >= 0 ? flex->FragStore.slots[fslot].frag_index : 0, frag, msg_n, out_r, out_m ? ".M" : "", blk, dir, hdr, sti, hex);
      else
        verbprintf(0, "%s|%s|%c.%d/%d.N%d.B=%d.%s%s%s|%s\n", flex->line_prefix, flex->line_type_tag, frag_flag, fslot >= 0 ? flex->FragStore.slots[fslot].frag_index : 0, frag, msg_n, blk, dir, hdr, sti, hex);
    } else {
      flex_next_json_emit(flex, flex->Decode.phase, flex->Decode.capcode, flex->Decode.addr_type,
                          0, flex->Decode.type, "HEX", is_initial_hex ? "fragment_start" : "fragment_orphan",
                          msg_n, msg_r, msg_m, -1,
                          hex_sig_fail ? 0 : (hex_has_sig ? 1 : -1),
                          hex, NULL, 0, NULL);
    }
    return;
  }

  if (frag_flag == 'C') {
    // Final fragment: combine with buffered data
    int fslot = frag_find(flex, flex->Decode.capcode, flex->Decode.type, msg_n);
    if (fslot >= 0 && flex->FragStore.slots[fslot].data_len > 0) {
      // Check F sequence on final fragment
      if (frag != flex->FragStore.slots[fslot].expected_f) {
        flex->FragStore.slots[fslot].f_mismatch = 1;
        frag_append(flex, fslot, (const unsigned char *)"<...>", 5);
      }
      if (!json_mode) {
        int out_r = flex->FragStore.slots[fslot].msg_r;
        int out_m = flex->FragStore.slots[fslot].msg_m;
        if (out_r >= 0)
          verbprintf(0, "%s|%s|%c.%d/%d.N%d.R%d%s|%.*s%s\n", flex->line_prefix, flex->line_type_tag, frag_flag, flex->FragStore.slots[fslot].frag_index + 1, frag, msg_n, out_r, out_m ? ".M" : "",
                     (int)flex->FragStore.slots[fslot].data_len, flex->FragStore.slots[fslot].data, hex);
        else
          verbprintf(0, "%s|%s|%c.%d/%d.N%d|%.*s%s\n", flex->line_prefix, flex->line_type_tag, frag_flag, flex->FragStore.slots[fslot].frag_index + 1, frag, msg_n,
                     (int)flex->FragStore.slots[fslot].data_len, flex->FragStore.slots[fslot].data, hex);
      } else {
        // Build reassembled hex string for JSON
        char reassembled[1024];
        int rlen = (int)flex->FragStore.slots[fslot].data_len;
        if (rlen > (int)sizeof(reassembled) - (int)sizeof(hex) - 1)
          rlen = (int)sizeof(reassembled) - (int)sizeof(hex) - 1;
        memcpy(reassembled, flex->FragStore.slots[fslot].data, (unsigned)rlen);
        memcpy(reassembled + rlen, hex, (unsigned)(hi + 1));
        flex_next_json_emit(flex, flex->Decode.phase, flex->Decode.capcode, flex->Decode.addr_type,
                            0, flex->Decode.type, "HEX", "reassembled",
                            msg_n, flex->FragStore.slots[fslot].msg_r,
                            flex->FragStore.slots[fslot].msg_m, -1,
                            hex_sig_fail ? 0 : (hex_has_sig ? 1 : -1),
                            reassembled, NULL, 0, NULL);
      }
      verbprintf(3, "FLEX_NEXT: HEX reassembled %u + %d nibbles for cap %" PRId64 "\n",
                 flex->FragStore.slots[fslot].data_len, hi, flex->Decode.capcode);
      frag_release(flex, fslot);
      return;
    }
    // No buffered fragment, output what we have
    if (!json_mode) {
      verbprintf(0, "%s|%s|%c.0/%d.N%d|%s\n", flex->line_prefix, flex->line_type_tag, frag_flag, frag, msg_n, hex);
    } else {
      flex_next_json_emit(flex, flex->Decode.phase, flex->Decode.capcode, flex->Decode.addr_type,
                          0, flex->Decode.type, "HEX", "fragment_end",
                          msg_n, msg_r, msg_m, -1,
                          hex_sig_fail ? 0 : (hex_has_sig ? 1 : -1),
                          hex, NULL, 0, NULL);
    }
    return;
  }

  // K (complete): output with frag flags and DUP status
  if (!json_mode) {
    const char *dup_str = (dedup_flag == 2) ? ".DUP+" : (dedup_flag == 1) ? ".DUP" : "";
    const char *sig_str = hex_sig_fail ? ".SIG-" : ".SIG+";
    const char *prio_str = flex->Decode.is_priority ? ".P" : "";
    if (is_initial_hex) {
      int blk = hex_blocking ? hex_blocking : 16;
      const char *dir = hex_display_rtl ? "RTL" : "LTR";
      const char *hdr = hex_header_msg ? ".HDR" : "";
      const char *sti = hex_status_info ? ".STI" : "";
      verbprintf(0, "%s|%s|%c.0/%d.N%d.R%d%s%s%s.B=%d.%s%s%s%s|%s\n", flex->line_prefix, flex->line_type_tag, frag_flag, frag, msg_n, msg_r, msg_m ? ".M" : "", prio_str, sig_str, blk, dir, hdr, sti, dup_str, hex);
    } else
      verbprintf(0, "%s|%s|%c.0/%d.N%d%s%s%s|%s\n", flex->line_prefix, flex->line_type_tag, frag_flag, frag, msg_n, prio_str, sig_str, dup_str, hex);
  } else {
    cJSON *extra = NULL;
    if (is_initial_hex) {
      extra = cJSON_CreateObject();
      cJSON_AddNumberToObject(extra, "blocking", hex_blocking ? hex_blocking : 16);
      cJSON_AddBoolToObject(extra, "display_rtl", hex_display_rtl);
      cJSON_AddBoolToObject(extra, "header_msg", hex_header_msg);
      cJSON_AddBoolToObject(extra, "status_info", hex_status_info);
    }
    flex_next_json_emit(flex, flex->Decode.phase, flex->Decode.capcode, flex->Decode.addr_type,
                        0, flex->Decode.type, "HEX", "complete",
                        msg_n, msg_r, msg_m, -1,
                        hex_sig_fail ? 0 : (hex_has_sig ? 1 : -1),
                        hex, NULL, 0, extra);
  }
}


static void decode_phase(struct Flex_Next * flex, char PhaseNo) {
  if (flex==NULL) return;
  verbprintf(3, "FLEX_NEXT: Decoding phase %c\n", PhaseNo);

  uint32_t *phaseptr=NULL;

  switch (PhaseNo) {
    case 'A': phaseptr=flex->Data.PhaseA.buf; break;
    case 'B': phaseptr=flex->Data.PhaseB.buf; break;
    case 'C': phaseptr=flex->Data.PhaseC.buf; break;
    case 'D': phaseptr=flex->Data.PhaseD.buf; break;
  }

  // BCH decode all 88 words first
  // Reset per-phase BCH counters
  {
    struct Flex_Phase *ph = NULL;
    switch (PhaseNo) {
      case 'A': ph = &flex->Data.PhaseA; break;
      case 'B': ph = &flex->Data.PhaseB; break;
      case 'C': ph = &flex->Data.PhaseC; break;
      case 'D': ph = &flex->Data.PhaseD; break;
    }
    if (ph) { ph->bch_0err = 0; ph->bch_1err = 0; ph->bch_2err = 0; ph->bch_uncorr = 0; }
  }
  for (unsigned int i = 0; i < PHASE_WORDS; i++) {
    int bch_rc=bch3121_fix_errors(flex, &phaseptr[i], PhaseNo);

    if (bch_rc < 0) {
      verbprintf(3, "FLEX_NEXT: BCH error at word %u (phase %c), marking uncorrectable\n", i, PhaseNo);

      switch (PhaseNo) {
        case 'A': flex->Data.PhaseA.bch_err[i]=1; flex->Data.PhaseA.bch_uncorr++; break;
        case 'B': flex->Data.PhaseB.bch_err[i]=1; flex->Data.PhaseB.bch_uncorr++; break;
        case 'C': flex->Data.PhaseC.bch_err[i]=1; flex->Data.PhaseC.bch_uncorr++; break;
        case 'D': flex->Data.PhaseD.bch_err[i]=1; flex->Data.PhaseD.bch_uncorr++; break;
      }
      continue;
    }

    if (bch_rc == 0) { /* clean word */ }
    else if (bch_rc == 1) { /* 1-bit fix */ }
    else if (bch_rc == 2) { /* 2-bit fix */ }
    switch (PhaseNo) {
      case 'A': if (bch_rc==0) flex->Data.PhaseA.bch_0err++; else if (bch_rc==1) flex->Data.PhaseA.bch_1err++; else flex->Data.PhaseA.bch_2err++; break;
      case 'B': if (bch_rc==0) flex->Data.PhaseB.bch_0err++; else if (bch_rc==1) flex->Data.PhaseB.bch_1err++; else flex->Data.PhaseB.bch_2err++; break;
      case 'C': if (bch_rc==0) flex->Data.PhaseC.bch_0err++; else if (bch_rc==1) flex->Data.PhaseC.bch_1err++; else flex->Data.PhaseC.bch_2err++; break;
      case 'D': if (bch_rc==0) flex->Data.PhaseD.bch_0err++; else if (bch_rc==1) flex->Data.PhaseD.bch_1err++; else flex->Data.PhaseD.bch_2err++; break;
    }

    phaseptr[i]&=0x1FFFFFL;
  }

  int *bch_err = NULL;
  switch (PhaseNo) {
    case 'A': bch_err=flex->Data.PhaseA.bch_err; break;
    case 'B': bch_err=flex->Data.PhaseB.bch_err; break;
    case 'C': bch_err=flex->Data.PhaseC.bch_err; break;
    case 'D': bch_err=flex->Data.PhaseD.bch_err; break;
  }

  // Multiple transmission (Section 3.4.2):
  // When num_tx > 1, frame is split into subframes for retransmission.
  // We always decode the full 88-word frame regardless.
  int num_tx = (int)flex->FIW.num_tx;
  if (num_tx > 1) {
    int sf_size = (num_tx == 2) ? 44 : (num_tx == 3) ? 29 : (num_tx == 4) ? 22 : 88;
    verbprintf(0, "FLEX_NEXT: num_tx=%d sf_size=%d, decoding full frame (subframe retransmission ignored)\n", num_tx, sf_size);
  }

  // Block information word is the first data word
  flex->biw_sysmsg_a_type = -1;
  flex->Decode.phase = PhaseNo;  // store for parse functions to access
  flex->Decode.sec_subtype = NULL;
  flex->Decode.opr_category = NULL;
  uint32_t biw = phaseptr[0];

  // If BIW itself is uncorrectable, we cannot parse this phase
  if (bch_err[0]) {
    verbprintf(3, "FLEX_NEXT: BIW uncorrectable (phase %c), skipping\n", PhaseNo);
    return;
  }

  // Nothing to see here, please move along
  // A valid BIW can never be all-zeros or all-ones because the
  // 4-bit checksum prevents it. Use voffset==aoffset for idle detection.
  if (biw == 0 || (biw & 0x1FFFFFL) == 0x1FFFFFL) {
    verbprintf(3, "FLEX_NEXT: BIW is all-zeros or all-ones, likely idle fill (not a valid BIW)\n");
    return;
  }

  // Address start address is bits 9-8, plus one for offset (to account for biw)
  unsigned int aoffset = ((biw >> 8) & 0x3L) + 1;
  // Vector start index is bits 15-10
  unsigned int voffset = (biw >> 10) & 0x3fL;
  // Priority address count is bits 7-4
  unsigned int prio = (biw >> 4) & 0xFL;
  // Carry-on is bits 17-16
  unsigned int carry = (biw >> 16) & 0x3L;
  // Collapse cycle is bits 20-18
  unsigned int collapse = (biw >> 18) & 0x7L;

  if (voffset < aoffset) {
      verbprintf(3, "FLEX_NEXT: Invalid biw\n");
      return;
  }

  // Parse BIW2/3/4 (words 1 through aoffset-1) before idle check.
  // These carry date, time, SSID etc. and are present even on idle frames.
  // BIW type is identified by bits 6-4 of each extra BIW word.
  {
    unsigned int bw;
    for (bw = 1; bw < aoffset && bw < (unsigned)PHASE_WORDS; bw++) {
      unsigned int bword;
      unsigned int btype;

      if (bch_err[bw]) {
        verbprintf(3, "FLEX_NEXT: BIW[%u] uncorrectable, skipping\n", bw);
        continue;
      }

      bword = phaseptr[bw];
      btype = (bword >> 4) & 0x7;

      switch (btype) {
        case 0: { // SSID1 (type 000): Local ID and Coverage Zone
          unsigned int lid = (bword >> 7) & 0x1FF;
          unsigned int cov = (bword >> 16) & 0x1F;
          verbprintf(2, "FLEX_NEXT: BIW SSID1: LID=%u CZ=%u (phase %c)\n", lid, cov, PhaseNo);
          if (json_mode) {
            cJSON *json = cJSON_CreateObject();
            if (json) {
              time_t now = time(NULL);
              struct tm *gmt = gmtime(&now);
              char ts[64];
              snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
                       gmt->tm_year+1900, gmt->tm_mon+1, gmt->tm_mday,
                       gmt->tm_hour, gmt->tm_min, gmt->tm_sec);
              cJSON_AddStringToObject(json, "timestamp", ts);
              cJSON_AddNumberToObject(json, "baud", flex->Sync.baud);
              cJSON_AddNumberToObject(json, "level", flex->Sync.levels);
              char ph[2] = { PhaseNo, '\0' };
              cJSON_AddStringToObject(json, "phase", ph);
              cJSON_AddNumberToObject(json, "cycle", flex->FIW.cycleno);
              cJSON_AddNumberToObject(json, "frame", flex->FIW.frameno);
              cJSON_AddStringToObject(json, "msg_type", "biw_sysid");
              cJSON_AddNumberToObject(json, "biw_position", bw);
              cJSON_AddStringToObject(json, "type_tag", "BIW_SSID1");
              cJSON_AddNumberToObject(json, "lid", lid);
              cJSON_AddNumberToObject(json, "cov", cov);
              char *out = cJSON_PrintUnformatted(json);
              if (out) { fprintf(stdout, "%s\n", out); free(out); }
              cJSON_Delete(json);
            }
          }
          break;
        }
        case 1: { // Date (type 001): year, day, month
          unsigned int year_raw = (bword >> 7) & 0x1F;
          unsigned int day = (bword >> 12) & 0x1F;
          unsigned int mon = (bword >> 17) & 0xF;
          unsigned int year = year_raw + 1994;
          /* The FLEX year field is 5 bits (0-31) with a base of 1994,
           * giving a native range of 1994-2025.  Starting in 2026 the
           * field wraps back to 0.  Correct by comparing against the
           * current system year and advancing by one 32-year epoch
           * when the decoded year is more than 16 years behind. */
          {
            time_t now = time(NULL);
            struct tm *gmt = gmtime(&now);
            int cur_year = gmt->tm_year + 1900;
            while ((int)year + 16 < cur_year)
              year += 32;
          }
          verbprintf(2, "FLEX_NEXT: BIW DATE: %04u-%02u-%02u (phase %c)\n", year, mon, day, PhaseNo);
          // Validate fields - month 0 or day 0 indicates corrupt data
          if (mon >= 1 && mon <= 12 && day >= 1 && day <= 31) {
            unsigned int abs_frame = flex->FIW.cycleno * 128 + flex->FIW.frameno;
            if (flextime_vote_date(&flex->ota_time, year, mon, day, abs_frame))
              flextime_emit(flex, PhaseNo);
          } else {
            verbprintf(3, "FLEX_NEXT: BIW DATE out of range: year=%u mon=%u day=%u, discarding\n", year, mon, day);
            break;
          }
          if (json_mode) {
            cJSON *json = cJSON_CreateObject();
            if (json) {
              time_t now = time(NULL);
              struct tm *gmt = gmtime(&now);
              char ts[64];
              snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
                       gmt->tm_year+1900, gmt->tm_mon+1, gmt->tm_mday,
                       gmt->tm_hour, gmt->tm_min, gmt->tm_sec);
              cJSON_AddStringToObject(json, "timestamp", ts);
              cJSON_AddNumberToObject(json, "baud", flex->Sync.baud);
              cJSON_AddNumberToObject(json, "level", flex->Sync.levels);
              char ph[2] = { PhaseNo, '\0' };
              cJSON_AddStringToObject(json, "phase", ph);
              cJSON_AddNumberToObject(json, "cycle", flex->FIW.cycleno);
              cJSON_AddNumberToObject(json, "frame", flex->FIW.frameno);
              cJSON_AddStringToObject(json, "msg_type", "biw_date");
              cJSON_AddNumberToObject(json, "biw_position", bw);
              cJSON_AddStringToObject(json, "type_tag", "BIW_DATE");
              cJSON_AddNumberToObject(json, "year", year);
              cJSON_AddNumberToObject(json, "month", mon);
              cJSON_AddNumberToObject(json, "day", day);
              char *out = cJSON_PrintUnformatted(json);
              if (out) { fprintf(stdout, "%s\n", out); free(out); }
              cJSON_Delete(json);
            }
          }
          break;
        }
        case 2: { // Time (type 010): hour, minute, second
          unsigned int hour = (bword >> 7) & 0x1F;
          unsigned int min = (bword >> 12) & 0x3F;
          unsigned int sec = (bword >> 18) & 0x7;
          verbprintf(2, "FLEX_NEXT: BIW TIME: %02u:%02u:%04.1f (phase %c)\n", hour, min, sec * 7.5, PhaseNo);
          // Validate range, then submit to voting with FIW cross-validation
          if (hour <= 23 && min <= 59 && sec <= 7) {
            unsigned int abs_frame = flex->FIW.cycleno * 128 + flex->FIW.frameno;
            if (flextime_vote_time(&flex->ota_time, hour, min, sec,
                               flex->FIW.cycleno, flex->FIW.frameno, abs_frame))
              flextime_emit(flex, PhaseNo);
          } else {
            verbprintf(3, "FLEX_NEXT: BIW TIME out of range: hour=%u min=%u sec=%u, discarding\n", hour, min, sec);
            break;
          }
          if (json_mode) {
            cJSON *json = cJSON_CreateObject();
            if (json) {
              time_t now = time(NULL);
              struct tm *gmt = gmtime(&now);
              char ts[64];
              snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
                       gmt->tm_year+1900, gmt->tm_mon+1, gmt->tm_mday,
                       gmt->tm_hour, gmt->tm_min, gmt->tm_sec);
              cJSON_AddStringToObject(json, "timestamp", ts);
              cJSON_AddNumberToObject(json, "baud", flex->Sync.baud);
              cJSON_AddNumberToObject(json, "level", flex->Sync.levels);
              char ph[2] = { PhaseNo, '\0' };
              cJSON_AddStringToObject(json, "phase", ph);
              cJSON_AddNumberToObject(json, "cycle", flex->FIW.cycleno);
              cJSON_AddNumberToObject(json, "frame", flex->FIW.frameno);
              cJSON_AddStringToObject(json, "msg_type", "biw_time");
              cJSON_AddNumberToObject(json, "biw_position", bw);
              cJSON_AddStringToObject(json, "type_tag", "BIW_TIME");
              cJSON_AddNumberToObject(json, "hour", hour);
              cJSON_AddNumberToObject(json, "min", min);
              cJSON_AddNumberToObject(json, "sec", sec * 7.5);
              char *out = cJSON_PrintUnformatted(json);
              if (out) { fprintf(stdout, "%s\n", out); free(out); }
              cJSON_Delete(json);
            }
          }
          break;
        }
        case 5: { // SysInfo (type 101): timezone, system messages
          unsigned int a_type = (bword >> 7) & 0xF;
          unsigned int info = (bword >> 11) & 0x3FF;
          // Store A-type for system message vector decode (Section 3.9.2)
          if (a_type <= 3) {
            // A=0000-0011: system message (all/home/roaming/SSID pagers)
            flex->biw_sysmsg_a_type = (int)a_type;
            verbprintf(3, "FLEX_NEXT: BIW SYSINFO: SysMsg A=%u I=0x%03X (phase %c)\n", a_type, info, PhaseNo);
          } else if (a_type == 4 || a_type == 8) {
            // Time-related: timezone, DST flag, and extended seconds.
            // I4-I0: Z4-Z0 timezone zone code (5 bits)
            // I5:    L0 DST flag (0=DST active, 1=standard time)
            // I7-I9: S5-S3 extended seconds (3 bits, combines with
            //        BIW TIME S2-S0 for 0.9375s resolution)
            unsigned int zone = info & 0x1F;
            unsigned int dst = (info >> 5) & 0x1;
            unsigned int esec = (info >> 7) & 0x7;
            int tz_min = (zone < 32) ? flex_tz_table[zone] : 0;
            verbprintf(2, "FLEX_NEXT: BIW SYSINFO: timezone zone=%u (%+dmin) DST=%u extsec=%u (phase %c)\n",
                       zone, tz_min, dst, esec, PhaseNo);
            // Submit to voting -- requires 3 identical readings
            {
              unsigned int abs_frame = flex->FIW.cycleno * 128 + flex->FIW.frameno;
              if (flextime_vote_tz(&flex->ota_time, zone, (int)dst, esec, tz_min,
                              flex->FIW.cycleno, flex->FIW.frameno, abs_frame))
                flextime_emit(flex, PhaseNo);
            }
          } else {
            verbprintf(3, "FLEX_NEXT: BIW SYSINFO: A=%u I=0x%03X (phase %c)\n", a_type, info, PhaseNo);
          }
          if (json_mode) {
            cJSON *json = cJSON_CreateObject();
            if (json) {
              time_t now = time(NULL);
              struct tm *gmt = gmtime(&now);
              char ts[64];
              snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
                       gmt->tm_year+1900, gmt->tm_mon+1, gmt->tm_mday,
                       gmt->tm_hour, gmt->tm_min, gmt->tm_sec);
              cJSON_AddStringToObject(json, "timestamp", ts);
              cJSON_AddNumberToObject(json, "baud", flex->Sync.baud);
              cJSON_AddNumberToObject(json, "level", flex->Sync.levels);
              char ph[2] = { PhaseNo, '\0' };
              cJSON_AddStringToObject(json, "phase", ph);
              cJSON_AddNumberToObject(json, "cycle", flex->FIW.cycleno);
              cJSON_AddNumberToObject(json, "frame", flex->FIW.frameno);
              cJSON_AddStringToObject(json, "msg_type", "biw_sysinfo");
              cJSON_AddNumberToObject(json, "biw_position", bw);
              cJSON_AddStringToObject(json, "type_tag", "BIW_SYSINFO");
              cJSON_AddNumberToObject(json, "a_type", a_type);
              cJSON_AddNumberToObject(json, "info", info);
              char *out = cJSON_PrintUnformatted(json);
              if (out) { fprintf(stdout, "%s\n", out); free(out); }
              cJSON_Delete(json);
            }
          }
          break;
        }
        case 7: { // SSID2 (type 111): Country Code and TMF
          unsigned int tmf = (bword >> 7) & 0xF;
          unsigned int country = (bword >> 11) & 0x3FF;
          verbprintf(2, "FLEX_NEXT: BIW SSID2: CC=%u TMF=0x%X (phase %c)\n", country, tmf, PhaseNo);
          if (json_mode) {
            cJSON *json = cJSON_CreateObject();
            if (json) {
              time_t now = time(NULL);
              struct tm *gmt = gmtime(&now);
              char ts[64];
              snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
                       gmt->tm_year+1900, gmt->tm_mon+1, gmt->tm_mday,
                       gmt->tm_hour, gmt->tm_min, gmt->tm_sec);
              cJSON_AddStringToObject(json, "timestamp", ts);
              cJSON_AddNumberToObject(json, "baud", flex->Sync.baud);
              cJSON_AddNumberToObject(json, "level", flex->Sync.levels);
              char ph[2] = { PhaseNo, '\0' };
              cJSON_AddStringToObject(json, "phase", ph);
              cJSON_AddNumberToObject(json, "cycle", flex->FIW.cycleno);
              cJSON_AddNumberToObject(json, "frame", flex->FIW.frameno);
              cJSON_AddStringToObject(json, "msg_type", "biw_countrycode");
              cJSON_AddNumberToObject(json, "biw_position", bw);
              cJSON_AddStringToObject(json, "type_tag", "BIW_SSID2");
              cJSON_AddNumberToObject(json, "country", country);
              cJSON_AddNumberToObject(json, "tmf", tmf);
              char *out = cJSON_PrintUnformatted(json);
              if (out) { fprintf(stdout, "%s\n", out); free(out); }
              cJSON_Delete(json);
            }
          }
          break;
        }
        default:
          verbprintf(3, "FLEX_NEXT: BIW[%u] reserved type=%u data=0x%05X\n", bw, btype, bword);
          break;
      }
    }
  }

  // No addresses if voffset == aoffset (idle frame with BIW only)
  if (voffset == aoffset) {
    verbprintf(3, "FLEX_NEXT: Idle frame, no addresses (voffset==aoffset=%u, collapse=%u)\n", voffset, collapse);
    return;
  }

  // long addresses use double AW and VW, so there are anywhere between ceil(v-a/2) to v-a pages in this frame
  verbprintf(3, "FLEX_NEXT: BlockInfoWord: (Phase %c) BIW:%08X AW %02u VW %02u prio=%u carry=%u collapse=%u (up to %u pages)\n", PhaseNo, biw, aoffset, voffset, prio, carry, collapse, voffset-aoffset);

  int flex_groupmessage = 0;
  int flex_groupbit = 0;

  // Tone-only detection (Section 3.8.1(6)):
  //
  // Tone-only addresses sit at the END of the address field with NO
  // corresponding vector word.  They are signaled by the transmitter
  // by placing more addresses than vectors in the frame.
  //
  // Previous approach used 4-bit nibble checksum on vector words to
  // find where "real" vectors end and tone-only addresses begin.
  // This was too aggressive -- the checksum can fail on valid vectors
  // after BCH correction or I&D merge substitution, causing false
  // tone-only classification and lost messages.
  //
  // Correct tone-only detection criteria (all must be true):
  //   1. The would-be vector word is idle fill (all-zeros 0x000000
  //      or all-ones 0x1FFFFF after BCH), with BCH error count <= 1
  //   2. ALL subsequent addresses (from this one to voffset-1) also
  //      have idle-fill vector words meeting criterion 1
  //   3. The address word itself passes BCH with error count <= 1
  //
  // This ensures we only classify as tone-only when we are certain
  // the vector slot contains no real data.  Currently disabled --
  // all vector slots are treated as valid (matches FLEX behavior).
  // Tone-only pages are extremely rare on real networks (e.g. P2000).
  int n_valid_vecs;
  {
    int max_vec = (int)(voffset - aoffset);
    if (max_vec > (int)((unsigned)PHASE_WORDS - voffset))
      max_vec = (int)((unsigned)PHASE_WORDS - voffset);
    n_valid_vecs = max_vec;
  }

  // Track how many vector slots have been consumed.
  unsigned int vec_used = 0;

  // Iterate through pages and dispatch to appropriate handler.
  for (unsigned int i = aoffset; i < voffset; i++) {
    // Skip address words with BCH errors
    if (bch_err[i]) {
      verbprintf(3, "FLEX_NEXT: Address word %u uncorrectable, skipping page\n", i);
      continue;
    }
    verbprintf(3, "FLEX_NEXT: Processing page offset #%u AW:%08X (vec_used=%u)\n", i - aoffset + 1, phaseptr[i], vec_used);
    if (phaseptr[i] == 0 ||
        (phaseptr[i] & 0x1FFFFFL) == 0x1FFFFFL) {
      verbprintf(3, "FLEX_NEXT: Idle codewords, invalid address\n");
      continue;
    }
    /*********************
     * Parse AW
     *
     * Address word classification per ARIB STD-43A Table 3.8.1-1.
     * All values are raw 21-bit address words (NOT capcodes).
     * For short/special: capcode = aw - 0x8000.
     *
     *   Short address (SA):   0x008001 - 0x1E0000  (capcode 1-1,933,312)
     *   Long Address 1 (LA1): 0x000001 - 0x008000
     *   Long Address 2 (LA2): 0x1F7FFF - 0x1FFFFE
     *   Long Address 3 (LA3): 0x1E0001 - 0x1E8000
     *   Long Address 4 (LA4): 0x1E8001 - 0x1F0000
     *   Reserved Short 1:     0x1F0001 - 0x1F27FF  (capcode 1,998,849-2,009,087)
     *   Info Service:         0x1F2800 - 0x1F67FF  (capcode 2,009,088-2,025,471)
     *   Network (NID):        0x1F6800 - 0x1F77FF  (capcode 2,025,472-2,029,567)
     *   Temporary Address:    0x1F7800 - 0x1F780F  (capcode 2,029,568-2,029,583)
     *   Operator Msg Address: 0x1F7810 - 0x1F781F  (capcode 2,029,584-2,029,599)
     *   Reserved Short 2:     0x1F7820 - 0x1F7FFE  (capcode 2,029,600-2,031,614)
     *
     * Long addresses use 2 consecutive address words.
     * The first word determines the type (LA1 or LA2).
     * The second word determines the set (LA2, LA3, or LA4).
     */
    uint32_t aiw = phaseptr[i];

    // Classify address word type (all values are raw 21-bit address words):
    // LA1: 0x000001 - 0x008000 (first word of long address sets 1-2, 1-3, 1-4)
    // SA:  0x008001 - 0x1E0000 (short address, capcode = aw - 0x8000, range 1-1,933,312)
    // LA3: 0x1E0001 - 0x1E8000 (second word only, never first)
    // LA4: 0x1E8001 - 0x1F0000 (second word only, never first)
    // Special addresses (capcode = aw - 0x8000):
    //   Rsvd Short 1: 0x1F0001 - 0x1F27FF (capcode 1,998,849-2,009,087)
    //   Info Service: 0x1F2800 - 0x1F67FF (capcode 2,009,088-2,025,471)
    //   Network:      0x1F6800 - 0x1F77FF (capcode 2,025,472-2,029,567)
    //   Temporary:    0x1F7800 - 0x1F780F (capcode 2,029,568-2,029,583)
    //   Operator:     0x1F7810 - 0x1F781F (capcode 2,029,584-2,029,599)
    //   Rsvd Short 2: 0x1F7820 - 0x1F7FFE (capcode 2,029,600-2,031,614)
    // LA2: 0x1F7FFF - 0x1FFFFE (first word of long address sets 2-3, 2-4)
    flex->Decode.long_address = (aiw >= 0x000001L && aiw <= 0x008000L) ||  // LA1
                                (aiw >= 0x1F7FFFL && aiw <= 0x1FFFFEL);    // LA2

    flex->Decode.addr_type = addr_type_char(aiw, flex->Decode.long_address);
    flex->Decode.capcode = aiw - 0x8000L;  // short address default
    if (flex->Decode.long_address) {
      // Long address decoding using all address sets per ARIB STD-43A Table 3.8.2.2-1.
      // Two address words: w1 = phaseptr[i], w2 = phaseptr[i+1]
      // The set is determined by which range w1 and w2 fall into.
      uint32_t w1 = aiw;
      uint32_t w2;
      // Bounds check: second address word must be within the frame
      if (i + 1 >= voffset) {
        verbprintf(3, "FLEX_NEXT: Long address at end of AF, no room for word 2\n");
        continue;
      }
      // Check for BCH error on second address word
      if (bch_err[i + 1]) {
        verbprintf(3, "FLEX_NEXT: Long address word 2 uncorrectable, skipping\n");
        i++;
        continue;
      }
      w2 = phaseptr[i + 1];

      flex->Decode.capcode = 0;

      // Set 1-2: w1 in LA1 (1-32768), w2 in LA2 (2064383-2097150)
      if (w1 >= 1 && w1 <= 32768 &&
          w2 >= 2064383L && w2 <= 2097150L) {
        flex->Decode.capcode = (int64_t)w1
          + (int64_t)(2097151L - w2) * 32768LL
          + 2068480LL;
      }
      // Set 1-3 / 1-4: w1 in LA1 (1-32768), w2 in LA3/LA4 (1966081-2031616)
      else if (w1 >= 1 && w1 <= 32768 &&
               w2 >= 1966081L && w2 <= 2031616L) {
        flex->Decode.capcode = (int64_t)w1
          + (int64_t)(w2 - 1933312L) * 32768LL
          + 2068480LL;
      }
      // Set 2-3 / 2-4: w1 in LA2 (2064383-2097150), w2 in LA3/LA4 (1966081-2031616)
      else if (w1 >= 2064383L && w1 <= 2097150L &&
               w2 >= 1966081L && w2 <= 2031616L) {
        flex->Decode.capcode = (int64_t)(w1 - 2064383L)
          + (int64_t)(w2 - 1867776L) * 32768LL
          + 2068479LL;
      }
      else {
        verbprintf(3, "FLEX_NEXT: Unknown long address set w1=0x%05X w2=0x%05X\n", w1, w2);
        i++;
        continue;
      }
    }
    if (flex->Decode.capcode > 4297068542LL || flex->Decode.capcode < 0) {
      // Invalid address (by spec, maximum address)
      verbprintf(3, "FLEX_NEXT: Invalid address, capcode out of range %" PRId64 "\n", flex->Decode.capcode);
      continue;
    }
    verbprintf(3, "FLEX_NEXT: CAPCODE:%016" PRIx64 " %" PRId64 "\n", flex->Decode.capcode, flex->Decode.capcode);

    flex_groupmessage = 0;
    flex_groupbit = 0;
    flex->Decode.sec_subtype = NULL;   // reset per page to prevent leaking between messages
    flex->Decode.opr_category = NULL;
    // Priority: first 'prio' address words in the AF are priority (Section 3.7, 3.8.1(6))
    flex->Decode.is_priority = ((i - aoffset) < prio) ? 1 : 0;
          // Temporary Address range per Section 3.8.2.3:
          // aw = 0x1F7800 - 0x1F780F (16 group delivery slots)
          // capcode = aw - 0x8000 = 2029568 - 2029583
          if ((flex->Decode.capcode >= FLEX_GROUP_ADDR_MIN) && (flex->Decode.capcode <= FLEX_GROUP_ADDR_MAX)) {
             flex_groupmessage = 1;
             flex_groupbit = flex->Decode.capcode - FLEX_GROUP_ADDR_MIN;
             if(flex_groupbit < 0) continue;
           }
    if (flex_groupmessage && flex->Decode.long_address) {
      // Invalid (by spec)
      verbprintf(3, "FLEX_NEXT: Don't process group messages if a long address\n");
      return;
    }
    verbprintf(3, "FLEX_NEXT: AIW %u: capcode:%" PRId64 " long:%d group:%d groupbit:%d\n", i, flex->Decode.capcode, flex->Decode.long_address, flex_groupmessage, flex_groupbit);

    /*********************
     * Parse VW
     *
     * Vector word layout (21 data bits) per ARIB STD-43A Section 3.9:
     *   bits 0-3:   checksum
     *   bits 4-6:   V (message type)
     *   bits 7-13:  b (message start word / data field, depends on type)
     *   bits 14-20: n (message word count / data field, depends on type)
     *
     * For alpha/hex/secure (V=000,101,110):
     *   b = first message word index, n = message word count
     *
     * Note on tone-only addresses (Section 3.8.1(6)):
     *   Tone-only addresses sit at the end of the address field with
     *   NO corresponding vector word. They are always short addresses.
     *   We detect them by checking the 4-bit checksum (Section 3.5.1)
     *   on the would-be vector word. Valid vector words always have a
     *   passing checksum. If it fails, this address is tone-only.
     *
     * For long addresses: the 2nd vector word (Vy) holds the first
     * message body word per Section 3.9.1: "the 1st word of the
     * message is placed at the 2nd word of the vector."
     * So the header word is at Vy (j+1), and n is decremented by 1
     * because one body word is already in the vector field.
     *
     * For short addresses: the header word is the first message word
     * (at mw1), and mw1 is incremented past it for content parsing.
     * n is decremented by 1 to account for the header word being
     * part of the message word count.
     */
    // Parse vector information word for address @ offset 'i'
    unsigned int j = voffset + vec_used;   // Vector slot for this address

    // Tone-only detection (Section 3.8.1(6)):
    // Tone-only addresses sit at the end of the address field with no
    // corresponding vector word. They have no vector, so they cannot
    // be long addresses (which require two vector words).
    // The pre-scan counted n_valid_vecs: the number of vector slots that
    // pass the 4-bit checksum.  Once vec_used reaches that count, all
    // remaining addresses are tone-only - no vector consumed.
    if ((int)vec_used >= n_valid_vecs || j >= (unsigned)PHASE_WORDS) {
      if (flex->Decode.long_address) {
        // Long addresses cannot be tone-only - skip as invalid
        verbprintf(3, "FLEX_NEXT: Long address past valid vectors, skipping cap %" PRId64 "\n", flex->Decode.capcode);
        i++;
        continue;
      }
      // Tone-only: address with no vector, output at debug level
      if (flex->Decode.capcode != 1) {  // skip idle artifact capcode 1
        if (!json_mode)
          verbprintf(3, "FLEX_NEXT|%i/%i|%02i.%03i.%c|%010" PRId64 "|%c|TON|\n",
                   flex->Sync.baud, flex->Sync.levels,
                   flex->FIW.cycleno, flex->FIW.frameno, PhaseNo,
                   flex->Decode.capcode,
                   addr_type_char(phaseptr[i], flex->Decode.long_address));
        if (json_mode)
          flex_next_json_emit(flex, PhaseNo, flex->Decode.capcode, flex->Decode.addr_type,
                              0, FLEX_PAGETYPE_TONE_ONLY, "TON", "complete",
                              -1, -1, -1, -1, -1, NULL, NULL, 0, NULL);
      }
      continue;
    }

    // Skip if vector word has BCH error
    if (bch_err[j]) {
      verbprintf(3, "FLEX_NEXT: Vector word %u uncorrectable, skipping page\n", j);
      vec_used += flex->Decode.long_address ? 2 : 1;
      if (flex->Decode.long_address) i++;
      continue;
    }
    uint32_t viw = phaseptr[j];
    flex->Decode.type = ((viw >> 4) & 0x7L);

    // Short Instruction (V=001) must be handled BEFORE hdr/len
    // calculation because instruction vectors use bits 7-20 for
    // instruction data, not message word pointers.  Extracting
    // mw1/len from an instruction vector gives garbage values
    // that can cause "Invalid VIW" false positives.
    if (flex->Decode.type == FLEX_PAGETYPE_SHORT_INSTRUCTION)
                {
                    // Short Instruction Vector (Section 3.9.6):
                    // The 14-bit instruction data is at bits 7-20 of the vector word.
                    // Within the 14-bit field:
                    //   bits 0-2 (vec bits 7-9):   i2i1i0 instruction type
                    //     000 = Temporary Address activation (group setup)
                    //     001 = System Event Notification
                    //     010-111 = reserved
                    //   bits 3-9 (vec bits 10-16):  f6-f0 target frame number
                    //   bits 10-13 (vec bits 17-20): a3-a0 temp address slot (0-15)
                    unsigned int instr_type = (viw >> 7) & 0x07;       // 3-bit instruction type
                    unsigned int iAssignedFrame = (viw >> 10) & 0x7f;  // 7-bit frame number
                    int groupbit = (viw >> 17) & 0x0f;                 // 4-bit slot index

                    // Output the instruction for visibility
                    if (!json_mode) {
                      char flex_ts[32];
                      flex_local_timestamp(flex_ts, sizeof(flex_ts));
                      if (instr_type == 0) {
                        // Temporary Address activation (group setup)
                        verbprintf(0, "FLEX_NEXT|%s|%i/%i|%02i.%03i.%c|%010" PRId64 "|%c|INS|K.GRP|slot=%d deliver_frame=%u\n",
                                 flex_ts,
                                 flex->Sync.baud, flex->Sync.levels,
                                 flex->FIW.cycleno, flex->FIW.frameno, PhaseNo,
                                 flex->Decode.capcode,
                                 addr_type_char(phaseptr[i], flex->Decode.long_address),
                                 groupbit, iAssignedFrame);
                      } else if (instr_type == 1) {
                        // System Event Notification - decode 11-bit event flags
                        unsigned int evt_data = (viw >> 9) & 0x7FF;
                        char evt_msg[256];
                        int epos = 0;
                        static const char *evt_names[] = {
                          "traffic_split_ssid", "traffic_split_nid",
                          "chan_setup_change", "new_freq_nid", "new_freq_ssid"
                        };
                        int ei;
                        for (ei = 0; ei < 5; ei++) {
                          if (evt_data & (1u << ei)) {
                            if (epos > 0) evt_msg[epos++] = ' ';
                            epos += snprintf(evt_msg + epos, sizeof(evt_msg) - epos, "%s", evt_names[ei]);
                          }
                        }
                        if (epos == 0) epos += snprintf(evt_msg, sizeof(evt_msg), "none");
                        verbprintf(0, "FLEX_NEXT|%s|%i/%i|%02i.%03i.%c|%010" PRId64 "|%c|INS|K.EVT|%s\n",
                                 flex_ts,
                                 flex->Sync.baud, flex->Sync.levels,
                                 flex->FIW.cycleno, flex->FIW.frameno, PhaseNo,
                                 flex->Decode.capcode,
                                 addr_type_char(phaseptr[i], flex->Decode.long_address),
                                 evt_msg);
                      } else {
                        // Reserved instruction type - raw data
                        unsigned int raw_data = (viw >> 9) & 0x7FF;
                        verbprintf(0, "FLEX_NEXT|%s|%i/%i|%02i.%03i.%c|%010" PRId64 "|%c|INS|K.RSV|type=%u data=0x%03X\n",
                                 flex_ts,
                                 flex->Sync.baud, flex->Sync.levels,
                                 flex->FIW.cycleno, flex->FIW.frameno, PhaseNo,
                                 flex->Decode.capcode,
                                 addr_type_char(phaseptr[i], flex->Decode.long_address),
                                 instr_type, raw_data);
                      }
                    } else {
                      cJSON *json = cJSON_CreateObject();
                      if (json) {
                        time_t now = time(NULL);
                        struct tm *gmt = gmtime(&now);
                        char ts[64];
                        snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
                                 gmt->tm_year+1900, gmt->tm_mon+1, gmt->tm_mday,
                                 gmt->tm_hour, gmt->tm_min, gmt->tm_sec);
                        cJSON_AddStringToObject(json, "timestamp", ts);
                        cJSON_AddNumberToObject(json, "baud", flex->Sync.baud);
                        cJSON_AddNumberToObject(json, "level", flex->Sync.levels);
                        char ph[2] = { PhaseNo, '\0' };
                        cJSON_AddStringToObject(json, "phase", ph);
                        cJSON_AddNumberToObject(json, "cycle", flex->FIW.cycleno);
                        cJSON_AddNumberToObject(json, "frame", flex->FIW.frameno);
                        cJSON_AddNumberToObject(json, "capcode", (double)flex->Decode.capcode);
                        cJSON_AddStringToObject(json, "msg_type", "instruction");
                        cJSON_AddStringToObject(json, "type_tag", "INS");
                        {
                          const char *itype_name = "reserved";
                          if (instr_type == 0) itype_name = "temp_addr";
                          else if (instr_type == 1) itype_name = "sys_event";
                          cJSON_AddStringToObject(json, "instruction_type", itype_name);
                        }
                        cJSON_AddNumberToObject(json, "target_frame", iAssignedFrame);
                        cJSON_AddNumberToObject(json, "group_slot", groupbit);
                        char *out = cJSON_PrintUnformatted(json);
                        if (out) { fprintf(stdout, "%s\n", out); free(out); }
                        cJSON_Delete(json);
                      }
                    }

                    // Only process temp address activation (i=000)
                    if (instr_type != 0) {
                      vec_used += flex->Decode.long_address ? 2 : 1;
                      if (flex->Decode.long_address) i++;
                      continue;
                    }

                    /* If this group slot is already active with a different
                     * delivery frame, or is mid-delivery, tear it down first. */
                    if (flex->GroupHandler.GroupFrame[groupbit] >= 0 &&
                        (flex->GroupHandler.GroupFrame[groupbit] != (int)iAssignedFrame ||
                         flex->GroupHandler.GroupDelivering[groupbit])) {
                      verbprintf(3, "FLEX_NEXT: Group slot %d reused (old frame=%d new frame=%u delivering=%d), tearing down\n",
                                 groupbit, flex->GroupHandler.GroupFrame[groupbit], iAssignedFrame,
                                 flex->GroupHandler.GroupDelivering[groupbit]);
                      flex->GroupHandler.GroupCodes[groupbit][CAPCODES_INDEX] = 0;
                      flex->GroupHandler.GroupFrame[groupbit] = -1;
                      flex->GroupHandler.GroupCycle[groupbit] = -1;
                      flex->GroupHandler.GroupDelivering[groupbit] = 0;
                      flex->GroupHandler.GroupTimeoutCount[groupbit] = 0;
                    }

                    if(groupbit < 0 || groupbit >= GROUP_BITS) {
                      verbprintf(3, "FLEX_NEXT: Invalid group bit: %i\n", groupbit);
                      vec_used += flex->Decode.long_address ? 2 : 1;
                      if (flex->Decode.long_address) i++;
                      continue;
                    }

                    if(flex->GroupHandler.GroupCodes[groupbit][CAPCODES_INDEX] >= GROUP_MAX_CODES) {
                      verbprintf(3, "FLEX_NEXT: Too many capcodes for group bit: %i\n", groupbit);
                      vec_used += flex->Decode.long_address ? 2 : 1;
                      if (flex->Decode.long_address) i++;
                      continue;
                    }

                    flex->GroupHandler.GroupCodes[groupbit][CAPCODES_INDEX]++;
                    int CapcodePlacement = flex->GroupHandler.GroupCodes[groupbit][CAPCODES_INDEX];
                    verbprintf(1, "FLEX_NEXT: Temp group setup: bit=%i capcodes=%i adding=[%010" PRId64 "] deliver_frame=%u\n", groupbit, CapcodePlacement, flex->Decode.capcode, iAssignedFrame);

                    flex->GroupHandler.GroupCodes[groupbit][CapcodePlacement] = flex->Decode.capcode;
                    flex->GroupHandler.GroupFrame[groupbit] = iAssignedFrame;

        if(iAssignedFrame > flex->FIW.frameno)
        {
      flex->GroupHandler.GroupCycle[groupbit] = (int)flex->FIW.cycleno;
      verbprintf(4, "FLEX_NEXT: Message frame is in this cycle: %i\n", flex->GroupHandler.GroupCycle[groupbit]);

        }
        else
        {
      if(flex->FIW.cycleno == 15)
                        {
        flex->GroupHandler.GroupCycle[groupbit] = 0;
      }
      else
      {
        flex->GroupHandler.GroupCycle[groupbit] = (int)flex->FIW.cycleno + 1;
          }
      verbprintf(4, "FLEX_NEXT: Message frame is in the next cycle: %i\n", flex->GroupHandler.GroupCycle[groupbit]);
        }


                    // Nothing else to do with this word.. move on!!
                    vec_used += flex->Decode.long_address ? 2 : 1;
                    if (flex->Decode.long_address) i++;
                    continue;
                }

    unsigned int mw1 = (viw >> 7) & 0x7FL;
    unsigned int len;
    unsigned int hdr;

    // Vector n field extraction depends on type (Section 3.9):
    //   Numeric types (V=011,100,111): bits 14-16 = 3-bit n, word_count = n+1
    //     bits 17-20 = K3-K0 checksum (NOT part of word count)
    //   Alpha/Hex/Secure (V=000,101,110): bits 14-20 = 7-bit word count
    //   Short Message (V=010): no word count (data in vector itself)
    //   Instruction (V=001): no word count (instruction data in vector)
    if (flex->Decode.type == FLEX_PAGETYPE_STANDARD_NUMERIC ||
        flex->Decode.type == FLEX_PAGETYPE_SPECIAL_NUMERIC ||
        flex->Decode.type == FLEX_PAGETYPE_NUMBERED_NUMERIC) {
      len = ((viw >> 14) & 0x07) + 1;  // 3-bit n field, word_count = n+1
    } else {
      len = (viw >> 14) & 0x7FL;       // 7-bit word count
    }
    if (flex->Decode.long_address) {
      // Long address: header is in the 2nd vector word (Vy)
      // per Section 3.9.1 / 3.9.3 / 3.9.4
      hdr = j + 1;
      if (len >= 1) {
        // 1st body word is in Vy, so message field has len-1 words
        len--;
      }
    } else {  // short address
      // Short address: header is the first message word
      hdr = mw1;
      mw1++;
      if (len >= 1) {
        // Header word is counted in n, so content is len-1 words.
        // This applies to ALL short address messages including group
        // messages - the header word layout is the same regardless
        // of whether the address is a temporary group delivery slot.
        len--;
      }
    }
    if (hdr >= (unsigned)PHASE_WORDS) {
      verbprintf(3, "FLEX_NEXT: Invalid VIW\n");
      continue;
    }
    // Check BCH status of header word
    if (bch_err[hdr]) {
      verbprintf(3, "FLEX_NEXT: Header word %u uncorrectable, skipping page\n", hdr);
      if (flex->Decode.long_address) i++;
      continue;
    }
    // Fragment flags from the message header word.
    // Bit layout is TYPE-DEPENDENT:
    //
    // Alpha/Secure (V=000,101): Section 3.10.1.3
    //   bits 0-9:   K (10-bit checksum)
    //   bit  10:    C (continuation)
    //   bits 11-12: F (fragment number)
    //   bits 13-18: N (message number)
    //   bit  19:    R (retrieval, initial only)
    //   bit  20:    M (maildrop, initial only)
    //
    // HEX/Binary (V=110): Section 3.10.1.2
    //   bits 0-11:  K (12-bit checksum)
    //   bit  12:    C (continuation)
    //   bits 13-14: F (fragment number)
    //   bits 15-20: N (message number)
    //   R and M are in hdr2 (second message word), not hdr1.
    //
    // Numeric (V=011,100,111): F/C from hdr1 same as alpha layout.
    //   N and R are in the first data word (numbered numeric only).
    //
    // Combined F/C interpretation:
    //   frag==3, cont==0: 'K' - complete message
    //   frag!=3, cont==1: 'F' - fragment, more coming
    //   frag!=3, cont==0: 'C' - continuation/last fragment
    int frag, cont, msg_n, msg_r, msg_m;

    if (flex->Decode.type == FLEX_PAGETYPE_BINARY) {
      // HEX/Binary: 12-bit K, C at bit 12, F at bits 13-14, N at bits 15-20
      frag  = (int) (phaseptr[hdr] >> 13) & 0x3;
      cont  = (int) (phaseptr[hdr] >> 12) & 0x1;
      msg_n = (int) (phaseptr[hdr] >> 15) & 0x3F;
      // R and M are in hdr2 (initial fragment only)
      int is_initial_hex = (frag == 0x03);
      if (is_initial_hex && mw1 < (unsigned)PHASE_WORDS && !bch_err[mw1]) {
        msg_r = (int) (phaseptr[mw1] >> 0) & 0x1;
        msg_m = (int) (phaseptr[mw1] >> 1) & 0x1;
      } else {
        msg_r = -1;
        msg_m = -1;
      }
    } else {
      // Alpha/Secure/Numeric: 10-bit K, C at bit 10, F at bits 11-12, N at bits 13-18
      frag  = (int) (phaseptr[hdr] >> 11) & 0x3;
      cont  = (int) (phaseptr[hdr] >> 10) & 0x1;
      msg_n = (int) (phaseptr[hdr] >> 13) & 0x3F;
      msg_r = (int) (phaseptr[hdr] >> 19) & 0x1;
      msg_m = (int) (phaseptr[hdr] >> 20) & 0x1;
    }
    int is_initial = (frag == 0x03);
    verbprintf(3, "FLEX_NEXT: VIW %u: type:%d mw1:%u len:%u frag:%d N:%d R:%d M:%d\n", j, flex->Decode.type, mw1, len, frag, msg_n, is_initial ? msg_r : -1, is_initial ? msg_m : -1);

    // mw1 == 0 is invalid (word 0 is always BIW), and must be within the frame.
    // The reference decoder only checks mw1 <= 87 - the vector b field can
    // legitimately point anywhere in the frame including the vector field
    // (e.g. long addresses where Vy holds body[0]).
    // For short message (type 2) and numeric types, len can be 0 (all data in vector).
    if (flex->Decode.type == FLEX_PAGETYPE_SHORT_MESSAGE)
      mw1 = len = 0;

    if (mw1 == 0 && len == 0 &&
        flex->Decode.type != FLEX_PAGETYPE_SHORT_MESSAGE &&
        flex->Decode.type != FLEX_PAGETYPE_STANDARD_NUMERIC &&
        flex->Decode.type != FLEX_PAGETYPE_SPECIAL_NUMERIC &&
        flex->Decode.type != FLEX_PAGETYPE_NUMBERED_NUMERIC) {
      verbprintf(3, "FLEX_NEXT: Invalid VIW\n");
      continue;
    }
    if (mw1 >= (unsigned)PHASE_WORDS) {
      verbprintf(3, "FLEX_NEXT: Invalid VIW\n");
      continue;
    }
    // mw1 + len == 89 was observed, but still contained valid page, so truncate
    if ((mw1 + len) > (unsigned)PHASE_WORDS){
      len = (unsigned)PHASE_WORDS - mw1;
    }

    // Log message body BCH errors (damaged words will show as '?' in output)
    {
      unsigned int body_errors = 0;
      unsigned int k;
      for (k = 0; k < len; k++) {
        if ((mw1 + k) < (unsigned)PHASE_WORDS && bch_err[mw1 + k])
          body_errors++;
      }
      if (body_errors > 0) {
        verbprintf(3, "FLEX_NEXT: %u/%u message body words uncorrectable for cap %" PRId64 "\n", body_errors, len, flex->Decode.capcode);
      }
    }

    // Word-level deduplication for complete (K) messages.
    // For fragmented messages (F/C), skip dedup - just reassemble.
    int is_complete = (frag == 3 && cont == 0);
    // 0=new, 1=exact duplicate (DUP), 2=improved retransmission (DUP+)
    int dedup_flag = 0;
    // Pointer/error arrays used for decode. Normally point at the
    // phase data, but may be redirected to merged dedup cache words.
    uint32_t *decode_words = phaseptr;
    int      *decode_errs  = bch_err;
    // Temporary arrays for decoding from dedup cache (merged words).
    // Sized to PHASE_WORDS so all existing index math works unchanged.
    uint32_t  merged_words[PHASE_WORDS];
    int       merged_errs[PHASE_WORDS];

    if (is_complete && len > 0) {
      // Pack message words: words[0]=hdr, words[1..len]=body at mw1
      // Store them contiguously for the dedup cache comparison.
      unsigned int n_msg_words = 1 + len;
      // Build a packed array for the dedup check. The cache stores
      // words packed as [hdr, body0, body1, ...] regardless of their
      // original positions in the frame.
      uint32_t packed_words[FLEX_DEDUP_MAX_WORDS];
      int      packed_errs[FLEX_DEDUP_MAX_WORDS];
      if (n_msg_words <= FLEX_DEDUP_MAX_WORDS) {
        packed_words[0] = phaseptr[hdr];
        packed_errs[0]  = bch_err[hdr];
        unsigned int pw;
        for (pw = 0; pw < len; pw++) {
          packed_words[1 + pw] = phaseptr[mw1 + pw];
          packed_errs[1 + pw]  = bch_err[mw1 + pw];
        }

        int dedup_slot = -1;
        int dedup_rc = dedup_check_words(flex, flex->Decode.capcode,
                                         flex->Decode.type, msg_n,
                                         packed_words, packed_errs,
                                         0, n_msg_words,
                                         0, 1, len, &dedup_slot);
        if (dedup_rc == 1) {
          // Exact duplicate - still output, tagged DUP
          dedup_flag = 1;
        }
        if (dedup_rc == 2 && dedup_slot >= 0) {
          // Improved retransmission - decode from merged cache words.
          dedup_flag = 2;
          memcpy(merged_words, phaseptr, (unsigned)PHASE_WORDS * sizeof(uint32_t));
          memcpy(merged_errs, bch_err, (unsigned)PHASE_WORDS * sizeof(int));
          struct Flex_DedupEntry *de = &flex->DedupStore.entries[dedup_slot];
          merged_words[hdr] = de->words[0];
          merged_errs[hdr]  = de->errs[0];
          for (pw = 0; pw < len; pw++) {
            merged_words[mw1 + pw] = de->words[1 + pw];
            merged_errs[mw1 + pw]  = de->errs[1 + pw];
          }
          decode_words = merged_words;
          decode_errs  = merged_errs;
          verbprintf(3, "FLEX_NEXT: Decoding from merged words for cap %" PRId64 "\n",
                     flex->Decode.capcode);
        }
        // dedup_rc == 0: new message, cached, decode normally
      }
    }

    if (!json_mode) {
      char flex_ts[32];
      flex_local_timestamp(flex_ts, sizeof(flex_ts));
      // Build group member capcodes for the capcode field (space-separated)
      // per FLEX output format: capcode member1 member2|...
      char cap_field[1024];
      int cap_pos = snprintf(cap_field, sizeof(cap_field), "%010" PRId64, flex->Decode.capcode);
      if (flex_groupmessage == 1) {
        int endpoint = flex_group_endpoint(&flex->GroupHandler, flex_groupbit);
        for (int g = 1; g <= endpoint; g++) {
          if (cap_pos >= (int)sizeof(cap_field) - 1)
            break;
          size_t cap_rem = sizeof(cap_field) - (size_t)cap_pos;
          int wrote = snprintf(cap_field + cap_pos, cap_rem, " %010" PRId64,
                               flex->GroupHandler.GroupCodes[flex_groupbit][g]);
          if (wrote < 0 || (size_t)wrote >= cap_rem) {
            cap_pos = (int)sizeof(cap_field) - 1;
            break;
          }
          cap_pos += wrote;
        }
      }
      // Build line prefix into flex->line_prefix for atomic output by parse functions
      snprintf(flex->line_prefix, sizeof(flex->line_prefix),
               "FLEX_NEXT|%s|%i/%i|%02i.%03i.%c|%s|%c",
               flex_ts,
               flex->Sync.baud, flex->Sync.levels,
               flex->FIW.cycleno, flex->FIW.frameno, PhaseNo,
               cap_field,
               flex->Decode.addr_type);
    }

    // Special address handling per ARIB STD-43A Section 3.8.2.
    // Network, Operator, Info Service, and Reserved addresses carry
    // system-level data that is distinct from normal pager messages.
    // We log them with their address type and fall through to normal
    // message decode for the body content.

    // Network Address (Section 3.8.2.1) with Secure (V=000) vector:
    // NID data is normally carried via Short Message Vector (t=00).
    // Secure messages on Network Addresses (e.g. NID Change Instruction,
    // Section 3.9.7) use formats not in the public standard (STD-43A).
    // No special handling -- falls through to normal SEC type dispatch.

    // Operator Message Address (Section 3.8.2.5): sub-type in LSB of
    // address word.  Categories: SysMsg (0-3), SSIDChange (0xE),
    // SysEvent (0xF).  Body decoded normally after logging category.
    if (flex->Decode.addr_type == 'O') {
      unsigned int lsb = aiw & 0x0F;
      const char *cat = "unknown";
      if (lsb <= 3)       cat = "SysMsg";
      else if (lsb == 0xE) cat = "SSIDChange";
      else if (lsb == 0xF) cat = "SysEvent";
      flex->Decode.opr_category = cat;
      if (!json_mode) {
        if (lsb <= 3)        flex->line_type_tag = "OPR/SysMsg";
        else if (lsb == 0xE) flex->line_type_tag = "OPR/SSIDChange";
        else if (lsb == 0xF) flex->line_type_tag = "OPR/SysEvent";
        else                 flex->line_type_tag = "OPR/unknown";
      }
      // Fall through to normal message decode for body content
    }

    // Info Service Address (Section 3.8.2, maildrop):
    // No special handling -- decoded by normal type dispatch below.
    // Address type 'I' distinguishes it from regular messages.

    // Reserved Short Address: log and skip (no defined behavior).
    if (flex->Decode.addr_type == 'R') {
      verbprintf(3, "FLEX_NEXT: Reserved short address aw=0x%05X cap=%" PRId64 ", skipping\n",
                 aiw, flex->Decode.capcode);
      goto page_done;
    }

    // Message type dispatch per ARIB STD-43A Section 3.9:
    //   V=000 (0): Secure - encrypted/proprietary content
    //   V=001 (1): Short Instruction - handled above (group setup)
    //   V=010 (2): Short Message Vector
    //   V=011 (3): Standard Numeric - BCD digits
    //   V=100 (4): Special Numeric - BCD with extended header
    //   V=101 (5): Alphanumeric - 7-bit ASCII
    //   V=110 (6): Binary/Hex - raw data
    //   V=111 (7): Numbered Numeric - BCD with message number
    switch (flex->Decode.type) {
    case FLEX_PAGETYPE_ALPHANUMERIC:
      flex->line_type_tag = "ALN";
      parse_alphanumeric(flex, decode_words, decode_errs, hdr, mw1, len, frag, cont, msg_n, msg_r, msg_m, dedup_flag, flex_groupmessage, flex_groupbit);
      break;
    case FLEX_PAGETYPE_SECURE: {
      // Secure messages (Section 3.9.4, Fig 3.10.1.4-1):
      // Same header layout as alpha, but bits 19-20 = t1t0 sub-type:
      //   t=00: 7-bit alphanumeric (JIS X 0201) - decode as alpha
      //   t=10: binary data - decode as hex
      //   t=01: data defined separately
      //   t=11: reserved
      // Registration Acknowledgment: t=00 with opcode '=' (0x3D) in 2nd word.
      int sec_t = -1;
      if (hdr < (unsigned)PHASE_WORDS && !decode_errs[hdr]) {
        sec_t = (decode_words[hdr] >> 19) & 0x3;
      }
      if (sec_t == 0) {
        // t=00: alphanumeric content - decode like alpha
        flex->line_type_tag = "SEC:ALN";
        flex->Decode.sec_subtype = "alpha";
        parse_alphanumeric(flex, decode_words, decode_errs, hdr, mw1, len, frag, cont, msg_n, msg_r, msg_m, dedup_flag, flex_groupmessage, flex_groupbit);
      } else if (sec_t == 2) {
        // t=10: binary data
        flex->line_type_tag = "SEC:BIN";
        flex->Decode.sec_subtype = "binary";
        parse_binary(flex, decode_words, decode_errs, mw1, len, frag, cont, msg_n, msg_r, msg_m, dedup_flag);
      } else {
        // t=01, t=11, or unknown: dump as hex
        flex->line_type_tag = (sec_t == 1) ? "SEC:VEN" : "SEC:RSV";
        flex->Decode.sec_subtype = (sec_t == 1) ? "vendor" : "reserved";
        parse_binary(flex, decode_words, decode_errs, mw1, len, frag, cont, msg_n, msg_r, msg_m, dedup_flag);
      }
      break;
    }
    case FLEX_PAGETYPE_STANDARD_NUMERIC:
      flex->line_type_tag = "NUM";
      parse_numeric(flex, decode_words, decode_errs, j, frag, cont, msg_n, msg_r, msg_m, dedup_flag);
      break;
    case FLEX_PAGETYPE_SPECIAL_NUMERIC:
      flex->line_type_tag = "SNUM";
      parse_numeric(flex, decode_words, decode_errs, j, frag, cont, msg_n, msg_r, msg_m, dedup_flag);
      break;
    case FLEX_PAGETYPE_NUMBERED_NUMERIC:
      flex->line_type_tag = "NNUM";
      parse_numeric(flex, decode_words, decode_errs, j, frag, cont, msg_n, msg_r, msg_m, dedup_flag);
      break;
    case FLEX_PAGETYPE_SHORT_MESSAGE:
      flex->line_type_tag = "SMSG";
      parse_short_message(flex, decode_words, decode_errs, j);
      break;
    case FLEX_PAGETYPE_BINARY:
      flex->line_type_tag = "HEX";
      parse_binary(flex, decode_words, decode_errs, mw1, len, frag, cont, msg_n, msg_r, msg_m, dedup_flag);
      break;
    default:
      flex->line_type_tag = "UNK";
      parse_binary(flex, decode_words, decode_errs, mw1, len, frag, cont, msg_n, msg_r, msg_m, dedup_flag);
      break;
    }

page_done:
    // long addresses eat 2 aw and 2 vw, so skip the next aw-vw pair
    if (flex->Decode.long_address) {
      vec_used += 2;
      i++;
    } else {
      vec_used++;
    }
  }

  // BIW101 System Message Vector at end of VF (Section 3.9.2, method (a)/(b)).
  // When BIW101 A=0000-0011 is present, a system message vector sits at the
  // END of the vector field (after all normal address/vector pairs).  This
  // vector points to message body words in the MF, same format as a normal
  // alpha/secure vector.  Decode it as a synthetic operator message.
  if (flex->biw_sysmsg_a_type >= 0 && flex->biw_sysmsg_a_type <= 3) {
    int sv_idx = (int)voffset + n_valid_vecs - 1;
    if (sv_idx > (int)voffset + (int)vec_used - 1 &&
        sv_idx < PHASE_WORDS && !bch_err[sv_idx]) {
      uint32_t sv = phaseptr[sv_idx];
      int sv_type = (sv >> 4) & 0x7;
      int sv_mw1 = (sv >> 7) & 0x7F;
      int sv_len = (sv >> 14) & 0x7F;
      if (sv_len > 0 && sv_mw1 < PHASE_WORDS) {
        if (!json_mode) {
          char flex_ts[32];
          flex_local_timestamp(flex_ts, sizeof(flex_ts));
          snprintf(flex->line_prefix, sizeof(flex->line_prefix),
                   "FLEX_NEXT|%s|%i/%i|%02i.%03i.%c|SysMsg_A%d|",
                   flex_ts,
                   flex->Sync.baud, flex->Sync.levels,
                   flex->FIW.cycleno, flex->FIW.frameno, PhaseNo,
                   flex->biw_sysmsg_a_type);
        }
        if (sv_type == FLEX_PAGETYPE_ALPHANUMERIC && !bch_err[sv_mw1]) {
          int sv_frag = (phaseptr[sv_mw1] >> 11) & 0x3;
          int sv_cont = (phaseptr[sv_mw1] >> 10) & 0x1;
          int sv_msg_n = (phaseptr[sv_mw1] >> 13) & 0x3F;
          int sv_msg_r = (phaseptr[sv_mw1] >> 19) & 0x1;
          int sv_msg_m = (phaseptr[sv_mw1] >> 20) & 0x1;
          int sv_hdr = sv_mw1;
          sv_mw1++;
          if (sv_len > 0) sv_len--;
          flex->line_type_tag = "SYS:ALN";
          parse_alphanumeric(flex, phaseptr, bch_err, sv_hdr, sv_mw1, sv_len, sv_frag, sv_cont, sv_msg_n, sv_msg_r, sv_msg_m, 0, 0, 0);
        } else {
          int sv_frag = 3;
          int sv_cont = 0;
          flex->line_type_tag = "SYS:SEC";
          parse_binary(flex, phaseptr, bch_err, sv_mw1, sv_len, sv_frag, sv_cont, -1, -1, -1, 0);
        }
      }
    }
  }

  /* Per-phase BCH summary */
  {
    struct Flex_Phase *ph = NULL;
    switch (PhaseNo) {
      case 'A': ph = &flex->Data.PhaseA; break;
      case 'B': ph = &flex->Data.PhaseB; break;
      case 'C': ph = &flex->Data.PhaseC; break;
      case 'D': ph = &flex->Data.PhaseD; break;
    }
    if (ph) {
      int errbits = ph->bch_1err + ph->bch_2err * 2 + ph->bch_uncorr * 3;
      if (!json_mode) {
        verbprintf(1, "FLEX_NEXT|%i/%i|%02i.%03i.%c|BCH|%s|0:%d|1:%d|2:%d|U:%d|errbits:%d\n",
                   flex->Sync.baud, flex->Sync.levels,
                   flex->FIW.cycleno, flex->FIW.frameno, PhaseNo,
                   flex->Sync.polarity ? "NEG" : "POS",
                   ph->bch_0err, ph->bch_1err, ph->bch_2err, ph->bch_uncorr, errbits);
      } else {
        cJSON *json = cJSON_CreateObject();
        if (json) {
          cJSON_AddNumberToObject(json, "baud", flex->Sync.baud);
          cJSON_AddNumberToObject(json, "level", flex->Sync.levels);
          char phs[2] = { PhaseNo, '\0' };
          cJSON_AddStringToObject(json, "phase", phs);
          cJSON_AddNumberToObject(json, "cycle", flex->FIW.cycleno);
          cJSON_AddNumberToObject(json, "frame", flex->FIW.frameno);
          cJSON_AddStringToObject(json, "msg_type", "bch_stats");
          cJSON_AddStringToObject(json, "polarity", flex->Sync.polarity ? "NEG" : "POS");
          cJSON_AddNumberToObject(json, "bch_0err", ph->bch_0err);
          cJSON_AddNumberToObject(json, "bch_1err", ph->bch_1err);
          cJSON_AddNumberToObject(json, "bch_2err", ph->bch_2err);
          cJSON_AddNumberToObject(json, "bch_uncorr", ph->bch_uncorr);
          cJSON_AddNumberToObject(json, "errbits", errbits);
          char *out = cJSON_PrintUnformatted(json);
          if (out) { fprintf(stdout, "%s\n", out); free(out); }
          cJSON_Delete(json);
        }
      }
    }
  }
}


static void clear_phase_data(struct Flex_Next * flex) {
  if (flex==NULL) return;
  int i;
  for (i = 0; i < PHASE_WORDS; i++) {
    flex->Data.PhaseA.buf[i]=0;
    flex->Data.PhaseB.buf[i]=0;
    flex->Data.PhaseC.buf[i]=0;
    flex->Data.PhaseD.buf[i]=0;
    flex->Data.PhaseA.bch_err[i]=0;
    flex->Data.PhaseB.bch_err[i]=0;
    flex->Data.PhaseC.bch_err[i]=0;
    flex->Data.PhaseD.bch_err[i]=0;
    flex->Data.AltA.buf[i]=0;
    flex->Data.AltB.buf[i]=0;
    flex->Data.AltC.buf[i]=0;
    flex->Data.AltD.buf[i]=0;
    flex->Data.AltA.bch_err[i]=0;
    flex->Data.AltB.bch_err[i]=0;
    flex->Data.AltC.bch_err[i]=0;
    flex->Data.AltD.bch_err[i]=0;
  }

  flex->Data.PhaseA.idle_count=0;
  flex->Data.PhaseB.idle_count=0;
  flex->Data.PhaseC.idle_count=0;
  flex->Data.PhaseD.idle_count=0;
  flex->Data.AltA.idle_count=0;
  flex->Data.AltB.idle_count=0;
  flex->Data.AltC.idle_count=0;
  flex->Data.AltD.idle_count=0;

  flex->Data.phase_toggle=0;
  flex->Data.data_bit_counter=0;

}


static void decode_data(struct Flex_Next * flex) {
  if (flex==NULL) return;

  // Expire stale fragment reassembly slots
  {
    unsigned int abs_frame = flex->FIW.cycleno * 128 + flex->FIW.frameno;
    frag_expire(flex, abs_frame);
  }

  // I&D merge: substitute alt words into primary where alt BCH succeeds
  // but primary fails. Applied to all active phases for all modes.
  {
    struct { struct Flex_Phase *pri; struct Flex_Phase *alt; char name; } pairs[4];
    int npairs = 0;

    // Determine active phases based on mode
    if (flex->Sync.baud == 1600) {
      if (flex->Sync.levels == 2) {
        // A1: Phase A only
        pairs[0] = (typeof(pairs[0])){ &flex->Data.PhaseA, &flex->Data.AltA, 'A' };
        npairs = 1;
      } else {
        // A3: Phase A + Phase C
        pairs[0] = (typeof(pairs[0])){ &flex->Data.PhaseA, &flex->Data.AltA, 'A' };
        pairs[1] = (typeof(pairs[0])){ &flex->Data.PhaseC, &flex->Data.AltC, 'C' };
        npairs = 2;
      }
    } else {
      if (flex->Sync.levels == 2) {
        // A2: Phase A + Phase C
        pairs[0] = (typeof(pairs[0])){ &flex->Data.PhaseA, &flex->Data.AltA, 'A' };
        pairs[1] = (typeof(pairs[0])){ &flex->Data.PhaseC, &flex->Data.AltC, 'C' };
        npairs = 2;
      } else {
        // A4: Phase A + B + C + D
        pairs[0] = (typeof(pairs[0])){ &flex->Data.PhaseA, &flex->Data.AltA, 'A' };
        pairs[1] = (typeof(pairs[0])){ &flex->Data.PhaseB, &flex->Data.AltB, 'B' };
        pairs[2] = (typeof(pairs[0])){ &flex->Data.PhaseC, &flex->Data.AltC, 'C' };
        pairs[3] = (typeof(pairs[0])){ &flex->Data.PhaseD, &flex->Data.AltD, 'D' };
        npairs = 4;
      }
    }

    int improved = 0;
    for (int p = 0; p < npairs; p++) {
      /* Parse BIW from primary to find frame structure. */
      uint32_t biw_tmp = pairs[p].pri->buf[0];
      int biw_ok = (bch3121_fix_errors(flex, &biw_tmp, pairs[p].name) >= 0);
      unsigned int voffset = PHASE_WORDS;
      unsigned int aoffset_val = 1;
      if (biw_ok) {
        biw_tmp &= 0x1FFFFFL;
        voffset = (biw_tmp >> 10) & 0x3fL;
        aoffset_val = ((biw_tmp >> 8) & 0x3L) + 1;
        if (voffset == 0) voffset = PHASE_WORDS;
      }

      for (unsigned int w = 0; w < PHASE_WORDS; w++) {
        uint32_t alt_word = pairs[p].alt->buf[w];
        int alt_rc = bch3121_fix_errors(flex, &alt_word, pairs[p].name);
        uint32_t pri_word = pairs[p].pri->buf[w];
        int pri_rc = bch3121_fix_errors(flex, &pri_word, pairs[p].name);

        /* Safe regions (BIW + body): substitute if alt BCH succeeds */
        if (pri_rc < 0 && alt_rc >= 0 && (w < aoffset_val || w >= voffset)) {
          pairs[p].pri->buf[w] = pairs[p].alt->buf[w];
          improved++;
        }
        /* Address/vector words: substitute only with perfect BCH and
         * single-bit raw word distance (safest recovery). */
        else if (pri_rc < 0 && alt_rc == 0 && w >= aoffset_val && w < voffset) {
          int ndiff = __builtin_popcount(pairs[p].pri->buf[w] ^ pairs[p].alt->buf[w]);
          if (ndiff <= 1) {
            pairs[p].pri->buf[w] = pairs[p].alt->buf[w];
            improved++;
          }
        }
      }
    }
    if (improved)
      verbprintf(3, "FLEX_NEXT: I&D merge improved %d words\n", improved);
  }

  // Phase decode per ARIB STD-43A Section 3.3:
  //   A1 (1600/2FSK): Phase A only
  //   A3 (1600/4FSK): Phase A + Phase C
  //   A2 (3200/2FSK): Phase A + Phase C
  //   A4 (3200/4FSK): Phase A + Phase B + Phase C + Phase D
  if (flex->Sync.baud == 1600) {
    if (flex->Sync.levels==2) {
      decode_phase(flex, 'A');
    } else {
      decode_phase(flex, 'A');
      decode_phase(flex, 'C');
    }
  } else {
    if (flex->Sync.levels==2) {
      decode_phase(flex, 'A');
      decode_phase(flex, 'C');
    } else {
      decode_phase(flex, 'A');
      decode_phase(flex, 'B');
      decode_phase(flex, 'C');
      decode_phase(flex, 'D');
    }
  }
}


static int read_data(struct Flex_Next * flex, unsigned char sym) {
  if (flex==NULL) return -1;
  // Decode one symbol into phase data bits and store into the
  // correct phase buffer(s) per ARIB STD-43A Section 3.3.
  //
  // Mode summary (baud = symbol rate, levels = FSK levels):
  //   A1 (1600 baud, 2FSK):  1600bps, Phase A only
  //   A3 (1600 baud, 4FSK):  3200bps, Phase A (MSB) + Phase C (LSB)
  //   A2 (3200 baud, 2FSK):  3200bps, Phase A + Phase C (interleaved)
  //   A4 (3200 baud, 4FSK):  6400bps, Phase A/B + Phase C/D (interleaved)
  //
  // 4FSK Gray code (Section 3.3.2):
  //   Symbol  bit_a(MSB)  bit_b(LSB)
  //     0        1           1
  //     1        1           0
  //     2        0           0
  //     3        0           1
  //
  // At 1600 baud (A1/A3), every symbol goes to the same phase pair:
  //   A1 (2FSK): bit_a -> Phase A only
  //   A3 (4FSK): bit_a -> Phase A,  bit_b -> Phase C
  // At 3200 baud (A2/A4), symbols alternate between two phase pairs:
  //   A2 (2FSK): even sym bit_a -> Phase A, odd sym bit_a -> Phase C
  //   A4 (4FSK): even sym bit_a -> Phase A, bit_b -> Phase B
  //              odd sym  bit_a -> Phase C, bit_b -> Phase D
  //
  // Bitrates: A1=1600bps, A2=3200bps, A3=3200bps, A4=6400bps.

  int bit_a = (sym > 1);
  int bit_b = 0;
  if (flex->Sync.levels == 4) {
    bit_b = (sym == 1) || (sym == 2);
  }

  if (flex->Sync.baud == 1600) {
    flex->Data.phase_toggle=0;
  }

  // De-interleave index: bits 0-2 map straight through, bits 5+
  // select the word.  This undoes the block interleaving.
  unsigned int idx= ((flex->Data.data_bit_counter>>5)&0xFFF8) |  (flex->Data.data_bit_counter&0x0007);

  if (flex->Data.phase_toggle==0) {
    // At 1600 baud: every symbol.  At 3200 baud: even symbols.
    // bit_a -> Phase A always.
    // bit_b -> Phase C at 1600 baud (A3), Phase B at 3200 baud (A4).
    flex->Data.PhaseA.buf[idx] = (flex->Data.PhaseA.buf[idx]>>1) | (bit_a?(0x80000000):0);
    if (flex->Sync.baud == 1600) {
      flex->Data.PhaseC.buf[idx] = (flex->Data.PhaseC.buf[idx]>>1) | (bit_b?(0x80000000):0);
    } else {
      flex->Data.PhaseB.buf[idx] = (flex->Data.PhaseB.buf[idx]>>1) | (bit_b?(0x80000000):0);
    }
    flex->Data.phase_toggle=1;

    if ((flex->Data.data_bit_counter & 0xFF) == 0xFF) {
      if (flex->Data.PhaseA.buf[idx] == 0x00000000 || flex->Data.PhaseA.buf[idx] == 0xffffffff) flex->Data.PhaseA.idle_count++;
      if (flex->Sync.baud == 1600) {
        if (flex->Data.PhaseC.buf[idx] == 0x00000000 || flex->Data.PhaseC.buf[idx] == 0xffffffff) flex->Data.PhaseC.idle_count++;
      } else {
        if (flex->Data.PhaseB.buf[idx] == 0x00000000 || flex->Data.PhaseB.buf[idx] == 0xffffffff) flex->Data.PhaseB.idle_count++;
      }
    }
  } else {
    // 3200 baud only: odd symbols.
    // bit_a -> Phase C,  bit_b -> Phase D.
    flex->Data.PhaseC.buf[idx] = (flex->Data.PhaseC.buf[idx]>>1) | (bit_a?(0x80000000):0);
    flex->Data.PhaseD.buf[idx] = (flex->Data.PhaseD.buf[idx]>>1) | (bit_b?(0x80000000):0);
    flex->Data.phase_toggle=0;

    if ((flex->Data.data_bit_counter & 0xFF) == 0xFF) {
      if (flex->Data.PhaseC.buf[idx] == 0x00000000 || flex->Data.PhaseC.buf[idx] == 0xffffffff) flex->Data.PhaseC.idle_count++;
      if (flex->Data.PhaseD.buf[idx] == 0x00000000 || flex->Data.PhaseD.buf[idx] == 0xffffffff) flex->Data.PhaseD.idle_count++;
    }
  }

  if (flex->Sync.baud == 1600 || flex->Data.phase_toggle==0) {
    flex->Data.data_bit_counter++;
  }

  // Report if all active phases have gone idle
  int idle=0;
  if (flex->Sync.baud == 1600) {
    if (flex->Sync.levels==2) {
      idle=(flex->Data.PhaseA.idle_count>IDLE_THRESHOLD);
    } else {
      idle=((flex->Data.PhaseA.idle_count>IDLE_THRESHOLD) && (flex->Data.PhaseC.idle_count>IDLE_THRESHOLD));
    }
  } else {
    if (flex->Sync.levels==2) {
      idle=((flex->Data.PhaseA.idle_count>IDLE_THRESHOLD) && (flex->Data.PhaseC.idle_count>IDLE_THRESHOLD));
    } else {
      idle=((flex->Data.PhaseA.idle_count>IDLE_THRESHOLD) && (flex->Data.PhaseB.idle_count>IDLE_THRESHOLD) && (flex->Data.PhaseC.idle_count>IDLE_THRESHOLD) && (flex->Data.PhaseD.idle_count>IDLE_THRESHOLD));
    }
  }

  return idle;
}


static void report_state(struct Flex_Next * flex) {
  if (flex->State.Current != flex->State.Previous) {
    flex->State.Previous = flex->State.Current;

    char * state="Unknown";
    switch (flex->State.Current) {
      case FLEX_STATE_SYNC1:
        state="SYNC1";
        break;
      case FLEX_STATE_FIW:
        state="FIW";
        break;
      case FLEX_STATE_SYNC2:
        state="SYNC2";
        break;
      case FLEX_STATE_DATA:
        state="DATA";
        break;
      default:
        break;

    }
    verbprintf(1, "FLEX_NEXT: State: %s\n", state);
  }
}

//Called for each received symbol
static void flex_sym(struct Flex_Next * flex, unsigned char sym) {
  if (flex==NULL) return;
  /*If the signal has a negative polarity, the symbols must be inverted*/
  /*Polarity is determined during the IDLE/sync word checking phase*/
  unsigned char sym_rectified;
  if (flex->Sync.polarity) {
    sym_rectified=3-sym;
  } else {
    sym_rectified=sym;
  }

  switch (flex->State.Current) {
    case FLEX_STATE_SYNC1:
      {
        // Continually compare the received symbol stream
        // against the known FLEX sync words.
        unsigned int sync_code=flex_sync(flex, sym); //Unrectified version of the symbol must be used here
        if (sync_code!=0) {
          decode_mode(flex,sync_code);

          if (flex->Sync.baud!=0 && flex->Sync.levels!=0) {
            flex->State.Current=FLEX_STATE_FIW;

            verbprintf(2, "FLEX_NEXT: SyncInfoWord: sync_code=0x%04x baud=%i levels=%i polarity=%s zero=%f envelope=%f symrate=%f\n",
                sync_code, flex->Sync.baud, flex->Sync.levels, flex->Sync.polarity?"NEG":"POS", flex->Modulation.zero, flex->Modulation.envelope, flex->Modulation.symbol_rate);
          } else {
            verbprintf(2, "FLEX_NEXT: Unknown Sync code = 0x%04x\n", sync_code);
            flex->State.Current=FLEX_STATE_SYNC1;
          }
        } else {
          flex->State.Current=FLEX_STATE_SYNC1;
        }

        flex->State.fiwcount=0;
        flex->FIW.rawdata=0;
        break;
      }
    case FLEX_STATE_FIW:
      {
        // Skip 16 bits of dotting, then accumulate 32 bits
        // of Frame Information Word.
        // FIW is accumulated, call BCH to error correct it
        flex->State.fiwcount++;
        if (flex->State.fiwcount>=16) {
          read_2fsk(flex, sym_rectified, &flex->FIW.rawdata);
        }

        if (flex->State.fiwcount==48) {
          if (decode_fiw(flex)==0) {
            flex->State.sync2_count=0;
            flex->State.sync2_shiftreg=0;
            flex->State.sync2_c_found=0;
            flex->State.sync2_c_pos=0;
            flex->State.sync2_c_errs=0;
            flex->State.sync2_cinv_found=0;
            flex->State.sync2_cinv_pos=0;
            flex->State.sync2_cinv_errs=0;
            flex->State.sync2_sym_buf_count=0;
            flex->State.sync2_sym_buf_start=0;
            flex->Demodulator.baud = flex->Sync.baud;
            flex->State.Current=FLEX_STATE_SYNC2;
          } else {
            flex->State.Current=FLEX_STATE_SYNC1;
          }
        }
        break;
      }
    case FLEX_STATE_SYNC2:
      {
        // S2 structure per Section 3.2:
        //   BS2 + C(16 bits) + inv.BS2 + inv.C(16 bits) = 25ms total
        //   S2 is symmetric: first half = BS2 + C, second half = inv.BS2 + inv.C.
        //   Distance from C-end to inv.C-end is always s2_symbols/2.
        unsigned int s2_symbols = flex->Sync.baud*25/1000;
        unsigned int s2_half = s2_symbols / 2;
        int bit_a = (sym_rectified > 1);

        if (flex->Sync.levels == 4) {
          int bit_b = (sym_rectified == 1) || (sym_rectified == 2);
          flex->State.sync2_shiftreg = (uint16_t)(
            (flex->State.sync2_shiftreg << 2) | (bit_a << 1) | bit_b);
        } else {
          flex->State.sync2_shiftreg = (uint16_t)(
            (flex->State.sync2_shiftreg << 1) | bit_a);
        }

        flex->State.sync2_count++;

        // Buffer symbols near the nominal boundary [nominal-2 .. nominal+2).
        if ((int)flex->State.sync2_count >= (int)s2_symbols - 2 &&
            flex->State.sync2_sym_buf_count < 4) {
          if (flex->State.sync2_sym_buf_count == 0)
            flex->State.sync2_sym_buf_start = flex->State.sync2_count;
          flex->State.sync2_sym_buf[flex->State.sync2_sym_buf_count++] = sym_rectified;
        }

        // Correlate against both C and inv.C independently.
        // C is in the first half, inv.C in the second half.
        // Once C is found, only accept inv.C near c_pos + s2_half (+/-2).
        // Keep best (lowest error) match within the expected window.
        {
          unsigned int bits_per_sym = (flex->Sync.levels == 4) ? 2 : 1;
          unsigned int c_sym_len = 16 / bits_per_sym;
          if (flex->State.sync2_count >= c_sym_len) {
            unsigned int c_errs = count_bits(flex, flex->State.sync2_shiftreg ^ FLEX_S2_C);
            unsigned int cinv_errs = count_bits(flex, flex->State.sync2_shiftreg ^ FLEX_S2_C_INV);

            // C detection: first half only (before s2_half + tolerance)
            if (c_errs <= 2 && flex->State.sync2_count <= s2_half + 2) {
              if (!flex->State.sync2_c_found || (int)c_errs < flex->State.sync2_c_errs) {
                flex->State.sync2_c_found = 1;
                flex->State.sync2_c_pos = flex->State.sync2_count;
                flex->State.sync2_c_errs = (int)c_errs;
                verbprintf(3, "FLEX_NEXT: S2 C detected at symbol %u/%u (%u errors)\n",
                           flex->State.sync2_count, s2_symbols, c_errs);
              }
            }

            // inv.C detection: second half only, and if C was found,
            // only accept near c_pos + s2_half (+/-2).
            if (cinv_errs <= 2 && flex->State.sync2_count > s2_half) {
              int accept = 0;
              if (flex->State.sync2_c_found) {
                // C found -- expect inv.C at c_pos + s2_half
                int expected_cinv = flex->State.sync2_c_pos + (int)s2_half;
                int dist = (int)flex->State.sync2_count - expected_cinv;
                if (dist >= -2 && dist <= 2)
                  accept = 1;
              } else {
                // C not found -- accept any inv.C in second half
                accept = 1;
              }
              if (accept && (!flex->State.sync2_cinv_found || (int)cinv_errs < flex->State.sync2_cinv_errs)) {
                flex->State.sync2_cinv_found = 1;
                flex->State.sync2_cinv_pos = flex->State.sync2_count;
                flex->State.sync2_cinv_errs = (int)cinv_errs;
                verbprintf(3, "FLEX_NEXT: S2 inv.C detected at symbol %u/%u (%u errors)\n",
                           flex->State.sync2_count, s2_symbols, cinv_errs);
              }
            }
          }
        }

        // End of S2 period -- decide boundary and transition to DATA.
        // Check at s2_symbols (nominal) and also handle early boundary.
        {
          int boundary = -1;  // -1 = not decided yet

          // At s2_symbols - 1: check if early boundary detected
          if (flex->State.sync2_count == s2_symbols - 1) {
            if (flex->State.sync2_c_found && flex->State.sync2_cinv_found) {
              int gap = flex->State.sync2_cinv_pos - flex->State.sync2_c_pos;
              int offset = flex->State.sync2_cinv_pos - (int)s2_symbols;
              if (gap == (int)s2_half && offset == -1) {
                boundary = (int)s2_symbols - 1;
                verbprintf(3, "FLEX_NEXT: S2 boundary correction: -1 (C@%d(%de) inv.C@%d(%de) gap=%d)\n",
                           flex->State.sync2_c_pos, flex->State.sync2_c_errs,
                           flex->State.sync2_cinv_pos, flex->State.sync2_cinv_errs, gap);
              }
            } else if (flex->State.sync2_c_found && flex->State.sync2_c_errs <= 1) {
              // C-only (no inv.C confirmation): only trust with <= 1 error.
              // At 2 errors the C pattern can match by coincidence on nominal
              // boundary frames, causing a false -1 shift that swaps phases.
              int offset = flex->State.sync2_c_pos - (int)s2_half;
              if (offset == -1) {
                boundary = (int)s2_symbols - 1;
                verbprintf(3, "FLEX_NEXT: S2 boundary correction from C: -1 (C@%d(%de))\n",
                           flex->State.sync2_c_pos, flex->State.sync2_c_errs);
              }
            }
          }

          // At s2_symbols: nominal boundary (if not already transitioned early)
          if (flex->State.sync2_count == s2_symbols && flex->State.Current == FLEX_STATE_SYNC2) {
            boundary = (int)s2_symbols;
            if (!flex->State.sync2_c_found && !flex->State.sync2_cinv_found) {
              verbprintf(3, "FLEX_NEXT: S2 no C/inv.C detected, using blind count (%u symbols)\n",
                         s2_symbols);
            }
          }

          if (boundary >= 0) {
            verbprintf(3, "FLEX_NEXT: S2 boundary: offset=%+d C:%s inv.C:%s\n",
                       boundary - (int)s2_symbols,
                       flex->State.sync2_c_found ? "yes" : "no",
                       flex->State.sync2_cinv_found ? "yes" : "no");

            flex->State.data_count = 0;
            clear_phase_data(flex);
            flex->State.Current = FLEX_STATE_DATA;
          }
        }

        break;
      }
    case FLEX_STATE_DATA:
      {
        // Continue S2 inv.C correlation for first 2 DATA symbols
        // to detect late S2 boundary and correct for it.
        if (flex->State.data_count < 2 && !flex->State.sync2_cinv_found) {
          flex->State.sync2_count++;
          int bit_a = (sym_rectified > 1);
          if (flex->Sync.levels == 4) {
            int bit_b = (sym_rectified == 1) || (sym_rectified == 2);
            flex->State.sync2_shiftreg = (uint16_t)(
              (flex->State.sync2_shiftreg << 2) | (bit_a << 1) | bit_b);
          } else {
            flex->State.sync2_shiftreg = (uint16_t)(
              (flex->State.sync2_shiftreg << 1) | bit_a);
          }
          unsigned int bits_per_sym = (flex->Sync.levels == 4) ? 2 : 1;
          unsigned int c_sym_len = 16 / bits_per_sym;
          if (flex->State.sync2_count >= c_sym_len) {
            unsigned int cinv_errs = count_bits(flex, flex->State.sync2_shiftreg ^ FLEX_S2_C_INV);
            if (cinv_errs <= 2) {
              unsigned int s2_symbols = flex->Sync.baud*25/1000;
              int late_offset = (int)flex->State.sync2_count - (int)s2_symbols;
              flex->State.sync2_cinv_found = 1;
              flex->State.sync2_cinv_pos = flex->State.sync2_count;
              flex->State.sync2_cinv_errs = (int)cinv_errs;
              verbprintf(3, "FLEX_NEXT: S2 late inv.C at symbol %u/%u (%ue), correcting +%d: reset phase data\n",
                         flex->State.sync2_count, s2_symbols, cinv_errs, late_offset);
              // The first data_count symbols were actually S2 tail.
              // Reset phase data and data_count so DATA starts fresh
              // from the next symbol (the real first DATA symbol).
              clear_phase_data(flex);
              flex->State.data_count = 0;
              break;  // don't feed this symbol to read_data
            }
          }
        }

        int idle = 0;
        idle = read_data(flex, sym_rectified);
        if (++flex->State.data_count == flex->Sync.baud*1760/1000 || idle) {
          decode_data(flex);
          flex->Demodulator.baud = 1600;
          flex->State.Current=FLEX_STATE_SYNC1;
          flex->State.data_count=0;
        }
        break;
      }
  }
}

static int buildSymbol(struct Flex_Next * flex, double sample) {
        if (flex == NULL) return 0;

        const int64_t phase_max = 100 * flex->Demodulator.sample_freq;                           // Maximum value for phase (calculated to divide by sample frequency without remainder)
        const int64_t phase_rate = phase_max*flex->Demodulator.baud / flex->Demodulator.sample_freq;      // Increment per baseband sample
        const double phasepercent = 100.0 *  flex->Demodulator.phase / phase_max;

        /*Update the sample counter*/
        flex->Demodulator.sample_count++;

        /*Remove DC offset (FIR filter)*/
        if (flex->State.Current == FLEX_STATE_SYNC1) {
                flex->Modulation.zero = (flex->Modulation.zero*(FREQ_SAMP*DC_OFFSET_FILTER) + sample) / ((FREQ_SAMP*DC_OFFSET_FILTER) + 1);
        }
        sample -= flex->Modulation.zero;

        if (flex->Demodulator.locked) {
                /*During the synchronisation period, establish the envelope of the signal*/
                if (flex->State.Current == FLEX_STATE_SYNC1) {
                        flex->Demodulator.envelope_sum += fabs(sample);
                        flex->Demodulator.envelope_count++;
                        /* Cap the averaging window to prevent infinite accumulation.
                         * Halve both sum and count when limit reached so older samples
                         * decay while maintaining a smooth average. */
                        if (flex->Demodulator.envelope_count > FREQ_SAMP * 2) {
                                flex->Demodulator.envelope_sum /= 2.0;
                                flex->Demodulator.envelope_count /= 2;
                        }
                        flex->Modulation.envelope = flex->Demodulator.envelope_sum / flex->Demodulator.envelope_count;
                }
        }
        else {
                /*Reset and hold in initial state*/
                flex->Modulation.envelope = 0;
                flex->Demodulator.envelope_sum = 0;
                flex->Demodulator.envelope_count = 0;
                flex->Demodulator.baud = 1600;
                flex->Demodulator.timeout = 0;
                flex->Demodulator.nonconsec = 0;
                flex->State.Current = FLEX_STATE_SYNC1;
        }

        /* MID 80% SYMBOL PERIOD: accumulate for both majority vote and I&D */
        if (phasepercent > 10 && phasepercent <90) {
                /* Majority vote bins */
                if (sample > 0) {
                        if (sample > flex->Modulation.envelope*SLICE_THRESHOLD)
                                flex->Demodulator.symcount[3]++;
                        else
                                flex->Demodulator.symcount[2]++;
                }
                else {
                        if (sample < -flex->Modulation.envelope*SLICE_THRESHOLD)
                                flex->Demodulator.symcount[0]++;
                        else
                                flex->Demodulator.symcount[1]++;
                }
                /* Integrate-and-dump: use center 50% of symbol period.
                 * Excludes inter-symbol transitions for cleaner mean. */
                if (phasepercent > 25 && phasepercent < 75) {
                        flex->Demodulator.sym_sum += sample;
                        flex->Demodulator.sym_n++;
                }
        }

        /* ZERO CROSSING */
        if ((flex->Demodulator.sample_last<0 && sample >= 0) || (flex->Demodulator.sample_last >= 0 && sample<0)) {
                /*The phase error has a direction towards the closest symbol boundary*/
                double phase_error = 0.0;
                if (phasepercent<50) {
                        phase_error = flex->Demodulator.phase;
                }
                else {
                        phase_error = flex->Demodulator.phase - phase_max;
                }

                /*Phase lock with the signal*/
                if (flex->Demodulator.locked) {
                        flex->Demodulator.phase -= phase_error * PHASE_LOCKED_RATE;
                }
                else {
                        flex->Demodulator.phase -= phase_error * PHASE_UNLOCKED_RATE;
                }

                /*If too many zero crossing occur within the mid 80% then indicate lock has been lost*/
                if (phasepercent > 10 && phasepercent < 90) {
                        flex->Demodulator.nonconsec++;
                        if (flex->Demodulator.nonconsec>20 && flex->Demodulator.locked) {
                                verbprintf(1, "FLEX_NEXT: Synchronisation Lost\n");
                                flex->Demodulator.locked = 0;
                        }
                }
                else {
                        flex->Demodulator.nonconsec = 0;
                }

                flex->Demodulator.timeout = 0;
        }
        flex->Demodulator.sample_last = sample;

  /* END OF SYMBOL PERIOD */
  flex->Demodulator.phase += phase_rate;

  if (flex->Demodulator.phase > phase_max) {
    flex->Demodulator.phase -= phase_max;
    return 1;
  } else {
    return 0;
  }

}

static void Flex_Demodulate(struct Flex_Next * flex, double sample) {
  if(flex == NULL) return;

  if (buildSymbol(flex, sample) == 1) {
                flex->Demodulator.nonconsec = 0;
    flex->Demodulator.symbol_count++;
    flex->Modulation.symbol_rate = 1.0 * flex->Demodulator.symbol_count*flex->Demodulator.sample_freq / flex->Demodulator.sample_count;

    /* PRIMARY: Majority vote symbol decision (original method) */
    int j;
    int decmax = 0;
    int symbol = 0;
    for (j = 0; j<4; j++) {
      if (flex->Demodulator.symcount[j] > decmax) {
        symbol = j;
        decmax = flex->Demodulator.symcount[j];
      }
    }
    flex->Demodulator.symcount[0] = 0;
    flex->Demodulator.symcount[1] = 0;
    flex->Demodulator.symcount[2] = 0;
    flex->Demodulator.symcount[3] = 0;

    /* ALTERNATE: Integrate-and-dump symbol decision.
     * Uses the mean of samples in the same window as majority vote.
     * For 4FSK, the mean provides better inner-level discrimination
     * than per-sample voting. */
    int alt_symbol = 1;
    if (flex->Demodulator.sym_n > 0) {
      double mean = flex->Demodulator.sym_sum / flex->Demodulator.sym_n;
      double thr = flex->Modulation.envelope * SLICE_THRESHOLD_IAD;
      if (mean > 0)
        alt_symbol = (mean > thr) ? 3 : 2;
      else
        alt_symbol = (mean < -thr) ? 0 : 1;
    }
    flex->Demodulator.sym_sum = 0;
    flex->Demodulator.sym_n = 0;

    /* Feed alternate I&D symbols into AltA/B/C/D during data for all modes.
     * Mirrors the read_data() phase mapping using the I&D slicer output. */
    if (flex->Demodulator.locked && flex->State.Current == FLEX_STATE_DATA) {
      int alt_bit_a = (alt_symbol > 1);
      int alt_bit_b = (alt_symbol == 1) || (alt_symbol == 2);
      unsigned int idx = ((flex->Data.data_bit_counter>>5)&0xFFF8) | (flex->Data.data_bit_counter&0x0007);

      /* At 1600 baud, phase_toggle is always 0 (no phase alternation).
       * read_data() forces this at the top of each call, but the alt
       * slicer runs BEFORE read_data, so it sees the stale toggle=1
       * left from the previous symbol.  Mirror the same reset here. */
      int alt_toggle = flex->Data.phase_toggle;
      if (flex->Sync.baud == 1600)
        alt_toggle = 0;

      if (idx < PHASE_WORDS) {
        if (alt_toggle == 0) {
          /* At 1600 baud: every symbol. At 3200 baud: even symbols. */
          flex->Data.AltA.buf[idx] = (flex->Data.AltA.buf[idx]>>1) | (alt_bit_a?(0x80000000):0);
          if (flex->Sync.baud == 1600) {
            /* 1600/4FSK: bit_b -> AltC (same as read_data PhaseC mapping) */
            flex->Data.AltC.buf[idx] = (flex->Data.AltC.buf[idx]>>1) | (alt_bit_b?(0x80000000):0);
          } else {
            /* 3200/4FSK: bit_b -> AltB */
            flex->Data.AltB.buf[idx] = (flex->Data.AltB.buf[idx]>>1) | (alt_bit_b?(0x80000000):0);
          }
        } else {
          /* 3200 baud only: odd symbols -> AltC, AltD */
          flex->Data.AltC.buf[idx] = (flex->Data.AltC.buf[idx]>>1) | (alt_bit_a?(0x80000000):0);
          flex->Data.AltD.buf[idx] = (flex->Data.AltD.buf[idx]>>1) | (alt_bit_b?(0x80000000):0);
        }
      }
    }


    if (flex->Demodulator.locked) {
      /*Process the symbol*/
      flex_sym(flex, symbol);
    }
    else {
      /*Check for lock pattern*/
      flex->Demodulator.lock_buf = (flex->Demodulator.lock_buf << 2) | (symbol ^ 0x1);
      uint64_t lock_pattern = flex->Demodulator.lock_buf ^ 0x6666666666666666ull;
      uint64_t lock_mask = (1ull << (2 * LOCK_LEN)) - 1;
      if ((lock_pattern&lock_mask) == 0 || ((~lock_pattern)&lock_mask) == 0) {
        verbprintf(1, "FLEX_NEXT: Locked\n");
        flex->Demodulator.locked = 1;
        /*Clear the syncronisation buffer*/
        flex->Demodulator.lock_buf = 0;
        flex->Demodulator.symbol_count = 0;
        flex->Demodulator.sample_count = 0;
      }
    }

    /*Time out after X periods with no zero crossing*/
    flex->Demodulator.timeout++;
    if (flex->Demodulator.timeout>DEMOD_TIMEOUT) {
      /* Force-decode partial frame if we were collecting data */
      if (flex->Demodulator.locked &&
          flex->State.Current == FLEX_STATE_DATA &&
          flex->State.data_count > 0) {
        verbprintf(1, "FLEX_NEXT: PLL timeout during DATA (%u/%u symbols), force-decoding partial frame.\n",
                   flex->State.data_count, flex->Sync.baud*1760/1000);
        decode_data(flex);
      } else {
        verbprintf(1, "FLEX_NEXT: Timeout\n");
      }
      flex->Demodulator.locked = 0;
    }
  }

  report_state(flex);
}

static void Flex_Delete(struct Flex_Next * flex) {
  if (flex==NULL) return;
  free(flex);
}


static struct Flex_Next * Flex_New(unsigned int SampleFrequency) {
  struct Flex_Next *flex=(struct Flex_Next *)malloc(sizeof(struct Flex_Next));
  if (flex!=NULL) {
    memset(flex, 0, sizeof(struct Flex_Next));

    flex->Demodulator.sample_freq=SampleFrequency;
    // The baud rate of first syncword and FIW is always 1600, so set that
    // rate to start.
    flex->Demodulator.baud = 1600;

    /* Initialize BCH tables (does nothing if already initialized) */
    bch_init();

    for(int g = 0; g < GROUP_BITS; g++)
    {
      flex->GroupHandler.GroupFrame[g] = -1;
          flex->GroupHandler.GroupCycle[g] = -1;
          flex->GroupHandler.GroupDelivering[g] = 0;
    }
  }

  return flex;
}


static void flex_next_demod(struct demod_state *s, buffer_t buffer, int length) {
  if (s==NULL) return;
  if (s->l1.flex_next==NULL) return;
  int i;
  for (i=0; i<length; i++) {
    Flex_Demodulate(s->l1.flex_next, buffer.fbuffer[i]);
  }
}


static void flex_next_init(struct demod_state *s) {
  if (s==NULL) return;
  s->l1.flex_next=Flex_New(FREQ_SAMP);
}


static void flex_next_deinit(struct demod_state *s) {
  if (s==NULL) return;
  if (s->l1.flex_next==NULL) return;

  Flex_Delete(s->l1.flex_next);
  s->l1.flex_next=NULL;
}


const struct demod_param demod_flex_next = {
  "FLEX_NEXT", true, FREQ_SAMP, FILTLEN, flex_next_init, flex_next_demod, flex_next_deinit
};
