/*-----------------------------------------------------------------------
 *
 *      demod_morse.c -- Morse/CW decoder
 *
 *      Written and placed into the public domain by
 *      Elias Önal <morse@eliasoenal.com>
 *
 *-----------------------------------------------------------------------*/
#include "multimon.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define FREQ_SAMP  22050

#define SMOOTHING_MAGNITUDE 9
#define GAIN 1
#define SQUELCH 500
#define HOLDOFF_MS 10               // Compensate for ringing
#define AUTO_THRESHOLD_MULT 2/3
//#define AUTO_THRESHOLD_MULT 1/2

#define DEBUG 0
#define SHOW_FAILED_DECODES 1
#define SPAM_SAMPLES 0
#define SPAM_STATE 0

#define LOW 0
#define HIGH 1
#define DIT 0x1 //0b01 .
#define DAH 0x2 //0b10 -
#define NUM_ELEMENTS(x) (sizeof(x)/sizeof(x[0]))

// Threshold between:   DITs and DAHs is 2*cw_dit_length
//                      GAPS and EOC  is 2*cw_gap_length
//                      EOC  and EOW  is 5*cw_gap_length
int cw_dit_length = 50;
int cw_gap_length = 50;
int cw_threshold = 500;
bool cw_disable_auto_threshold = false;
bool cw_disable_auto_timing = false;

static const struct morse_codes
{
    const uint64_t sequence;
    const char* character;
} morse_codes[] =
{
    // DITs are 0b01 and DAHs are 0b10
    // List is sorted for binary search
    // HexValue       Character     BinValue                 MORSE/CW
    {0x0000,            "<NULL>"},  // 0b0                      
    {0x0001,            "E"},       // 0b01                     .
    {0x0002,            "T"},       // 0b10                     -
    {0x0005,            "I"},       // 0b0101                   ..
    {0x0006,            "A"},       // 0b0110                   .-
    {0x0009,            "N"},       // 0b1001                   -.
    {0x000A,            "M"},       // 0b1010                   --
    {0x0015,            "S"},       // 0b010101                 ...
    {0x0016,            "U"},       // 0b010110                 ..-
    {0x0019,            "R"},       // 0b011001                 .-.
    {0x001A,            "W"},       // 0b011010                 .--
    {0x0025,            "D"},       // 0b100101                 -..
    {0x0026,            "K"},       // 0b100110                 -.-
    {0x0029,            "G"},       // 0b101001                 --.
    {0x002A,            "O"},       // 0b101010                 ---
    {0x0055,            "H"},       // 0b01010101               ....
    {0x0056,            "V"},       // 0b01010110               ...-
    {0x0059,            "F"},       // 0b01011001               ..-.
    {0x005A,            "Ü"},       // 0b01011010               ..-- wrong? *?
    {0x0065,            "L"},       // 0b01100101               .-..
    {0x0066,            "Ä"},       // 0b01100110               .-.-
    {0x0069,            "P"},       // 0b01101001               .--.
    {0x006A,            "J"},       // 0b01101010               .---
    {0x0095,            "B"},       // 0b10010101               -...
    {0x0096,            "X"},       // 0b10010110               -..-
    {0x0099,            "C"},       // 0b10011001               -.-.
    {0x009A,            "Y"},       // 0b10011010               -.--
    {0x00A5,            "Z"},       // 0b10100101               --..
    {0x00A6,            "Q"},       // 0b10100110               --.-
    {0x00A9,            "Ö"},       // 0b10101001               ---.
    {0x00AA,            "CH"},      // 0b10101010               ----
    {0x0155,            "5"},       // 0b0101010101             .....
    {0x0156,            "4"},       // 0b0101010110             ....-
    {0x0159,            "<SN>"},    // 0b0101011001             ...-.
    {0x015A,            "3"},       // 0b0101011010             ...--
    {0x0166,            "/"},       // 0b0101100110             ..-.-
    {0x016A,            "2"},       // 0b0101101010             ..---
    {0x0195,            "&"},       // 0b0110010101             .-...
    {0x0199,            "+"},       // 0b0110011001             .-.-.
    {0x01AA,            "1"},       // 0b0110101010             .----
    {0x0255,            "6"},       // 0b1001010101             -....
    {0x0256,            "="},       // 0b1001010110             -...-
    {0x0266,            "<CT>"},    // 0b1001100110             -.-.-
    {0x0259,            "/"},       // 0b1001011001             -..-. wrong?
    {0x0269,            "("},       // 0b1001101001             -.--.
    {0x0295,            "7"},       // 0b1010010101             --...
    {0x02A5,            "8"},       // 0b1010100101             ---..
    {0x02A9,            "9"},       // 0b1010101001             ----.
    {0x02AA,            "0"},       // 0b1010101010             -----
    {0x0555,            "<ERR_6>"}, // 0b010101010101           ......
    {0x0566,            "<SK>"},    // 0b010101100110           ...-.-
    {0x05A5,            "?"},       // 0b010110100101           ..--..
    {0x05A6,            "_"},       // 0b010110100110           ..--.-
    {0x0659,            "\""},      // 0b011001011001           .-..-.
    {0x0666,            "."},       // 0b011001100110           .-.-.-
    {0x0699,            "@"},       // 0b011010011001           .--.-.
    {0x06A9,            "'"},       // 0b011010101001           .----.
    {0x0956,            "-"},       // 0b100101010110           -....-
    {0x096A,            "<DO>"},    // 0b100101101010           -..---
    {0x0999,            ";"},       // 0b100110011001           -.-.-.
    {0x099A,            "!"},       // 0b100110011010           -.-.--
    {0x09A6,            ")"},       // 0b100110100110           -.--.-
    {0x0A5A,            ","},       // 0b101001011010           --..--
    {0x0A95,            ":"},       // 0b101010010101           ---...
    {0x1555,            "<ERR_7>"}, // 0b01010101010101         .......
    {0x1596,            "$"},       // 0b01010110010110         ...-..-
    {0x2566,            "<BK>"},    // 0b10010101100110         -...-.-
    {0x5555,            "<ERR_8>"}, // 0b0101010101010101       ........
    {0x9965,            "<CL>"},    // 0b1001100101100101       -.-..-..
    {0x15555,           "<ERR_9>"}, // 0b010101010101010101     .........
    {0x15A95,           "<SOS>"},   // 0b010101101010010101     ...---...
    {0x55555,           "<ERR_10>"},// 0b01010101010101010101   ..........
    {0x155555,          "<ERR_11>"},// 0b0101010101010101010101 ...........
    {0x555555,          "<ERR_12>"},// 0b0101010101010101(...)  ......(...)
    {0x1555555,         "<ERR_13>"},// 0b0101010101010101(...)  ......(...)
    {0x5555555,         "<ERR_14>"},// 0b0101010101010101(...)  ......(...)
    {0x15555555,        "<ERR_15>"},// 0b0101010101010101(...)  ......(...)
    {0x55555555,        "<ERR_16>"},// 0b0101010101010101(...)  ......(...)
    {0x155555555,       "<ERR_17>"},// 0b0101010101010101(...)  ......(...)
    {0x555555555,       "<ERR_18>"},// 0b0101010101010101(...)  ......(...)
    {0x1555555555,      "<ERR_19>"},// 0b0101010101010101(...)  ......(...)
    {0x5555555555,      "<ERR_20>"},// 0b0101010101010101(...)  ......(...)
    {0x15555555555,     "<ERR_21>"},// 0b0101010101010101(...)  ......(...)
    {0x55555555555,     "<ERR_22>"},// 0b0101010101010101(...)  ......(...)
    {0x155555555555,    "<ERR_23>"},// 0b0101010101010101(...)  ......(...)
    {0x555555555555,    "<ERR_24>"},// 0b0101010101010101(...)  ......(...)
    {0x1555555555555,   "<ERR_25>"},// 0b0101010101010101(...)  ......(...)
    {0x5555555555555,   "<ERR_26>"},// 0b0101010101010101(...)  ......(...)
    {0x15555555555555,  "<ERR_27>"},// 0b0101010101010101(...)  ......(...)
    {0x55555555555555,  "<ERR_28>"},// 0b0101010101010101(...)  ......(...)
    {0x155555555555555, "<ERR_29>"},// 0b0101010101010101(...)  ......(...)
    {0x555555555555555, "<ERR_30>"},// 0b0101010101010101(...)  ......(...)
    {0x1555555555555555,"<ERR_31>"},// 0b0101010101010101(...)  ......(...)
    {0x5555555555555555,"<ERR_32>"},// 0b0101010101010101(...)  ......(...)
    // Sequences longer than 32 DITs will overflow and show up as <ERR_32> as well.
};

typedef struct dec_ret_t
{
    char* restrict string_ptr;
    char string[32 + 3]; //32 dit/dahs + 2 brackets and 0
    bool status;
} dec_ret_t;

// Binary search is crazy fast! 0.01% of our CPU time. O(log n) ftw!
static inline dec_ret_t decode_character(const struct demod_state * restrict const s)
{
    int_fast16_t min = 0;
    int_fast16_t max = NUM_ELEMENTS(morse_codes) - 1;
    uint64_t sequence = s->l1.morse.current_sequence;
    
    while (max >= min) // Search
    {
        int_fast16_t mid = (min + max) / 2;
        if(morse_codes[mid].sequence > sequence) max = mid - 1;
        else if(morse_codes[mid].sequence < sequence) min = mid + 1;
        else{dec_ret_t rtn = {(char*)morse_codes[mid].character,"",true};
            return rtn;}
    }
    
    // Build ASCII art from sequence
    dec_ret_t rtn = {rtn.string,"",false};
    *(rtn.string_ptr++) = '<';
    char symbol;
    for(int i = 0; i < (int)sizeof(sequence) * 8; i+=2)
        if((symbol = (sequence >> (sizeof(sequence) - 2 - i)) & (0x3)))
            *(rtn.string_ptr++) = (symbol == DIT)?'.':'_';
    *(rtn.string_ptr++) = '>';
    *rtn.string_ptr = '\0';
    rtn.string_ptr = rtn.string;
    return rtn;
}

// Low pass filter - eats 19.4% of the CPU time according to profiling
// It's a basic IIR filter optimized for integer operation while
// minimizing the rounding error.
static inline int_fast32_t low_pass(const int_fast32_t last_filtered,
                                    const int_fast32_t new_sample,
                                    const int_fast16_t strength)
{
    return ((last_filtered << strength) + 
            new_sample - last_filtered) >> strength;
}

// Probably a lot room for improvements here
// Another 19.5% of our precious CPU time. It's optional though.
static inline void auto_threshold(struct demod_state * restrict const s)
{
    // Hackish solution in order to have the threshold adjust.
    // The highest known amplitude bleeds 20 times 0.1%, per second.
    s->l1.morse.threshold_ctr = (s->l1.morse.threshold_ctr+1) % (FREQ_SAMP / 20);
    if(!s->l1.morse.threshold_ctr && s->l1.morse.signal_max > 0)
    {
        s->l1.morse.signal_max = s->l1.morse.signal_max * 999 / 1000;
        s->l1.morse.detection_threshold = s->l1.morse.signal_max*AUTO_THRESHOLD_MULT;
    }
    
    // Check for a higher upper limit
    if(s->l1.morse.filtered > s->l1.morse.signal_max)
    {
        s->l1.morse.signal_max = s->l1.morse.filtered;
        s->l1.morse.detection_threshold = s->l1.morse.signal_max*AUTO_THRESHOLD_MULT;
    }
    
    // Prevent threshold from dropping below SQUELCH
    if(s->l1.morse.detection_threshold < SQUELCH)
        s->l1.morse.detection_threshold = SQUELCH;
}

// TODO: Come up with a more fancy solution!
static inline void auto_timing(const bool state, struct demod_state * restrict const s)
{
    if(s->l1.morse.samples_since_change < FREQ_SAMP / (1000 / 120)) //120ms
    {
        if(state == LOW)
        {
            if(s->l1.morse.time_unit_gaps_samples > s->l1.morse.samples_since_change)
                s->l1.morse.time_unit_gaps_samples -= 50;
            else s->l1.morse.time_unit_gaps_samples += 50;
        }
        else
        {
            if(s->l1.morse.time_unit_dit_dah_samples > s->l1.morse.samples_since_change)
                s->l1.morse.time_unit_dit_dah_samples -= 50;
            else s->l1.morse.time_unit_dit_dah_samples += 50;
        }
    }
}

// This one gets about 60% of our cycles.
static void morse_demod(struct demod_state * restrict const s,
                        const buffer_t buffer, const int length)
{
    for(int i = 0; i < length; i++)
    {
        // A low-pass is nice, though we could add a high-pass in order to get a band-pass :)
        s->l1.morse.filtered = low_pass(s->l1.morse.filtered, (int_fast32_t)abs(buffer.sbuffer[i]) * GAIN,
                                       s->l1.morse.lowpass_strength);
        
        // Don't count too far
        if(s->l1.morse.samples_since_change < INT_FAST32_MAX/1000)
            s->l1.morse.samples_since_change++;
        
        if(!cw_disable_auto_threshold) auto_threshold(s);
        
        int_fast8_t oldstate = s->l1.morse.current_state;
        
        // Reject change for holdoff period
        if(s->l1.morse.samples_since_change > s->l1.morse.holdoff_samples)
            s->l1.morse.current_state = s->l1.morse.filtered > s->l1.morse.detection_threshold;
        
        if(SPAM_SAMPLES) verbprintf(0, " %d", s->l1.morse.filtered);
        if(SPAM_STATE) verbprintf(0, " %s", s->l1.morse.current_state?"#":".");
        
        int_fast8_t statechange = oldstate != s->l1.morse.current_state;
        int_fast8_t timeout = s->l1.morse.samples_since_change == 5*s->l1.morse.time_unit_gaps_samples;
        
        // Enter on state transition or timeout
        if(statechange || timeout)
        {
            // Ignore glitches only lasting the holdoff period
            if(s->l1.morse.samples_since_change == s->l1.morse.holdoff_samples+1)
            {
                if(DEBUG) verbprintf(0, "<GLITCH %dms>", s->l1.morse.samples_since_change * 1000 / FREQ_SAMP);
                s->l1.morse.glitches++;
                goto reset_samples;
            }
            
            if(oldstate == LOW)
            {
                // Check whether it was just a inter DIT/DAH gap, else decode
                if(s->l1.morse.samples_since_change >= 2*s->l1.morse.time_unit_gaps_samples)
                {
                    dec_ret_t rtn = {NULL,"",false};
                    if(s->l1.morse.current_sequence)
                    {
                        rtn = decode_character(s);
                        if(SHOW_FAILED_DECODES) verbprintf(0, "%s", rtn.string_ptr);
                        else if(rtn.status) verbprintf(0, "%s", rtn.string_ptr);
                        
                        if(rtn.status) s->l1.morse.decoded_chars++;
                        else s->l1.morse.erroneous_chars++;
                        s->l1.morse.current_sequence = 0; // Start a new sequence
                    }
                    
                    if(s->l1.morse.samples_since_change < 5*s->l1.morse.time_unit_gaps_samples) // End of Char
                    {
                        if(DEBUG) verbprintf(0, "<EOC %dms>", s->l1.morse.samples_since_change * 1000 / FREQ_SAMP);
                    }
                    else if(timeout) // End of word - timeout
                    {
                        if(rtn.status)
                            verbprintf(0," "); //Don't print additional spaces if last character failed to decode
                        if(DEBUG) verbprintf(0, "<EOW %dms>", s->l1.morse.samples_since_change * 1000 / FREQ_SAMP);
                        goto end; // Don't reset samples, since there wasn't any change in state.
                    }
                } // It was just a inter DIT/DAH gap
                else if(DEBUG) verbprintf(0, "<GAP %dms>", s->l1.morse.samples_since_change * 1000 / FREQ_SAMP);
            }
            else // Last state was either DIT or DAH
            {
                if(s->l1.morse.samples_since_change < 2*s->l1.morse.time_unit_dit_dah_samples)
                {
                    s->l1.morse.current_sequence = (s->l1.morse.current_sequence << 2) | DIT;
                    if(DEBUG) verbprintf(0, "<DIT %dms>", s->l1.morse.samples_since_change * 1000 / FREQ_SAMP);
                }
                else 
                {
                    s->l1.morse.current_sequence = (s->l1.morse.current_sequence << 2) | DAH;
                    if(DEBUG) verbprintf(0, "<DAH %dms>", s->l1.morse.samples_since_change * 1000 / FREQ_SAMP);
                }
            }
            
            if(!cw_disable_auto_timing) auto_timing(oldstate, s);
reset_samples:
            s->l1.morse.samples_since_change = 0; // State has changed, restart counting
end:;
        }
    }
}

static void morse_init(struct demod_state * restrict s)
{
    memset(&s->l1.morse, 0, sizeof(s->l1.morse));
    s->l1.morse.time_unit_dit_dah_samples = FREQ_SAMP / (1000 / cw_dit_length);
    s->l1.morse.time_unit_gaps_samples = FREQ_SAMP / (1000 / cw_gap_length);
    s->l1.morse.detection_threshold = cw_threshold;
    s->l1.morse.lowpass_strength = SMOOTHING_MAGNITUDE;
    if(HOLDOFF_MS) s->l1.morse.holdoff_samples = FREQ_SAMP / (1000 / HOLDOFF_MS);
    
    s->l1.morse.signal_max = SQUELCH;
}

static void morse_deinit(struct demod_state * const restrict s)
{
    verbprintf(1, "\nMAX: %d THRESHOLD: %d GLITCHES: %d FAILED: %d "
               "DECODED: %d TIMING_GAP: %d TIMING_DIT: %d",
               s->l1.morse.signal_max,
               s->l1.morse.detection_threshold,
               s->l1.morse.glitches,
               s->l1.morse.erroneous_chars,
               s->l1.morse.decoded_chars,
               s->l1.morse.time_unit_gaps_samples * 1000 / FREQ_SAMP,
               s->l1.morse.time_unit_dit_dah_samples * 1000 / FREQ_SAMP);
    verbprintf(0, "\n");
}

const struct demod_param demod_morse = {
    "MORSE_CW", false, FREQ_SAMP, 0, morse_init, morse_demod, morse_deinit
};
