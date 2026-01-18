/*
 *      unixinput.c -- input sound samples
 *
 *      Copyright (C) 1996
 *          Thomas Sailer (sailer@ife.ee.ethz.ch, hb9jnx@hb9w.che.eu)
 *
 *      Copyright (C) 2012-2026
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

/* MinGW compatibility for timespec_get */
#if defined(__MINGW32__) || defined(__MINGW64__)
#include <windows.h>

#ifndef TIME_UTC
#define TIME_UTC 1
#endif

/* Provide timespec_get for MinGW if not available */
#ifndef timespec_get
static inline int timespec_get(struct timespec *ts, int base)
{
    if (base != TIME_UTC) return 0;
    
    /* Get current time using Windows-specific functions */
    FILETIME ft;
    ULARGE_INTEGER ui;
    
    GetSystemTimeAsFileTime(&ft);
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    
    /* Convert from 100-nanosecond intervals since 1601-01-01 to Unix epoch */
    const uint64_t EPOCH_DIFF = 116444736000000000ULL;
    uint64_t tmp = ui.QuadPart - EPOCH_DIFF;
    
    ts->tv_sec = (time_t)(tmp / 10000000ULL);
    ts->tv_nsec = (long)((tmp % 10000000ULL) * 100);
    
    return TIME_UTC;
}
#endif

/* MinGW doesn't support %F and %T format specifiers for strftime */
#define ISO8601_FORMAT "%Y-%m-%dT%H:%M:%S"
#else
#define ISO8601_FORMAT "%FT%T"
#endif

#ifdef SUN_AUDIO
#include <sys/audioio.h>
#include <stropts.h>
#include <sys/conf.h>
#elif PULSE_AUDIO
#include <pulse/simple.h>
#include <pulse/error.h>
#elif COREAUDIO
#include <AudioToolbox/AudioQueue.h>
#include <CoreAudio/CoreAudio.h>
#ifdef HAS_PROCESSTAP
/* External functions from macos_audio.m for system audio capture */
extern int macos_screencapture_available(void);
extern int macos_start_system_audio(unsigned int sample_rate, void (*callback)(short *, int));
extern void macos_stop_system_audio(void);
extern void macos_audio_run_loop(double seconds);
extern void macos_set_quiet(int quiet);
#endif
#elif WIN32_AUDIO
//see win32_soundin.c
#elif DUMMY_AUDIO
// NO AUDIO
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

/* Extension to type mapping (extensions that differ from type name) */
static const struct { const char *ext; const char *type; } ext_map[] = {
    { "aif", "aiff" },
    { NULL, NULL }
};

/* Detect input type from filename extension, returns NULL if unknown */
static const char *detect_type_from_extension(const char *fname)
{
    if (!fname || !strcmp(fname, "-"))
        return NULL;

    const char *dot = strrchr(fname, '.');
    if (!dot || dot == fname)
        return NULL;

    const char *ext = dot + 1;
    size_t ext_len = strlen(ext);
    if (ext_len == 0 || ext_len > 8)
        return NULL;

    /* Convert extension to lowercase for comparison */
    char ext_lower[16];
    for (size_t i = 0; i <= ext_len && i < sizeof(ext_lower) - 1; i++)
        ext_lower[i] = (ext[i] >= 'A' && ext[i] <= 'Z') ? ext[i] + 32 : ext[i];

    /* Check extension map first (for aliases like .aif -> aiff) */
    for (int i = 0; ext_map[i].ext; i++) {
        if (!strcmp(ext_lower, ext_map[i].ext))
            return ext_map[i].type;
    }

    /* Check if extension matches a known type directly */
    for (const char **t = allowed_types; *t; t++) {
        if (!strcmp(ext_lower, *t))
            return *t;
    }

    return NULL;
}

/* Check if sox is available, returns 1 if found, 0 if not */
static int check_sox_available(void)
{
#ifdef WIN32
    /* On Windows, just try to run it - harder to check PATH reliably */
    return 1;
#else
    /* Check common locations and PATH */
    if (access("/usr/bin/sox", X_OK) == 0) return 1;
    if (access("/usr/local/bin/sox", X_OK) == 0) return 1;
    if (access("/opt/homebrew/bin/sox", X_OK) == 0) return 1;

    /* Check PATH using which */
    int ret = system("which sox >/dev/null 2>&1");
    return (ret == 0);
#endif
}

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
static int repeatable_sox = 1;  /* Always use sox -R for deterministic output */
static int mute_sox = 0;
static int type_explicit = 0;   /* Track if -t was explicitly provided */
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
extern int pocsag_polarity;
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
                strftime(time_buf, sizeof time_buf, ISO8601_FORMAT, gmtime(&ts.tv_sec)); //2024-09-13T20:35:30
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

void addJsonTimestamp(cJSON *json_output)
{
    if (json_output==NULL) return;
    if (!timestamp) return;

    char json_temp[100];
    if(iso8601)
    {
        struct timespec ts;
        timespec_get(&ts, TIME_UTC);
        strftime(json_temp, sizeof json_temp, ISO8601_FORMAT, gmtime(&ts.tv_sec)); //2024-09-13T20:35:30
        snprintf(json_temp + strlen(json_temp), sizeof json_temp - strlen(json_temp), ".%06ld", ts.tv_nsec/1000); //2024-09-13T20:35:30.156337
    }
    else
    {
        time_t t;
        struct tm* tm_info;
        t = time(NULL);
        tm_info = localtime(&t);
        strftime(json_temp, sizeof(json_temp), "%Y-%m-%d %H:%M:%S", tm_info);
    }
    cJSON_AddStringToObject(json_output, "timestamp", json_temp);

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
#elif COREAUDIO
/* macOS Core Audio implementation using AudioQueue */

/* Shared state for audio queue callback */
static struct {
    short *buffer;
    unsigned int buffer_size;
    volatile int data_ready;
    volatile int running;
} ca_state;

static void ca_input_callback(void *userdata, AudioQueueRef queue,
                              AudioQueueBufferRef buf,
                              const AudioTimeStamp *start_time,
                              UInt32 num_packets,
                              const AudioStreamPacketDescription *desc)
{
    (void)userdata;
    (void)start_time;
    (void)desc;
    
    if (num_packets > 0 && ca_state.running) {
        unsigned int bytes = num_packets * sizeof(short);
        if (bytes > ca_state.buffer_size)
            bytes = ca_state.buffer_size;
        memcpy(ca_state.buffer, buf->mAudioData, bytes);
        ca_state.data_ready = bytes / sizeof(short);
    }
    
    /* Re-enqueue the buffer for continuous recording */
    if (ca_state.running)
        AudioQueueEnqueueBuffer(queue, buf, 0, NULL);
}

static void input_sound(unsigned int sample_rate, unsigned int overlap,
                        const char *ifname)
{
    AudioQueueRef queue = NULL;
    AudioQueueBufferRef buffers[3];
    OSStatus status;
    
    short buffer[8192];
    float fbuf[16384];
    unsigned int fbuf_cnt = 0;
    short *sp;
    int i;
    
    (void)ifname;  /* Device selection not implemented yet */
    
    /* Set up audio format: 16-bit signed integer, mono */
    AudioStreamBasicDescription format = {
        .mSampleRate = sample_rate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
        .mBytesPerPacket = sizeof(short),
        .mFramesPerPacket = 1,
        .mBytesPerFrame = sizeof(short),
        .mChannelsPerFrame = 1,
        .mBitsPerChannel = 16,
        .mReserved = 0
    };
    
    /* Initialize shared state */
    ca_state.buffer = buffer;
    ca_state.buffer_size = sizeof(buffer);
    ca_state.data_ready = 0;
    ca_state.running = 1;
    
    /* Create audio input queue */
    status = AudioQueueNewInput(&format, ca_input_callback, NULL,
                                CFRunLoopGetCurrent(), kCFRunLoopCommonModes,
                                0, &queue);
    if (status != noErr) {
        fprintf(stderr, "AudioQueueNewInput failed: %d\n", (int)status);
        exit(10);
    }
    
    /* Allocate and enqueue buffers */
    const unsigned int buf_size = sizeof(buffer);
    for (int b = 0; b < 3; b++) {
        status = AudioQueueAllocateBuffer(queue, buf_size, &buffers[b]);
        if (status != noErr) {
            fprintf(stderr, "AudioQueueAllocateBuffer failed: %d\n", (int)status);
            exit(10);
        }
        AudioQueueEnqueueBuffer(queue, buffers[b], 0, NULL);
    }
    
    /* Start recording */
    status = AudioQueueStart(queue, NULL);
    if (status != noErr) {
        fprintf(stderr, "AudioQueueStart failed: %d\n", (int)status);
        exit(10);
    }
    
    fprintf(stdout, "Core Audio: recording at %u Hz\n", sample_rate);
    
    /* Main loop: process audio from callback */
    for (;;) {
        /* Run the run loop to process callbacks */
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
        
        if (ca_state.data_ready > 0) {
            i = ca_state.data_ready;
            sp = buffer;
            ca_state.data_ready = 0;
            
            if (integer_only) {
                fbuf_cnt = i;
            } else {
                for (; i > 0; i--, sp++)
                    fbuf[fbuf_cnt++] = (*sp) * (1.0f / 32768.0f);
            }
            
            if (fbuf_cnt > overlap) {
                process_buffer(fbuf, buffer, fbuf_cnt - overlap);
                memmove(fbuf, fbuf + fbuf_cnt - overlap, overlap * sizeof(fbuf[0]));
                fbuf_cnt = overlap;
            }
        }
    }
    
    /* Cleanup (unreachable in normal operation, but good practice) */
    ca_state.running = 0;
    AudioQueueStop(queue, true);
    AudioQueueDispose(queue, true);
}

#ifdef HAS_PROCESSTAP
/* System audio capture using Core Audio ProcessTap (macOS 14.2+) */
static unsigned int sck_overlap;
static float sck_fbuf[16384];
static short sck_sbuf[8192];
static unsigned int sck_fbuf_cnt = 0;

static void sck_audio_callback(short *samples, int count)
{
    if (count <= 0)
        return;
    
    /* Copy samples to our buffer */
    if (count > 8192)
        count = 8192;
    memcpy(sck_sbuf, samples, count * sizeof(short));
    
    if (integer_only) {
        sck_fbuf_cnt = count;
    } else {
        for (int i = 0; i < count; i++)
            sck_fbuf[sck_fbuf_cnt++] = samples[i] * (1.0f / 32768.0f);
    }
    
    if (sck_fbuf_cnt > sck_overlap) {
        process_buffer(sck_fbuf, sck_sbuf, sck_fbuf_cnt - sck_overlap);
        memmove(sck_fbuf, sck_fbuf + sck_fbuf_cnt - sck_overlap, sck_overlap * sizeof(sck_fbuf[0]));
        sck_fbuf_cnt = sck_overlap;
    }
}

static void input_system_audio(unsigned int sample_rate, unsigned int overlap)
{
    sck_overlap = overlap;
    sck_fbuf_cnt = 0;
    
    if (!macos_screencapture_available()) {
        fprintf(stderr, "System audio capture requires macOS 14.2 or later\n");
        exit(10);
    }
    
    if (macos_start_system_audio(sample_rate, sck_audio_callback) != 0) {
        fprintf(stderr, "Failed to start system audio capture\n");
        fprintf(stderr, "Make sure to grant screen recording permission in System Preferences\n");
        exit(10);
    }
    
    /* Keep running - the callback handles audio processing */
    for (;;) {
        macos_audio_run_loop(0.1);
    }
    
    macos_stop_system_audio();
}
#endif /* HAS_PROCESSTAP */
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
        "  -t <type>    : Input file type (auto-detected from extension if not specified)\n"
        "                 Types other than raw require sox. Supported: hw (hardware input),\n"
#ifdef HAS_PROCESSTAP
        "                 system (capture system audio output, macOS 14.2+),\n"
#endif
        "                 raw, wav, flac, mp3, ogg, aiff, au, etc.\n"
        "  -a <demod>   : Add demodulator\n"
        "  -s <demod>   : Subtract demodulator\n"
        "  -c           : Remove all demodulators (must be added with -a <demod>)\n"
        "  -q           : Quiet\n"
        "  -v <level>   : Level of verbosity (e.g. '-v 3')\n"
        "                 For POCSAG and MORSE_CW '-v1' prints decoding statistics.\n"
        "  -h           : This help\n"
        "  -A           : APRS mode (TNC2 text output)\n"
        "  -m           : Mute SoX warnings\n"
        "  -r           : (Deprecated) Repeatable mode is now always enabled.\n"
        "  -n           : Don't flush stdout, increases performance.\n"
        "  -j           : FMS: Just output hex data and CRC, no parsing.\n"
        "  -e           : POCSAG: Hide empty messages.\n"
        "  -u           : POCSAG: Heuristically prune unlikely decodes.\n"
        "  -i           : POCSAG: (Deprecated) Polarity is now auto-detected.\n"
        "  -p           : POCSAG: Show partially received messages.\n"
        "  -P <mode>    : POCSAG: Polarity (auto/normal/inverted, default: auto).\n"
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
#ifdef HAS_PROCESSTAP
    char *input_type = "system";  /* Default to system audio capture on macOS */
#else
    char *input_type = "hw";
#endif

    static struct option long_options[] =
      {
        {"timestamp", no_argument, &timestamp, 1},
        {"flex-no-ts", no_argument, &flex_disable_timestamp, 1},
        {"iso8601", no_argument, &iso8601, 1},
        {"label", required_argument, NULL, 'l'},
        {"charset", required_argument, NULL, 'C'},
        {"json", no_argument, &json_mode, 1},
        {"pocsag-polarity", required_argument, NULL, 'P'},
        {0, 0, 0, 0}
      };

    while ((c = getopt_long(argc, argv, "t:a:s:v:f:b:C:o:d:g:P:cqhAmrnjeuipxy", long_options, NULL)) != EOF) {
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

        case 'P':
            if (!strcmp(optarg, "auto") || !strcmp(optarg, "0"))
                pocsag_polarity = 0;
            else if (!strcmp(optarg, "normal") || !strcmp(optarg, "1"))
                pocsag_polarity = 1;
            else if (!strcmp(optarg, "inverted") || !strcmp(optarg, "2"))
                pocsag_polarity = 2;
            else {
                fprintf(stderr, "Invalid POCSAG polarity: %s (use auto/normal/inverted)\n", optarg);
                errflg++;
            }
            break;
            
        case 'm':
            mute_sox = 1;
            break;

        case 'j':
            fms_justhex = true;
            break;
            
        case 'r':
            fprintf(stderr, "Warning: -r is deprecated. Repeatable mode is now always enabled.\n");
            break;
            
        case 't':
            type_explicit = 1;
            /* Special case: -t hw for explicit hardware input */
            if (!strcmp(optarg, "hw")) {
                input_type = "hw";
                break;
            }
#ifdef HAS_PROCESSTAP
            /* Special case: -t system for system audio capture */
            if (!strcmp(optarg, "system")) {
                input_type = "system";
                break;
            }
#endif
            for (itype = (char **)allowed_types; *itype; itype++)
                if (!strcmp(*itype, optarg)) {
                    input_type = *itype;
                    goto intypefound;
                }
            fprintf(stderr, "invalid input type \"%s\"\n"
                    "allowed types: hw ", optarg);
#ifdef HAS_PROCESSTAP
            fprintf(stderr, "system ");
#endif
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
            fprintf(stderr, "Warning: -i is deprecated. POCSAG polarity is now auto-detected.\n");
            /* Flag kept for backwards compatibility but now ignored */
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
        fprintf(stderr, "multimon-ng 1.5.0\n"
            "  (C) 1996/1997 by Tom Sailer HB9JNX/AE4WA\n"
            "  (C) 2012-2026 by Elias Oenal\n"
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
        type_explicit = 1;  /* stdin requires explicit raw */
    }
    
    /* If no explicit type was set, check if we should auto-detect from files */
    if ((!strcmp(input_type, "hw") || !strcmp(input_type, "system")) && !type_explicit && (argc - optind) >= 1) {
        /* Check if the argument looks like a device path (starts with /dev/) */
        const char *first_arg = argv[optind];
        if (strncmp(first_arg, "/dev/", 5) == 0) {
            /* Device path - switch to hw mode */
            input_type = "hw";
        } else {
            /* Regular file - switch to file mode with auto-detection */
            input_type = NULL;
        }
    }
    
#ifdef HAS_PROCESSTAP
    if (input_type && !strcmp(input_type, "system")) {
        macos_set_quiet(quietflg);
        input_system_audio(sample_rate, overlap);
        quit();
        exit(0);
    }
#endif
    
    if (input_type && !strcmp(input_type, "hw")) {
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
    
    for (i = optind; i < argc; i++) {
        const char *file_type = input_type;
        
        /* Auto-detect type from extension if -t wasn't specified */
        if (!type_explicit) {
            const char *detected = detect_type_from_extension(argv[i]);
            if (detected) {
                file_type = detected;
            } else {
                /* Unknown extension, default to raw with warning */
                if (!quietflg) {
                    const char *dot = strrchr(argv[i], '.');
                    if (dot && dot[1]) {
                        fprintf(stderr, "Warning: Unknown extension '%s', assuming raw. Use -t to specify type.\n", dot);
                    }
                }
                file_type = "raw";
            }
        }
        
        /* Check sox availability for non-raw types */
        if (strcmp(file_type, "raw") != 0 && !check_sox_available()) {
            fprintf(stderr, "Error: sox is required for .%s files but was not found.\n", file_type);
            fprintf(stderr, "Install sox or convert manually:\n");
            fprintf(stderr, "  sox -R -t %s '%s' -esigned-integer -b16 -r %d -t raw output.raw\n",
                    file_type, argv[i], sample_rate);
            exit(10);
        }
        
        input_file(sample_rate, overlap, argv[i], file_type);
    }
    
    quit();
    exit(0);
}

/* ---------------------------------------------------------------------- */
