/*
 *      unixinput.c -- input sound samples
 *
 *      Copyright (C) 1996
 *          Thomas Sailer (sailer@ife.ee.ethz.ch, hb9jnx@hb9w.che.eu)
 *
 *      Copyright (C) 2012
 *          Elias Oenal    (EliasOenal@gmail.com)
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
#ifndef _MSC_VER
#include <unistd.h>
#endif
#include <errno.h>
#include <string.h>
#include <stdlib.h>

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

/* ---------------------------------------------------------------------- */

static const char *allowed_types[] = {
    "raw", "aiff", "au", "hcom", "sf", "voc", "cdr", "dat",
    "smp", "wav", "maud", "vwe", NULL
};

/* ---------------------------------------------------------------------- */

static const struct demod_param *dem[] = { ALL_DEMOD };

#define NUMDEMOD (sizeof(dem)/sizeof(dem[0]))

static struct demod_state dem_st[NUMDEMOD];
static unsigned int dem_mask[(NUMDEMOD+31)/32];

#define MASK_SET(n) dem_mask[(n)>>5] |= 1<<((n)&0x1f)
#define MASK_RESET(n) dem_mask[(n)>>5] &= ~(1<<((n)&0x1f))
#define MASK_ISSET(n) (dem_mask[(n)>>5] & 1<<((n)&0x1f))

/* ---------------------------------------------------------------------- */

static int verbose_level = 0;
static int repeatable_sox = 0;
static int mute_sox = 0;

extern int pocsag_mode;
extern int aprs_mode;
void quit(void);

/* ---------------------------------------------------------------------- */

void _verbprintf(int verb_level, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    if (verb_level <= verbose_level) {
        vfprintf(stdout, fmt, args);
        fflush(stdout);
    }
    va_end(args);
}

/* ---------------------------------------------------------------------- */

void process_buffer(float *buf, unsigned int len)
{
    int i;

    for (i = 0; i <  NUMDEMOD; i++)
        if (MASK_ISSET(i) && dem[i]->demod)
            dem[i]->demod(dem_st+i, buf, len);
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
            for (; i >= sizeof(buffer[0]); i -= sizeof(buffer[0]), sp++)
                fbuf[fbuf_cnt++] = (*sp) * (1.0/32768.0);
            if (i)
                fprintf(stderr, "warning: noninteger number of samples read\n");
            if (fbuf_cnt > overlap) {
                process_buffer(fbuf, fbuf_cnt-overlap);
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
    //printf("DUMMY SOUND IN!");
    //fflush(stdout);
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

    // Init stuff from pa.org
    pa_simple *s;
    pa_sample_spec ss;

    ss.format = PA_SAMPLE_S16NE;
    ss.channels = 1;
    ss.rate = sample_rate;


    /* Create the recording stream */
    if (!(s = pa_simple_new(NULL, "multimon-ng", PA_STREAM_RECORD, NULL, "record", &ss, NULL, NULL, &error))) {
        fprintf(stderr, __FILE__": pa_simple_new() failed: %s\n", pa_strerror(error));
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
            for (; i >= sizeof(buffer[0]); i -= sizeof(buffer[0]), sp++)
                fbuf[fbuf_cnt++] = (*sp) * (1.0/32768.0);
            if (i)
                fprintf(stderr, "warning: noninteger number of samples read\n");
            if (fbuf_cnt > overlap) {
                process_buffer(fbuf, fbuf_cnt-overlap);
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
            i = read(fd, bp = b.b, sizeof(b.b));
            if (i < 0 && errno != EAGAIN) {
                perror("read");
                exit(4);
            }
            if (!i)
                break;
            if (i > 0) {
                for (; i >= sizeof(b.b[0]); i -= sizeof(b.b[0]), sp++)
                    fbuf[fbuf_cnt++] = ((int)(*bp)-0x80) * (1.0/128.0);
                if (i)
                    fprintf(stderr, "warning: noninteger number of samples read\n");
                if (fbuf_cnt > overlap) {
                    process_buffer(fbuf, fbuf_cnt-overlap);
                    memmove(fbuf, fbuf+fbuf_cnt-overlap, overlap*sizeof(fbuf[0]));
                    fbuf_cnt = overlap;
                }
            }
        } else {
            i = read(fd, sp = b.s, sizeof(b.s));
            if (i < 0 && errno != EAGAIN) {
                perror("read");
                exit(4);
            }
            if (!i)
                break;
            if (i > 0) {
                for (; i >= sizeof(b.s[0]); i -= sizeof(b.s[0]), sp++)
                    fbuf[fbuf_cnt++] = (*sp) * (1.0/32768.0);
                if (i)
                    fprintf(stderr, "warning: noninteger number of samples read\n");
                if (fbuf_cnt > overlap) {
                    process_buffer(fbuf, fbuf_cnt-overlap);
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
            for (; i >= sizeof(buffer[0]); i -= sizeof(buffer[0]), sp++)
                fbuf[fbuf_cnt++] = (*sp) * (1.0f/32768.0f);
            if (i)
                fprintf(stderr, "warning: noninteger number of samples read\n");
            if (fbuf_cnt > overlap) {
                process_buffer(fbuf, fbuf_cnt-overlap);
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
    for (i = 0; i < NUMDEMOD; i++)
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
        "  -t <type>  : input file type (any other type than raw requires sox)\n"
        "  -a <demod> : add demodulator\n"
        "  -s <demod> : subtract demodulator\n"
        "  -c         : remove all demodulators (must be added with -a <demod>)\n"
        "  -q         : quiet\n"
        "  -v <level> : level of verbosity (for example '-v 10')\n"
        "  -f <mode>  : forces POCSAG data decoding as <mode> (<mode> can be 'numeric', 'alpha' and 'skyper')\n"
        "  -h         : this help\n"
        "  -A         : APRS mode (TNC2 text output)\n"
        "  -m         : mute SoX warnings\n"
        "  -r         : call SoX in repeatable mode (e.g. random seed for dithering)\n"
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

    fprintf(stderr, "multimon-ng  (C) 1996/1997 by Tom Sailer HB9JNX/AE4WA\n"
            "             (C) 2012/2013 by Elias Oenal\n"
            "available demodulators:");
    for (i = 0; i < NUMDEMOD; i++)
        fprintf(stderr, " %s", dem[i]->name);
    fprintf(stderr, "\n");
    while ((c = getopt(argc, argv, "t:a:s:v:f:cqhAmr")) != EOF) {
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
            for (i = 0; i < NUMDEMOD; i++)
                if (!strcasecmp("AFSK1200", dem[i]->name)) {
                    MASK_SET(i);
                    break;
                }
            break;

        case 'v':
            verbose_level = strtoul(optarg, 0, 0);
            break;

        case 'm':
            mute_sox = 1;
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
            for (i = 0; i < NUMDEMOD; i++)
                if (!strcasecmp(optarg, dem[i]->name)) {
                    MASK_SET(i);
                    break;
                }
            if (i >= NUMDEMOD) {
                fprintf(stderr, "invalid mode \"%s\"\n", optarg);
                errflg++;
            }
            break;

        case 's':
            if (mask_first)
                memset(dem_mask, 0xff, sizeof(dem_mask));
            mask_first = 0;
            for (i = 0; i < NUMDEMOD; i++)
                if (!strcasecmp(optarg, dem[i]->name)) {
                    MASK_RESET(i);
                    break;
                }
            if (i >= NUMDEMOD) {
                fprintf(stderr, "invalid mode \"%s\"\n", optarg);
                errflg++;
            }
            break;

        case 'c':
            if (mask_first)
                memset(dem_mask, 0xff, sizeof(dem_mask));
            mask_first = 0;
            for (i = 0; i < NUMDEMOD; i++)
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
            }else fprintf(stderr, "a POCSAG mode has already been selected!\n");
            break;
        }
    }
    if (errflg) {
        (void)fprintf(stderr, usage_str, argv[0]);
        exit(2);
    }
    if (mask_first)
        memset(dem_mask, 0xff, sizeof(dem_mask));

    if (!quietflg)
        fprintf(stdout, "Enabled demodulators:");
    for (i = 0; i < NUMDEMOD; i++)
        if (MASK_ISSET(i)) {
            if (!quietflg)
                fprintf(stdout, " %s", dem[i]->name);
            memset(dem_st+i, 0, sizeof(dem_st[i]));
            dem_st[i].dem_par = dem[i];
            if (dem[i]->init)
                dem[i]->init(dem_st+i);
            if (sample_rate == -1)
                sample_rate = dem[i]->samplerate;
            else if (sample_rate != dem[i]->samplerate) {
                if (!quietflg)
                    fprintf(stdout, "\n");
                fprintf(stderr, "Error: Current sampling rate %d, "
                        " demodulator \"%s\" requires %d\n",
                        sample_rate, dem[i]->name, dem[i]->samplerate);
                exit(3);
            }
            if (dem[i]->overlap > overlap)
                overlap = dem[i]->overlap;
        }
    if (!quietflg)
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
