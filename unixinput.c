/*
 *      unixinput.c -- input sound samples
 *
 *      Copyright (C) 1996
 *          Thomas Sailer (sailer@ife.ee.ethz.ch, hb9jnx@hb9w.che.eu)
 *
 *      Copyright (C) 2012-2025
 *          Elias Oenal    (multimon-ng@eliasoenal.com)
 *
 *      Copyright (C) 2024
 *          Jason Lingohr (jason@lucid.net.au)
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
#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _MSC_VER
#include <io.h>
#else
#include <unistd.h>
#endif
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <getopt.h>

#ifdef SUN_AUDIO
#include <sys/audioio.h>
#include <stropts.h>
#include <sys/conf.h>
#elif PULSE_AUDIO
#include <pulse/simple.h>
#include <pulse/error.h>
#elif WIN32_AUDIO
//see win32_soundin.c
#elif DUMMY_AUDIO
// NO AUDIO FOR OSX :/
#else /* SUN_AUDIO */
#include <sys/soundcard.h>
#include <sys/ioctl.h>
//#include <sys/wait.h>
#endif /* SUN_AUDIO */

#ifndef ONLY_RAW
#include <sys/wait.h>
#endif

/* ---------------------------------------------------------------------- */

static const char *allowed_types[] = {
    "raw", "aiff", "au", "hcom", "sf", "voc", "cdr", "dat",
    "smp", "wav", "maud", "vwe", "mp3", "mp4", "ogg", "flac", NULL
};

/* ---------------------------------------------------------------------- */

static const struct demod_param *dem[] = { ALL_DEMOD };

#define NUMDEMOD (sizeof(dem)/sizeof(dem[0]))

static struct demod_state dem_st[NUMDEMOD];
static unsigned int dem_mask[(NUMDEMOD+31)/32];

#define MASK_SET(n) dem_mask[(n)>>5] |= 1<<((n)&0x1f)
#define MASK_RESET(n) dem_mask[(n)>>5] &= ~(1<<((n)&0x1f))
#define MASK_ISSET(n) (dem_mask[(n)>>5] & 1<<((n)&0x1f))

#define MAX_VAR_ARGS 100

/* ---------------------------------------------------------------------- */

static int verbose_level = 0;
static int repeatable_sox = 0;
static int mute_sox = 0;
static int integer_only = true;
static bool dont_flush = false;
static bool is_startline = true;
static int timestamp = 0;
static int iso8601 = 0;
static char *label = NULL;
int json_mode = 0;

extern bool fms_justhex;

extern int pocsag_mode;
extern int pocsag_invert_input;
extern int pocsag_error_correction;
extern int pocsag_show_partial_decodes;
extern int pocsag_heuristic_pruning;
extern int pocsag_prune_empty;
extern bool pocsag_init_charset(char *charset);

extern int aprs_mode;
extern int cw_dit_length;
extern int cw_gap_length;
extern int cw_threshold;
extern bool cw_disable_auto_threshold;
extern bool cw_disable_auto_timing;

extern int flex_disable_timestamp;

void quit(void);

/* ---------------------------------------------------------------------- */

void _verbprintf(int verb_level, const char *fmt, ...)
{
	char time_buf[20];

    if (verb_level > verbose_level)
        return;
    va_list args;
    va_start(args, fmt);

    if (is_startline)
    {
        if (label != NULL)
            fprintf(stdout, "%s: ", label);
        
        if (timestamp) {
            if(iso8601)
            {
                struct timespec ts;
                timespec_get(&ts, TIME_UTC);
                strftime(time_buf, sizeof time_buf, "%FT%T", gmtime(&ts.tv_sec)); //2024-09-13T20:35:30
                fprintf(stdout, "%s.%06ld: ", time_buf, ts.tv_nsec/1000); //2024-09-13T20:35:30.156337
            }
            else
            {
                time_t t;
                struct tm* tm_info;
                t = time(NULL);
                tm_info = localtime(&t);
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
                fprintf(stdout, "%s: ", time_buf);
            }
        }

        is_startline = false;
    }
    if (NULL != strchr(fmt,'\n')) /* detect end of line in stream */
        is_startline = true;

    vfprintf(stdout, fmt, args);
    if(!dont_flush)
        fflush(stdout);
    va_end(args);
}

/* ---------------------------------------------------------------------- */

void process_buffer(float *float_buf, short *short_buf, unsigned int len)
{
    for (int i = 0; (unsigned int) i <  NUMDEMOD; i++)
        if (MASK_ISSET(i) && dem[i]->demod)
        {
            buffer_t buffer = {short_buf, float_buf};
            dem[i]->demod(dem_st+i, buffer, len);
        }
}

/* ---------------------------------------------------------------------- */
#ifdef SUN_AUDIO

static void input_sound(unsigned int sample_rate, unsigned int overlap,
                        const char *ifname)
{
    audio_info_t audioinfo;
    audio_info_t audioinfo2;
    audio_device_t audiodev;
    int fd;
    short buffer[8192];
    float fbuf[16384];
    unsigned int fbuf_cnt = 0;
    int i;
    short *sp;
    
    if ((fd = open(ifname ? ifname : "/dev/audio", O_RDONLY)) < 0) {
        perror("open");
        exit (10);
    }
    if (ioctl(fd, AUDIO_GETDEV, &audiodev) == -1) {
        perror("ioctl: AUDIO_GETDEV");
        exit (10);
    }
    AUDIO_INITINFO(&audioinfo);
    audioinfo.record.sample_rate = sample_rate;
    audioinfo.record.channels = 1;
    audioinfo.record.precision = 16;
    audioinfo.record.encoding = AUDIO_ENCODING_LINEAR;
    /*audioinfo.record.gain = 0x20;
      audioinfo.record.port = AUDIO_LINE_IN;
      audioinfo.monitor_gain = 0;*/
    if (ioctl(fd, AUDIO_SETINFO, &audioinfo) == -1) {
        perror("ioctl: AUDIO_SETINFO");
        exit (10);
    }
    if (ioctl(fd, I_FLUSH, FLUSHR) == -1) {
        perror("ioctl: I_FLUSH");
        exit (10);
    }
    if (ioctl(fd, AUDIO_GETINFO, &audioinfo2) == -1) {
        perror("ioctl: AUDIO_GETINFO");
        exit (10);
    }
    fprintf(stdout, "Audio device: name %s, ver %s, config %s, "
            "sampling rate %d\n", audiodev.name, audiodev.version,
            audiodev.config, audioinfo.record.sample_rate);
    for (;;) {
        i = read(fd, sp = buffer, sizeof(buffer));
        if (i < 0 && errno != EAGAIN) {
            perror("read");
            exit(4);
        }
        if (!i)
            break;
        if (i > 0) {
            if(integer_only)
        {
                fbuf_cnt = i/sizeof(buffer[0]);
        }
            else
            {
                for (; i >= sizeof(buffer[0]); i -= sizeof(buffer[0]), sp++)
                    fbuf[fbuf_cnt++] = (*sp) * (1.0/32768.0);
                if (i)
                    fprintf(stderr, "warning: noninteger number of samples read\n");
            }
            if (fbuf_cnt > overlap) {
                process_buffer(fbuf, buffer, fbuf_cnt-overlap);
                memmove(fbuf, fbuf+fbuf_cnt-overlap, overlap*sizeof(fbuf[0]));
                fbuf_cnt = overlap;
            }
        }
    }
    close(fd);
}

#elif DUMMY_AUDIO
static void input_sound(unsigned int sample_rate, unsigned int overlap,
                        const char *ifname)
{
    (void)sample_rate;
    (void)overlap;
    (void)ifname;
}
#elif WIN32_AUDIO
//Implemented in win32_soundin.c
void input_sound(unsigned int sample_rate, unsigned int overlap, const char *ifname);
#elif PULSE_AUDIO
static void input_sound(unsigned int sample_rate, unsigned int overlap,
                        const char *ifname)
{
    
    short buffer[8192];
    float fbuf[16384];
    unsigned int fbuf_cnt = 0;
    int i;
    int error;
    short *sp;

    (void) ifname;  // Suppress the warning.

    
    // Init stuff from pa.org
    pa_simple *s;
    pa_sample_spec ss;
    
    ss.format = PA_SAMPLE_S16NE;
    ss.channels = 1;
    ss.rate = sample_rate;
    
    
    /* Create the recording stream */
    if (!(s = pa_simple_new(NULL, "multimon-ng", PA_STREAM_RECORD, NULL, "record", &ss, NULL, NULL, &error))) {
        fprintf(stderr, "unixinput.c: pa_simple_new() failed: %s\n", pa_strerror(error));
        exit(4);
    }
    
    for (;;) {
        i = pa_simple_read(s, sp = buffer, sizeof(buffer), &error);
        if (i < 0 && errno != EAGAIN) {
            perror("read");
            fprintf(stderr, "error 1\n");
            exit(4);
        }
        i=sizeof(buffer);
        if (!i)
            break;
        
        if (i > 0) {
            if(integer_only)
        {
                fbuf_cnt = i/sizeof(buffer[0]);
        }
            else
            {
                for (; (unsigned int) i >= sizeof(buffer[0]); i -= sizeof(buffer[0]), sp++)
                    fbuf[fbuf_cnt++] = (*sp) * (1.0/32768.0);
                if (i)
                    fprintf(stderr, "warning: noninteger number of samples read\n");
            }
            if (fbuf_cnt > overlap) {
                process_buffer(fbuf, buffer, fbuf_cnt-overlap);
                memmove(fbuf, fbuf+fbuf_cnt-overlap, overlap*sizeof(fbuf[0]));
                fbuf_cnt = overlap;
            }
        }
    }
    pa_simple_free(s);
}

#else /* SUN_AUDIO */
/* ---------------------------------------------------------------------- */

static void input_sound(unsigned int sample_rate, unsigned int overlap,
                        const char *ifname)
{
    int sndparam;
    int fd;
    union {
        short s[8192];
        unsigned char b[8192];
    } b;
    float fbuf[16384];
    unsigned int fbuf_cnt = 0;
    int i;
    short *sp;
    unsigned char *bp;
    int fmt = 0;
    
    if ((fd = open(ifname ? ifname : "/dev/dsp", O_RDONLY)) < 0) {
        perror("open");
        exit (10);
    }
    sndparam = AFMT_S16_NE; /* we want 16 bits/sample signed */
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &sndparam) == -1) {
        perror("ioctl: SNDCTL_DSP_SETFMT");
        exit (10);
    }
    if (sndparam != AFMT_S16_NE) {
        fmt = 1;
        sndparam = AFMT_U8;
        if (ioctl(fd, SNDCTL_DSP_SETFMT, &sndparam) == -1) {
            perror("ioctl: SNDCTL_DSP_SETFMT");
            exit (10);
        }
        if (sndparam != AFMT_U8) {
            perror("ioctl: SNDCTL_DSP_SETFMT");
            exit (10);
        }
    }
    sndparam = 0;   /* we want only 1 channel */
    if (ioctl(fd, SNDCTL_DSP_STEREO, &sndparam) == -1) {
        perror("ioctl: SNDCTL_DSP_STEREO");
        exit (10);
    }
    if (sndparam != 0) {
        fprintf(stderr, "soundif: Error, cannot set the channel "
                "number to 1\n");
        exit (10);
    }
    sndparam = sample_rate;
    if (ioctl(fd, SNDCTL_DSP_SPEED, &sndparam) == -1) {
        perror("ioctl: SNDCTL_DSP_SPEED");
        exit (10);
    }
    if ((10*abs(sndparam-sample_rate)) > sample_rate) {
        perror("ioctl: SNDCTL_DSP_SPEED");
        exit (10);
    }
    if (sndparam != sample_rate) {
        fprintf(stderr, "Warning: Sampling rate is %u, "
                "requested %u\n", sndparam, sample_rate);
    }
#if 0
    sndparam = 4;
    if (ioctl(fd, SOUND_PCM_SUBDIVIDE, &sndparam) == -1) {
        perror("ioctl: SOUND_PCM_SUBDIVIDE");
    }
    if (sndparam != 4) {
        perror("ioctl: SOUND_PCM_SUBDIVIDE");
    }
#endif
    for (;;) {
        if (fmt) {
            perror("ioctl: 8BIT SAMPLES NOT SUPPORTED!");
            exit (10);
            //            i = read(fd, bp = b.b, sizeof(b.b));
            //            if (i < 0 && errno != EAGAIN) {
            //                perror("read");
            //                exit(4);
            //            }
            //            if (!i)
            //                break;
            //            if (i > 0) {
            //                for (; i >= sizeof(b.b[0]); i -= sizeof(b.b[0]), sp++)
            //                    fbuf[fbuf_cnt++] = ((int)(*bp)-0x80) * (1.0/128.0);
            //                if (i)
            //                    fprintf(stderr, "warning: noninteger number of samples read\n");
            //                if (fbuf_cnt > overlap) {
            //                    process_buffer(fbuf, fbuf_cnt-overlap);
            //                    memmove(fbuf, fbuf+fbuf_cnt-overlap, overlap*sizeof(fbuf[0]));
            //                    fbuf_cnt = overlap;
            //                }
            //            }
        } else {
            i = read(fd, sp = b.s, sizeof(b.s));
            if (i < 0 && errno != EAGAIN) {
                perror("read");
                exit(4);
            }
            if (!i)
                break;
            if (i > 0) {
                if(integer_only)
        {
                    fbuf_cnt = i/sizeof(b.s[0]);
        }
                else
                {
                    for (; i >= sizeof(b.s[0]); i -= sizeof(b.s[0]), sp++)
                        fbuf[fbuf_cnt++] = (*sp) * (1.0/32768.0);
                    if (i)
                        fprintf(stderr, "warning: noninteger number of samples read\n");
                }
                if (fbuf_cnt > overlap) {
                    process_buffer(fbuf, b.s, fbuf_cnt-overlap);
                    memmove(fbuf, fbuf+fbuf_cnt-overlap, overlap*sizeof(fbuf[0]));
                    fbuf_cnt = overlap;
                }
            }
        }
    }
    close(fd);
}
#endif /* SUN_AUDIO */

/* ---------------------------------------------------------------------- */

static void input_file(unsigned int sample_rate, unsigned int overlap,
                       const char *fname, const char *type)
{
    struct stat statbuf;
    int pipedes[2];
    int pid = 0, soxstat;
    int fd;
    int i;
    short buffer[8192];
    float fbuf[16384];
    unsigned int fbuf_cnt = 0;
    short *sp;
    
    /*
     * if the input type is not raw, sox is started to convert the
     * samples to the requested format
     */
    if (!strcmp(fname, "-"))
    {
        // read from stdin and force raw input
        fd = 0;
        type = "raw";
#ifdef WINDOWS
        setmode(fd, O_BINARY);
#endif
    }
    else if (!type || !strcmp(type, "raw")) {
#ifdef WINDOWS
        if ((fd = open(fname, O_RDONLY | O_BINARY)) < 0) {
#else
        if ((fd = open(fname, O_RDONLY)) < 0) {
#endif
            perror("open");
            exit(10);
        }
    }
    
#ifndef ONLY_RAW
    else {
        if (stat(fname, &statbuf)) {
            perror("stat");
            exit(10);
        }
        if (pipe(pipedes)) {
            perror("pipe");
            exit(10);
        }
        if (!(pid = fork())) {
            char srate[8];
            /*
             * child starts here... first set up filedescriptors,
             * then start sox...
             */
            sprintf(srate, "%d", sample_rate);
            close(pipedes[0]); /* close reading pipe end */
            close(1); /* close standard output */
            if (dup2(pipedes[1], 1) < 0)
                perror("dup2");
            close(pipedes[1]); /* close writing pipe end */
            execlp("sox", "sox", repeatable_sox?"-R":"-V2", mute_sox?"-V1":"-V2",
                   "--ignore-length",
                   "-t", type, fname,
                   "-t", "raw", "-esigned-integer", "-b16", "-r", srate, "-", "remix", "1",
                   NULL);
            perror("execlp");
            exit(10);
        }
        if (pid < 0) {
            perror("fork");
            exit(10);
        }
        close(pipedes[1]); /* close writing pipe end */
        fd = pipedes[0];
    }
#endif

    /*
     * demodulate
     */
    for (;;) {
        i = read(fd, sp = buffer, sizeof(buffer));
        if (i < 0 && errno != EAGAIN) {
            perror("read");
            exit(4);
        }
        if (!i)
            break;
        if (i > 0) {
            if(integer_only)
        {
                fbuf_cnt = i/sizeof(buffer[0]);
        }
            else
            {
                for (; (unsigned int) i >= sizeof(buffer[0]); i -= sizeof(buffer[0]), sp++)
                    fbuf[fbuf_cnt++] = (*sp) * (1.0f/32768.0f);
                if (i)
                    fprintf(stderr, "warning: noninteger number of samples read\n");
            }
            if (fbuf_cnt > overlap) {
                process_buffer(fbuf, buffer, fbuf_cnt-overlap);
                memmove(fbuf, fbuf+fbuf_cnt-overlap, overlap*sizeof(fbuf[0]));
                fbuf_cnt = overlap;
            }
        }
    }
    close(fd);
    
#ifndef ONLY_RAW
    waitpid(pid, &soxstat, 0);
#endif
}

void quit(void)
{
    int i = 0;
    for (i = 0; (unsigned int) i < NUMDEMOD; i++)
    {
        if(MASK_ISSET(i))
            if (dem[i]->deinit)
                dem[i]->deinit(dem_st+i);
    }
}

/* ---------------------------------------------------------------------- */

static const char usage_str[] = "\n"
        "Usage: %s [file] [file] [file] ...\n"
        "  If no [file] is given, input will be read from your default sound\n"
        "  hardware. A filename of \"-\" denotes standard input.\n"
        "  -t <type>    : Input file type (any other type than raw requires sox)\n"
        "  -a <demod>   : Add demodulator\n"
        "  -s <demod>   : Subtract demodulator\n"
        "  -c           : Remove all demodulators (must be added with -a <demod>)\n"
        "  -q           : Quiet\n"
        "  -v <level>   : Level of verbosity (e.g. '-v 3')\n"
        "                 For POCSAG and MORSE_CW '-v1' prints decoding statistics.\n"
        "  -h           : This help\n"
        "  -A           : APRS mode (TNC2 text output)\n"
        "  -m           : Mute SoX warnings\n"
        "  -r           : Call SoX in repeatable mode (e.g. fixed random seed for dithering)\n"
        "  -n           : Don't flush stdout, increases performance.\n"
        "  -j           : FMS: Just output hex data and CRC, no parsing.\n"
        "  -e           : POCSAG: Hide empty messages.\n"
        "  -u           : POCSAG: Heuristically prune unlikely decodes.\n"
        "  -i           : POCSAG: Inverts the input samples. Try this if decoding fails.\n"
        "  -p           : POCSAG: Show partially received messages.\n"
        "  -f <mode>    : POCSAG: Overrides standards and forces decoding of data as <mode>\n"
        "                         (<mode> can be 'numeric', 'alpha', 'skyper' or 'auto')\n"
        "  -b <level>   : POCSAG: BCH bit error correction level. Set 0 to disable, default is 2.\n"
        "                         Lower levels increase performance and lower false positives.\n"
        "  -C <cs>      : POCSAG: Set Charset.\n"
        "  -o           : CW: Set threshold for dit detection (default: 500)\n"
        "  -d           : CW: Dit length in ms (default: 50)\n"
        "  -g           : CW: Gap length in ms (default: 50)\n"
        "  -x           : CW: Disable auto threshold detection\n"
        "  -y           : CW: Disable auto timing detection\n"
        "  --timestamp  : Add a time stamp in front of every printed line\n"
        "  --iso8601    : Use UTC timestamp in ISO 8601 format that includes microseconds\n"
        "  --label      : Add a label to the front of every printed line\n"
        "  --flex-no-ts : FLEX: Do not add a timestamp to the FLEX demodulator output\n"
        "  --json       : Format output as JSON. Supported by the following demodulators:\n"
        "                 DTMF, EAS, FLEX, POCSAG. (Other demodulators will silently ignore this flag.)\n"
        "\n"
        "   Raw input requires one channel, 16 bit, signed integer (platform-native)\n"
        "   samples at the demodulator's input sampling rate, which is\n"
        "   usually 22050 Hz. Raw input is assumed and required if piped input is used.\n";

int main(int argc, char *argv[])
{
    int c;
    int errflg = 0;
    int quietflg = 0;
    int i;
    char **itype;
    int mask_first = 1;
    int sample_rate = -1;
    unsigned int overlap = 0;
    char *input_type = "hw";

    static struct option long_options[] =
      {
        {"timestamp", no_argument, &timestamp, 1},
        {"flex-no-ts", no_argument, &flex_disable_timestamp, 1},
        {"iso8601", no_argument, &iso8601, 1},
        {"label", required_argument, NULL, 'l'},
        {"charset", required_argument, NULL, 'C'},
        {"json", no_argument, &json_mode, 1},
        {0, 0, 0, 0}
      };

    while ((c = getopt_long(argc, argv, "t:a:s:v:f:b:C:o:d:g:cqhAmrnjeuipxy", long_options, NULL)) != EOF) {
        switch (c) {
        case 'h':
        case '?':
            errflg++;
            break;
            
        case 'q':
            quietflg++;
            break;
            
        case 'A':
            aprs_mode = 1;
            memset(dem_mask, 0, sizeof(dem_mask));
            mask_first = 0;
            for (i = 0; (unsigned int) i < NUMDEMOD; i++)
                if (!strcasecmp("AFSK1200", dem[i]->name)) {
                    MASK_SET(i);
                    break;
                }
            break;
            
        case 'v':
            verbose_level = strtoul(optarg, 0, 0);
            break;

        case 'b':
            pocsag_error_correction = strtoul(optarg, 0, 0);
            if(pocsag_error_correction > 2 || pocsag_error_correction < 0)
            {
                fprintf(stderr, "Invalid error correction value!\n");
                pocsag_error_correction = 2;
            }
            break;

        case'p':
            pocsag_show_partial_decodes = 1;
            break;

        case'u':
            pocsag_heuristic_pruning = 1;
            break;

        case'e':
            pocsag_prune_empty = 1;
            break;
            
        case 'm':
            mute_sox = 1;
            break;

        case 'j':
            fms_justhex = true;
            break;
            
        case 'r':
            repeatable_sox = 1;
            break;
            
        case 't':
            for (itype = (char **)allowed_types; *itype; itype++)
                if (!strcmp(*itype, optarg)) {
                    input_type = *itype;
                    goto intypefound;
                }
            fprintf(stderr, "invalid input type \"%s\"\n"
                    "allowed types: ", optarg);
            for (itype = (char **)allowed_types; *itype; itype++)
                fprintf(stderr, "%s ", *itype);
            fprintf(stderr, "\n");
            errflg++;
intypefound:
            break;
            
        case 'a':
            if (mask_first)
                memset(dem_mask, 0, sizeof(dem_mask));
            mask_first = 0;
            for (i = 0; (unsigned int) i < NUMDEMOD; i++)
                if (!strcasecmp(optarg, dem[i]->name)) {
                    MASK_SET(i);
                    break;
                }
            if ((unsigned int) i >= NUMDEMOD) {
                fprintf(stderr, "invalid mode \"%s\"\n", optarg);
                errflg++;
            }
            break;
            
        case 's':
            if (mask_first)
                memset(dem_mask, 0xff, sizeof(dem_mask));
            mask_first = 0;
            for (i = 0; (unsigned int) i < NUMDEMOD; i++)
                if (!strcasecmp(optarg, dem[i]->name)) {
                    MASK_RESET(i);
                    break;
                }
            if ((unsigned int) i >= NUMDEMOD) {
                fprintf(stderr, "invalid mode \"%s\"\n", optarg);
                errflg++;
            }
            break;
            
        case 'c':
            if (mask_first)
                memset(dem_mask, 0xff, sizeof(dem_mask));
            mask_first = 0;
            for (i = 0; (unsigned int) i < NUMDEMOD; i++)
                MASK_RESET(i);
            break;
            
        case 'f':
            if(!pocsag_mode)
            {
                if(!strncmp("numeric",optarg, sizeof("numeric")))
                    pocsag_mode = POCSAG_MODE_NUMERIC;
                else if(!strncmp("alpha",optarg, sizeof("alpha")))
                    pocsag_mode = POCSAG_MODE_ALPHA;
                else if(!strncmp("skyper",optarg, sizeof("skyper")))
                    pocsag_mode = POCSAG_MODE_SKYPER;
                else if(!strncmp("auto",optarg, sizeof("auto")))
                    pocsag_mode = POCSAG_MODE_AUTO;
            }else fprintf(stderr, "a POCSAG mode has already been selected!\n");
            break;
            
        case 'C':
    		if (!pocsag_init_charset(optarg))
    			errflg++;
        	break;
        	
        case 'n':
            dont_flush = true;
            break;

        case 'i':
            pocsag_invert_input = true;
            break;
            
        case 'd':
        {
            int i = 0;
            sscanf(optarg, "%d", &i);
            if(i) cw_dit_length = abs(i);
            break;
        }
            
        case 'g':
        {
            int i = 0;
            sscanf(optarg, "%d", &i);
            if(i) cw_gap_length = abs(i);
            break;
        }
            
        case 'o':
        {
            int i = 0;
            sscanf(optarg, "%d", &i);
            if(i) cw_threshold = abs(i);
            break;
        }
            
        case 'x':
            cw_disable_auto_threshold = true;
            break;
            
        case 'y':
            cw_disable_auto_timing = true;
            break;
            
	case 'l':
	    label = optarg;
	    break;
        }
    }


    if ( !quietflg && !json_mode)
    { // pay heed to the quietflg or JSON mode
        fprintf(stderr, "multimon-ng 1.4.1\n"
            "  (C) 1996/1997 by Tom Sailer HB9JNX/AE4WA\n"
            "  (C) 2012-2025 by Elias Oenal\n"
            "Available demodulators:");
        for (i = 0; (unsigned int) i < NUMDEMOD; i++) {
            fprintf(stderr, " %s", dem[i]->name);
        }
        fprintf(stderr, "\n");
    }

    if (errflg) {
        (void)fprintf(stderr, usage_str, argv[0]);
        exit(2);
    }
    if (mask_first)
        memset(dem_mask, 0xff, sizeof(dem_mask));
    
    if (!quietflg && !json_mode)
        fprintf(stdout, "Enabled demodulators:");
    for (i = 0; (unsigned int) i < NUMDEMOD; i++)
        if (MASK_ISSET(i)) {
            if (!quietflg && !json_mode)
                fprintf(stdout, " %s", dem[i]->name);       //Print demod name
            if(dem[i]->float_samples) integer_only = false; //Enable float samples on demand
            memset(dem_st+i, 0, sizeof(dem_st[i]));
            dem_st[i].dem_par = dem[i];
            if (dem[i]->init)
                dem[i]->init(dem_st+i);
            if (sample_rate == -1)
                sample_rate = dem[i]->samplerate;
            else if ( (unsigned int) sample_rate != dem[i]->samplerate) {
                if (!quietflg && !json_mode)
                    fprintf(stdout, "\n");
                fprintf(stderr, "Error: Current sampling rate %d, "
                        " demodulator \"%s\" requires %d\n",
                        sample_rate, dem[i]->name, dem[i]->samplerate);
                exit(3);
            }
            if (dem[i]->overlap > overlap)
                overlap = dem[i]->overlap;
        }
    if (!quietflg && !json_mode)
        fprintf(stdout, "\n");
    
    if (optind < argc && !strcmp(argv[optind], "-"))
    {
        input_type = "raw";
    }
    
    if (!strcmp(input_type, "hw")) {
        if ((argc - optind) >= 1)
            input_sound(sample_rate, overlap, argv[optind]);
        else
            input_sound(sample_rate, overlap, NULL);
        quit();
        exit(0);
    }
    if ((argc - optind) < 1) {
        (void)fprintf(stderr, "no source files specified\n");
        exit(4);
    }
    
    for (i = optind; i < argc; i++)
        input_file(sample_rate, overlap, argv[i], input_type);
    
    quit();
    exit(0);
}

/* ---------------------------------------------------------------------- */
