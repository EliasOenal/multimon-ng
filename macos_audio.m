/*
 *      macos_audio.m -- macOS system audio capture via Core Audio ProcessTap
 *
 *      Copyright (C) 2025
 *          Elias Oenal (multimon-ng@eliasoenal.com)
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

#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Buffer for passing audio data to the C processing code */
#define AUDIO_BUFFER_SIZE 32768
static short g_audio_buffer[AUDIO_BUFFER_SIZE];
static volatile int g_audio_running = 0;

/* Callback function pointer - set by the C code */
typedef void (*audio_callback_t)(short *samples, int count);
static audio_callback_t g_audio_callback = NULL;

/* Core Audio state */
static AudioObjectID g_tap_id = kAudioObjectUnknown;
static AudioObjectID g_aggregate_id = kAudioObjectUnknown;
static AudioDeviceIOProcID g_io_proc_id = NULL;
static unsigned int g_input_channels = 0;
static double g_input_sample_rate = 0;
static unsigned int g_output_sample_rate = 22050;

/* Resampling state */
static double g_resample_pos = 0.0;

/* Check if ProcessTap API is available (macOS 14.2+) */
int macos_screencapture_available(void) {
    if (@available(macOS 14.2, *)) {
        return 1;
    }
    return 0;
}

/* Helper: convert float to int16 */
static inline int16_t float_to_s16(float x) {
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return (int16_t)(x * 32767.0f);
}

/* IOProc callback - called from Core Audio's realtime thread */
static OSStatus io_proc(AudioDeviceID inDevice,
                        const AudioTimeStamp *inNow,
                        const AudioBufferList *inInputData,
                        const AudioTimeStamp *inInputTime,
                        AudioBufferList *outOutputData,
                        const AudioTimeStamp *inOutputTime,
                        void *inClientData)
{
    (void)inDevice; (void)inNow; (void)inInputTime;
    (void)outOutputData; (void)inOutputTime; (void)inClientData;
    
    if (!g_audio_running || !inInputData || inInputData->mNumberBuffers == 0)
        return noErr;
    
    /* Process first buffer - assume float32 */
    const AudioBuffer *buf = &inInputData->mBuffers[0];
    if (!buf->mData || buf->mDataByteSize == 0)
        return noErr;
    
    /* First: mix to mono float buffer */
    static float mono_buffer[AUDIO_BUFFER_SIZE];
    size_t input_frames;
    
    if (inInputData->mNumberBuffers > 1 && g_input_channels >= 2) {
        /* Non-interleaved: each buffer is one channel */
        const AudioBuffer *left = &inInputData->mBuffers[0];
        const AudioBuffer *right = &inInputData->mBuffers[1];
        const float *leftData = (const float *)left->mData;
        const float *rightData = (const float *)right->mData;
        input_frames = left->mDataByteSize / sizeof(float);
        
        if (input_frames > AUDIO_BUFFER_SIZE)
            input_frames = AUDIO_BUFFER_SIZE;
        
        for (size_t i = 0; i < input_frames; i++) {
            mono_buffer[i] = (leftData[i] + rightData[i]) * 0.5f;
        }
    } else {
        /* Interleaved or mono */
        const float *floatData = (const float *)buf->mData;
        size_t totalFloats = buf->mDataByteSize / sizeof(float);
        
        if (g_input_channels >= 2) {
            /* Interleaved stereo: mix to mono */
            input_frames = totalFloats / g_input_channels;
            if (input_frames > AUDIO_BUFFER_SIZE)
                input_frames = AUDIO_BUFFER_SIZE;
            
            for (size_t i = 0; i < input_frames; i++) {
                float mono = 0.0f;
                for (unsigned int ch = 0; ch < g_input_channels; ch++) {
                    mono += floatData[i * g_input_channels + ch];
                }
                mono_buffer[i] = mono / g_input_channels;
            }
        } else {
            /* Mono */
            input_frames = totalFloats;
            if (input_frames > AUDIO_BUFFER_SIZE)
                input_frames = AUDIO_BUFFER_SIZE;
            
            memcpy(mono_buffer, floatData, input_frames * sizeof(float));
        }
    }
    
    /* Now resample from input_sample_rate to output_sample_rate */
    double ratio = g_input_sample_rate / (double)g_output_sample_rate;
    int output_samples = 0;
    
    while (g_resample_pos < (double)input_frames && output_samples < AUDIO_BUFFER_SIZE) {
        size_t idx = (size_t)g_resample_pos;
        double frac = g_resample_pos - (double)idx;
        
        /* Linear interpolation */
        float sample;
        if (idx + 1 < input_frames) {
            sample = mono_buffer[idx] * (1.0f - (float)frac) + mono_buffer[idx + 1] * (float)frac;
        } else if (idx < input_frames) {
            sample = mono_buffer[idx];
        } else {
            break;
        }
        
        g_audio_buffer[output_samples++] = float_to_s16(sample);
        g_resample_pos += ratio;
    }
    
    /* Keep fractional position for next callback, reset integer part */
    g_resample_pos -= (double)input_frames;
    if (g_resample_pos < 0) g_resample_pos = 0;
    
    if (g_audio_callback && output_samples > 0) {
        g_audio_callback(g_audio_buffer, output_samples);
    }
    
    return noErr;
}

/* Get tap UID as CFString */
static CFStringRef copy_tap_uid(AudioObjectID tap) {
    CFStringRef uid = NULL;
    UInt32 sz = sizeof(uid);
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioTapPropertyUID,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain
    };
    OSStatus st = AudioObjectGetPropertyData(tap, &addr, 0, NULL, &sz, &uid);
    return (st == noErr) ? uid : NULL;
}

/* Start system audio capture using Core Audio ProcessTap (macOS 14.2+) */
int macos_start_system_audio(unsigned int sample_rate, audio_callback_t callback) {
    if (@available(macOS 14.2, *)) {
        OSStatus status;
        
        g_audio_callback = callback;
        g_audio_running = 1;
        g_output_sample_rate = sample_rate;
        g_resample_pos = 0.0;
        
        @autoreleasepool {
            /* Create a tap description for all system audio */
            CATapDescription *tapDesc = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
            tapDesc.name = @"multimon-ng system audio tap";
            tapDesc.privateTap = YES;
            tapDesc.muteBehavior = CATapUnmuted;
            tapDesc.exclusive = YES;
            
            /* Create the process tap */
            status = AudioHardwareCreateProcessTap(tapDesc, &g_tap_id);
            if (status != noErr) {
                fprintf(stderr, "AudioHardwareCreateProcessTap failed: %d\n", (int)status);
                fprintf(stderr, "Make sure to grant 'System Audio Recording' permission in System Settings\n");
                return -1;
            }
            
            /* Get tap format */
            AudioStreamBasicDescription tapFormat;
            UInt32 propSize = sizeof(tapFormat);
            AudioObjectPropertyAddress formatAddr = {
                kAudioTapPropertyFormat,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            
            status = AudioObjectGetPropertyData(g_tap_id, &formatAddr, 0, NULL, &propSize, &tapFormat);
            if (status != noErr) {
                fprintf(stderr, "Failed to get tap format: %d\n", (int)status);
                AudioHardwareDestroyProcessTap(g_tap_id);
                g_tap_id = kAudioObjectUnknown;
                return -1;
            }
            
            g_input_channels = tapFormat.mChannelsPerFrame;
            g_input_sample_rate = tapFormat.mSampleRate;
            
            fprintf(stdout, "Tap: %.0f Hz %dch -> resampling to %u Hz mono\n", 
                    g_input_sample_rate, g_input_channels, g_output_sample_rate);
            
            /* Get tap UID for aggregate device */
            CFStringRef tapUID = copy_tap_uid(g_tap_id);
            if (!tapUID) {
                fprintf(stderr, "Failed to get tap UID\n");
                AudioHardwareDestroyProcessTap(g_tap_id);
                g_tap_id = kAudioObjectUnknown;
                return -1;
            }
            
            /* Create unique aggregate device UID */
            NSString *aggUID = [NSString stringWithFormat:@"com.multimon-ng.agg.%d.%llu",
                               getpid(), (unsigned long long)[[NSDate date] timeIntervalSince1970]];
            
            /* Build tap entry for aggregate device */
            int32_t one = 1;
            CFNumberRef oneNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &one);
            
            CFMutableDictionaryRef tapEntry = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(tapEntry, CFSTR(kAudioSubTapUIDKey), tapUID);
            CFDictionarySetValue(tapEntry, CFSTR(kAudioSubTapDriftCompensationKey), oneNum);
            
            const void *tapEntries[1] = { tapEntry };
            CFArrayRef taps = CFArrayCreate(kCFAllocatorDefault, tapEntries, 1, &kCFTypeArrayCallBacks);
            
            /* Build aggregate device properties */
            CFMutableDictionaryRef props = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(props, CFSTR(kAudioAggregateDeviceNameKey), CFSTR("multimon-ng aggregate"));
            CFDictionarySetValue(props, CFSTR(kAudioAggregateDeviceUIDKey), (__bridge CFStringRef)aggUID);
            CFDictionarySetValue(props, CFSTR(kAudioAggregateDeviceTapListKey), taps);
            CFDictionarySetValue(props, CFSTR(kAudioAggregateDeviceTapAutoStartKey), oneNum);
            CFDictionarySetValue(props, CFSTR(kAudioAggregateDeviceIsPrivateKey), oneNum);
            
            /* Create aggregate device */
            status = AudioHardwareCreateAggregateDevice(props, &g_aggregate_id);
            
            CFRelease(props);
            CFRelease(taps);
            CFRelease(tapEntry);
            CFRelease(oneNum);
            CFRelease(tapUID);
            
            if (status != noErr) {
                fprintf(stderr, "AudioHardwareCreateAggregateDevice failed: %d\n", (int)status);
                AudioHardwareDestroyProcessTap(g_tap_id);
                g_tap_id = kAudioObjectUnknown;
                return -1;
            }
            
            /* Create IOProc for the aggregate device */
            status = AudioDeviceCreateIOProcID(g_aggregate_id, io_proc, NULL, &g_io_proc_id);
            if (status != noErr || !g_io_proc_id) {
                fprintf(stderr, "AudioDeviceCreateIOProcID failed: %d\n", (int)status);
                AudioHardwareDestroyAggregateDevice(g_aggregate_id);
                AudioHardwareDestroyProcessTap(g_tap_id);
                g_aggregate_id = kAudioObjectUnknown;
                g_tap_id = kAudioObjectUnknown;
                return -1;
            }
            
            /* Start the device */
            status = AudioDeviceStart(g_aggregate_id, g_io_proc_id);
            if (status != noErr) {
                fprintf(stderr, "AudioDeviceStart failed: %d\n", (int)status);
                AudioDeviceDestroyIOProcID(g_aggregate_id, g_io_proc_id);
                AudioHardwareDestroyAggregateDevice(g_aggregate_id);
                AudioHardwareDestroyProcessTap(g_tap_id);
                g_io_proc_id = NULL;
                g_aggregate_id = kAudioObjectUnknown;
                g_tap_id = kAudioObjectUnknown;
                return -1;
            }
            
            fprintf(stdout, "System audio capture started\n");
        }
        
        return 0;
    }
    
    fprintf(stderr, "System audio capture requires macOS 14.2 or later\n");
    return -1;
}

void macos_stop_system_audio(void) {
    g_audio_running = 0;
    
    if (g_io_proc_id && g_aggregate_id != kAudioObjectUnknown) {
        AudioDeviceStop(g_aggregate_id, g_io_proc_id);
        AudioDeviceDestroyIOProcID(g_aggregate_id, g_io_proc_id);
        g_io_proc_id = NULL;
    }
    
    if (g_aggregate_id != kAudioObjectUnknown) {
        AudioHardwareDestroyAggregateDevice(g_aggregate_id);
        g_aggregate_id = kAudioObjectUnknown;
    }
    
    if (@available(macOS 14.2, *)) {
        if (g_tap_id != kAudioObjectUnknown) {
            AudioHardwareDestroyProcessTap(g_tap_id);
            g_tap_id = kAudioObjectUnknown;
        }
    }
    
    g_audio_callback = NULL;
}

/* Run loop for the main thread */
void macos_audio_run_loop(double seconds) {
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}
