/*
 * demod_sdl_scope.c -- SDL3 digital phosphor oscilloscope display
 *
 * Elias Oenal (multimon-ng@eliasoenal.com)
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * For more information, please refer to <https://unlicense.org/>
 */

#ifndef NO_SDL3

#include "multimon.h"
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include <pthread.h>
#endif

/* ---------------------------------------------------------------------- */

#define PHOSPHOR_WIDTH  480
#define PHOSPHOR_HEIGHT 270
#define SAMPLING_RATE   22050
#define WINDOW_SECONDS  5.0f

#define SAMPLES_PER_COLUMN ((int)((WINDOW_SECONDS * SAMPLING_RATE) / PHOSPHOR_WIDTH))
#define RING_BUFFER_SIZE (SAMPLING_RATE)

#define MAX_INTENSITY 400.0f
#define TRACE_INTENSITY 80.0f
#define INTENSITY_LUT_SIZE 1024

/* ---------------------------------------------------------------------- */

/* Pre-computed colormaps: green (P32) and amber (P3) phosphor */
static uint32_t colormap_green[256];
static uint32_t colormap_amber[256];
static uint32_t *colormap = NULL;
static uint8_t intensity_lut[INTENSITY_LUT_SIZE];

static void init_colormap(void)
{
    /* Green phosphor (P32) */
    for (int i = 0; i < 256; i++) {
        if (i == 0) {
            colormap_green[i] = 0xFF080808;
            continue;
        }
        float t = powf(i / 255.0f, 0.7f);
        uint8_t g = (uint8_t)(8 + t * 247);
        uint8_t r, b;
        if (t < 0.6f) {
            r = (uint8_t)(8 + t * 20);
            b = (uint8_t)(8 + t * 20);
        } else {
            float s = (t - 0.6f) / 0.4f;
            r = (uint8_t)(20 + s * 130);
            b = (uint8_t)(20 + s * 130);
        }
        colormap_green[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }

    /* Amber phosphor (P3) */
    for (int i = 0; i < 256; i++) {
        if (i == 0) {
            colormap_amber[i] = 0xFF080808;
            continue;
        }
        float t = powf(i / 255.0f, 0.7f);
        uint8_t r = (uint8_t)(8 + t * 247);
        uint8_t g = (uint8_t)(8 + t * 140);
        uint8_t b;
        if (t < 0.6f) {
            b = (uint8_t)(8 + t * 10);
        } else {
            float s = (t - 0.6f) / 0.4f;
            b = (uint8_t)(10 + s * 60);
        }
        colormap_amber[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }

    colormap = colormap_green;

    /* Build intensity to color index LUT (includes sqrt for gamma) */
    for (int i = 0; i < INTENSITY_LUT_SIZE; i++) {
        float val = (float)i * MAX_INTENSITY / (INTENSITY_LUT_SIZE - 1);
        int idx = (int)(sqrtf(val / MAX_INTENSITY) * 255.0f);
        if (idx > 255) idx = 255;
        intensity_lut[i] = (uint8_t)idx;
    }
}

/* ---------------------------------------------------------------------- */

static struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    float (*intensity)[PHOSPHOR_HEIGHT];
    uint32_t *pixels;
    bool dirty[PHOSPHOR_WIDTH];

    float ring[RING_BUFFER_SIZE];
    volatile int ring_write;
    volatile int ring_read;

    int current_x;
    int samples_in_col;
    int prev_y;
    bool has_prev_y;
    uint64_t last_render;
    uint64_t last_decay;

    /* Real-time throttling for file input */
    uint64_t start_time;
    uint64_t samples_processed;

    bool initialized;
    bool running;
    bool sdl_ready;
#ifdef __APPLE__
    dispatch_source_t timer;
#endif
} sdl_state = {0};

/* ---------------------------------------------------------------------- */

/* Process a single sample directly into the intensity array */
static void process_sample(float sample)
{
    if (sample > 1.0f) sample = 1.0f;
    if (sample < -1.0f) sample = -1.0f;

    int y = (int)((1.0f - sample * 0.7f) * 0.5f * (PHOSPHOR_HEIGHT - 1));
    if (y < 0) y = 0;
    if (y >= PHOSPHOR_HEIGHT) y = PHOSPHOR_HEIGHT - 1;

    /* Mark columns as dirty */
    sdl_state.dirty[sdl_state.current_x] = true;
    if (sdl_state.current_x > 0)
        sdl_state.dirty[sdl_state.current_x - 1] = true;
    if (sdl_state.current_x < PHOSPHOR_WIDTH - 1)
        sdl_state.dirty[sdl_state.current_x + 1] = true;

    /* Each sample has a fixed energy budget (TRACE_INTENSITY).
     * This energy is spread across the distance traveled.
     * Short distance = bright, long distance = dim per pixel.
     * Bloom spreads energy to adjacent columns (same total budget). */
    if (sdl_state.has_prev_y) {
        int y0 = sdl_state.prev_y;
        int y1 = y;
        if (y0 > y1) { int tmp = y0; y0 = y1; y1 = tmp; }
        int span = y1 - y0 + 1;
        float per_pixel = TRACE_INTENSITY / (float)span;
        /* Split energy: 50% center, 25% left, 25% right */
        float center = per_pixel * 0.5f;
        float side = per_pixel * 0.25f;
        for (int yy = y0; yy <= y1; yy++) {
            sdl_state.intensity[sdl_state.current_x][yy] += center;
            if (sdl_state.current_x > 0)
                sdl_state.intensity[sdl_state.current_x - 1][yy] += side;
            if (sdl_state.current_x < PHOSPHOR_WIDTH - 1)
                sdl_state.intensity[sdl_state.current_x + 1][yy] += side;
        }
    } else {
        /* Split energy: 50% center, 25% left, 25% right */
        float center = TRACE_INTENSITY * 0.5f;
        float side = TRACE_INTENSITY * 0.25f;
        sdl_state.intensity[sdl_state.current_x][y] += center;
        if (sdl_state.current_x > 0)
            sdl_state.intensity[sdl_state.current_x - 1][y] += side;
        if (sdl_state.current_x < PHOSPHOR_WIDTH - 1)
            sdl_state.intensity[sdl_state.current_x + 1][y] += side;
    }
    sdl_state.prev_y = y;
    sdl_state.has_prev_y = true;

    sdl_state.samples_in_col++;
    if (sdl_state.samples_in_col >= SAMPLES_PER_COLUMN) {
        sdl_state.samples_in_col = 0;
        sdl_state.current_x = (sdl_state.current_x + 1) % PHOSPHOR_WIDTH;
        for (int dy = 0; dy < PHOSPHOR_HEIGHT; dy++)
            sdl_state.intensity[sdl_state.current_x][dy] = 0;
    }
}

/* Render the current intensity array to screen */
static void render_display(void)
{
    if (!sdl_state.sdl_ready) return;

    uint64_t now = SDL_GetTicks();
    float lut_scale = (INTENSITY_LUT_SIZE - 1) / MAX_INTENSITY;

    /* Time-based decay: 0.94 every 50ms (equivalent to every 3 frames at 60fps) */
    uint64_t decay_elapsed = now - sdl_state.last_decay;
    if (decay_elapsed >= 50) {
        /* Calculate how many 50ms periods elapsed and compute cumulative decay */
        int decay_steps = (int)(decay_elapsed / 50);
        float decay = powf(0.94f, (float)decay_steps);
        sdl_state.last_decay = now - (decay_elapsed % 50);  /* Keep remainder */

        /* Decay frame: process all columns */
        for (int x = 0; x < PHOSPHOR_WIDTH; x++) {
            float *col = sdl_state.intensity[x];
            for (int y = 0; y < PHOSPHOR_HEIGHT; y++) {
                float val = col[y] * decay;
                col[y] = val;
                int lut_idx = (int)(val * lut_scale);
                if (lut_idx >= INTENSITY_LUT_SIZE) lut_idx = INTENSITY_LUT_SIZE - 1;
                sdl_state.pixels[y * PHOSPHOR_WIDTH + x] = colormap[intensity_lut[lut_idx]];
            }
            sdl_state.dirty[x] = false;
        }
    } else {
        /* Non-decay frame: only process dirty columns */
        for (int x = 0; x < PHOSPHOR_WIDTH; x++) {
            if (!sdl_state.dirty[x]) continue;
            float *col = sdl_state.intensity[x];
            for (int y = 0; y < PHOSPHOR_HEIGHT; y++) {
                float val = col[y];
                int lut_idx = (int)(val * lut_scale);
                if (lut_idx >= INTENSITY_LUT_SIZE) lut_idx = INTENSITY_LUT_SIZE - 1;
                sdl_state.pixels[y * PHOSPHOR_WIDTH + x] = colormap[intensity_lut[lut_idx]];
            }
            sdl_state.dirty[x] = false;
        }
    }

    /* Position marker */
    for (int y = 0; y < PHOSPHOR_HEIGHT; y++)
        sdl_state.pixels[y * PHOSPHOR_WIDTH + sdl_state.current_x] = 0xFF222222;

    SDL_UpdateTexture(sdl_state.texture, NULL, sdl_state.pixels, PHOSPHOR_WIDTH * sizeof(uint32_t));
    SDL_RenderClear(sdl_state.renderer);
    SDL_RenderTexture(sdl_state.renderer, sdl_state.texture, NULL, NULL);
    SDL_RenderPresent(sdl_state.renderer);
}

static void do_render_frame(void)
{
    if (!sdl_state.sdl_ready) return;

    /* Process events */
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT ||
            (event.type == SDL_EVENT_KEY_DOWN &&
             (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q))) {
            sdl_state.running = false;
            /* Use _exit to avoid hang on macOS with dispatch_async */
            _exit(0);
        }
        /* Toggle phosphor color with space */
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
            colormap = (colormap == colormap_green) ? colormap_amber : colormap_green;
        }
    }

    /* Process buffered samples into intensity array (for async audio input) */
    while (sdl_state.ring_read != sdl_state.ring_write) {
        float sample = sdl_state.ring[sdl_state.ring_read];
        sdl_state.ring_read = (sdl_state.ring_read + 1) % RING_BUFFER_SIZE;
        process_sample(sample);
    }

    /* Rate limit rendering to 60fps */
    uint64_t now = SDL_GetTicks();
    if (now - sdl_state.last_render < 16) return;
    sdl_state.last_render = now;

    render_display();
}

/* ---------------------------------------------------------------------- */

static void sdl_scope_init(struct demod_state *s)
{
    (void)s;

    if (sdl_state.initialized) return;

    init_colormap();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_SCOPE: SDL3 init failed: %s\n", SDL_GetError());
        return;
    }

    sdl_state.window = SDL_CreateWindow("multimon-ng Digital Phosphor Scope",
                                        PHOSPHOR_WIDTH, PHOSPHOR_HEIGHT, 0);
    if (!sdl_state.window) {
        fprintf(stderr, "SDL_SCOPE: Window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return;
    }

    sdl_state.renderer = SDL_CreateRenderer(sdl_state.window, NULL);
    if (!sdl_state.renderer) {
        SDL_DestroyWindow(sdl_state.window);
        SDL_Quit();
        return;
    }

    sdl_state.texture = SDL_CreateTexture(sdl_state.renderer, SDL_PIXELFORMAT_ARGB8888,
                                          SDL_TEXTUREACCESS_STREAMING,
                                          PHOSPHOR_WIDTH, PHOSPHOR_HEIGHT);
    if (!sdl_state.texture) {
        SDL_DestroyRenderer(sdl_state.renderer);
        SDL_DestroyWindow(sdl_state.window);
        SDL_Quit();
        return;
    }

    SDL_SetTextureScaleMode(sdl_state.texture, SDL_SCALEMODE_LINEAR);

    sdl_state.intensity = calloc(PHOSPHOR_WIDTH, sizeof(*sdl_state.intensity));
    sdl_state.pixels = calloc(PHOSPHOR_WIDTH * PHOSPHOR_HEIGHT, sizeof(uint32_t));
    if (!sdl_state.intensity || !sdl_state.pixels) {
        SDL_DestroyTexture(sdl_state.texture);
        SDL_DestroyRenderer(sdl_state.renderer);
        SDL_DestroyWindow(sdl_state.window);
        SDL_Quit();
        return;
    }

    sdl_state.sdl_ready = true;
    sdl_state.initialized = true;
    sdl_state.running = true;
    sdl_state.last_render = SDL_GetTicks();

#ifdef __APPLE__
    /* Create a timer to pump SDL events even when no audio is playing */
    sdl_state.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                              dispatch_get_main_queue());
    if (sdl_state.timer) {
        dispatch_source_set_timer(sdl_state.timer,
                                  dispatch_time(DISPATCH_TIME_NOW, 0),
                                  NSEC_PER_SEC / 60, /* 60 Hz */
                                  NSEC_PER_MSEC);    /* 1ms leeway */
        dispatch_source_set_event_handler(sdl_state.timer, ^{
            if (sdl_state.running && sdl_state.sdl_ready)
                do_render_frame();
        });
        dispatch_resume(sdl_state.timer);
    }
#endif
}

/* ---------------------------------------------------------------------- */

static void sdl_scope_demod(struct demod_state *s, buffer_t buffer, int length)
{
    (void)s;

    if (!sdl_state.initialized || !sdl_state.running) return;

#ifdef __APPLE__
    bool is_main_thread = pthread_main_np();
#else
    bool is_main_thread = true;
#endif

    if (is_main_thread) {
        /* File/stdin input: process directly at real-time pace */
        if (sdl_state.start_time == 0)
            sdl_state.start_time = SDL_GetTicks();

        int samples_per_frame = SAMPLING_RATE / 60;

        for (int i = 0; i < length; i++) {
            process_sample(buffer.fbuffer[i]);
            sdl_state.samples_processed++;

            /* Render and throttle every frame's worth of samples */
            if (sdl_state.samples_processed % samples_per_frame == 0) {
                /* Poll events */
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    if (event.type == SDL_EVENT_QUIT ||
                        (event.type == SDL_EVENT_KEY_DOWN &&
                         (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q))) {
                        sdl_state.running = false;
                        _exit(0);
                    }
                    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                        colormap = (colormap == colormap_green) ? colormap_amber : colormap_green;
                    }
                }

                /* Rate limit and render (same as audio path) */
                uint64_t now = SDL_GetTicks();
                if (now - sdl_state.last_render >= 16) {
                    sdl_state.last_render = now;
                    render_display();
                }

                /* Real-time throttle */
                uint64_t expected_ms = (sdl_state.samples_processed * 1000) / SAMPLING_RATE;
                uint64_t elapsed_ms = SDL_GetTicks() - sdl_state.start_time;
                if (expected_ms > elapsed_ms + 2) {
                    SDL_Delay((uint32_t)(expected_ms - elapsed_ms));
                }
            }
        }
    } else {
        /* Async audio input: queue samples for timer-based rendering */
        for (int i = 0; i < length; i++) {
            sdl_state.ring[sdl_state.ring_write] = buffer.fbuffer[i];
            sdl_state.ring_write = (sdl_state.ring_write + 1) % RING_BUFFER_SIZE;
        }
#ifdef __APPLE__
        dispatch_async(dispatch_get_main_queue(), ^{
            do_render_frame();
        });
#endif
    }
}

/* ---------------------------------------------------------------------- */

static void sdl_scope_deinit(struct demod_state *s)
{
    (void)s;

    if (!sdl_state.initialized) return;
    sdl_state.running = false;

#ifdef __APPLE__
    if (sdl_state.timer) {
        dispatch_source_cancel(sdl_state.timer);
        sdl_state.timer = NULL;
    }
#endif

    if (sdl_state.texture) SDL_DestroyTexture(sdl_state.texture);
    if (sdl_state.renderer) SDL_DestroyRenderer(sdl_state.renderer);
    if (sdl_state.window) SDL_DestroyWindow(sdl_state.window);
    SDL_Quit();

    free(sdl_state.intensity);
    free(sdl_state.pixels);
    memset(&sdl_state, 0, sizeof(sdl_state));
}

/* ---------------------------------------------------------------------- */

const struct demod_param demod_sdl_scope = {
    "SDL_SCOPE", true, SAMPLING_RATE, 0, sdl_scope_init, sdl_scope_demod, sdl_scope_deinit
};

#endif /* NO_SDL3 */
