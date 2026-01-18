/*
 * gen_scope.c -- Generate audio that draws text on SDL_SCOPE
 *
 * Encodes text as amplitude patterns that render as readable characters
 * on a phosphor oscilloscope display.
 */

#include "gen.h"
#include <string.h>

/* 5x7 bitmap font - each character is 5 columns of 7 bits */
static const unsigned char font_5x7[][5] = {
    /* Space */
    {0x00, 0x00, 0x00, 0x00, 0x00},
    /* ! */
    {0x00, 0x00, 0x5F, 0x00, 0x00},
    /* " */
    {0x00, 0x07, 0x00, 0x07, 0x00},
    /* # */
    {0x14, 0x7F, 0x14, 0x7F, 0x14},
    /* $ */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},
    /* % */
    {0x23, 0x13, 0x08, 0x64, 0x62},
    /* & */
    {0x36, 0x49, 0x55, 0x22, 0x50},
    /* ' */
    {0x00, 0x05, 0x03, 0x00, 0x00},
    /* ( */
    {0x00, 0x1C, 0x22, 0x41, 0x00},
    /* ) */
    {0x00, 0x41, 0x22, 0x1C, 0x00},
    /* * */
    {0x08, 0x2A, 0x1C, 0x2A, 0x08},
    /* + */
    {0x08, 0x08, 0x3E, 0x08, 0x08},
    /* , */
    {0x00, 0x50, 0x30, 0x00, 0x00},
    /* - */
    {0x08, 0x08, 0x08, 0x08, 0x08},
    /* . */
    {0x00, 0x60, 0x60, 0x00, 0x00},
    /* / */
    {0x20, 0x10, 0x08, 0x04, 0x02},
    /* 0 */
    {0x3E, 0x51, 0x49, 0x45, 0x3E},
    /* 1 */
    {0x00, 0x42, 0x7F, 0x40, 0x00},
    /* 2 */
    {0x42, 0x61, 0x51, 0x49, 0x46},
    /* 3 */
    {0x21, 0x41, 0x45, 0x4B, 0x31},
    /* 4 */
    {0x18, 0x14, 0x12, 0x7F, 0x10},
    /* 5 */
    {0x27, 0x45, 0x45, 0x45, 0x39},
    /* 6 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30},
    /* 7 */
    {0x01, 0x71, 0x09, 0x05, 0x03},
    /* 8 */
    {0x36, 0x49, 0x49, 0x49, 0x36},
    /* 9 */
    {0x06, 0x49, 0x49, 0x29, 0x1E},
    /* : */
    {0x00, 0x36, 0x36, 0x00, 0x00},
    /* ; */
    {0x00, 0x56, 0x36, 0x00, 0x00},
    /* < */
    {0x00, 0x08, 0x14, 0x22, 0x41},
    /* = */
    {0x14, 0x14, 0x14, 0x14, 0x14},
    /* > */
    {0x41, 0x22, 0x14, 0x08, 0x00},
    /* ? */
    {0x02, 0x01, 0x51, 0x09, 0x06},
    /* @ */
    {0x32, 0x49, 0x79, 0x41, 0x3E},
    /* A */
    {0x7E, 0x11, 0x11, 0x11, 0x7E},
    /* B */
    {0x7F, 0x49, 0x49, 0x49, 0x36},
    /* C */
    {0x3E, 0x41, 0x41, 0x41, 0x22},
    /* D */
    {0x7F, 0x41, 0x41, 0x22, 0x1C},
    /* E */
    {0x7F, 0x49, 0x49, 0x49, 0x41},
    /* F */
    {0x7F, 0x09, 0x09, 0x01, 0x01},
    /* G */
    {0x3E, 0x41, 0x41, 0x51, 0x32},
    /* H */
    {0x7F, 0x08, 0x08, 0x08, 0x7F},
    /* I */
    {0x00, 0x41, 0x7F, 0x41, 0x00},
    /* J */
    {0x20, 0x40, 0x41, 0x3F, 0x01},
    /* K */
    {0x7F, 0x08, 0x14, 0x22, 0x41},
    /* L */
    {0x7F, 0x40, 0x40, 0x40, 0x40},
    /* M */
    {0x7F, 0x02, 0x04, 0x02, 0x7F},
    /* N */
    {0x7F, 0x04, 0x08, 0x10, 0x7F},
    /* O */
    {0x3E, 0x41, 0x41, 0x41, 0x3E},
    /* P */
    {0x7F, 0x09, 0x09, 0x09, 0x06},
    /* Q */
    {0x3E, 0x41, 0x51, 0x21, 0x5E},
    /* R */
    {0x7F, 0x09, 0x19, 0x29, 0x46},
    /* S */
    {0x46, 0x49, 0x49, 0x49, 0x31},
    /* T */
    {0x01, 0x01, 0x7F, 0x01, 0x01},
    /* U */
    {0x3F, 0x40, 0x40, 0x40, 0x3F},
    /* V */
    {0x1F, 0x20, 0x40, 0x20, 0x1F},
    /* W */
    {0x7F, 0x20, 0x18, 0x20, 0x7F},
    /* X */
    {0x63, 0x14, 0x08, 0x14, 0x63},
    /* Y */
    {0x03, 0x04, 0x78, 0x04, 0x03},
    /* Z */
    {0x61, 0x51, 0x49, 0x45, 0x43},
    /* [ */
    {0x00, 0x00, 0x7F, 0x41, 0x41},
    /* \ */
    {0x02, 0x04, 0x08, 0x10, 0x20},
    /* ] */
    {0x41, 0x41, 0x7F, 0x00, 0x00},
    /* ^ */
    {0x04, 0x02, 0x01, 0x02, 0x04},
    /* _ */
    {0x40, 0x40, 0x40, 0x40, 0x40},
    /* ` */
    {0x00, 0x01, 0x02, 0x04, 0x00},
    /* a */
    {0x20, 0x54, 0x54, 0x54, 0x78},
    /* b */
    {0x7F, 0x48, 0x44, 0x44, 0x38},
    /* c */
    {0x38, 0x44, 0x44, 0x44, 0x20},
    /* d */
    {0x38, 0x44, 0x44, 0x48, 0x7F},
    /* e */
    {0x38, 0x54, 0x54, 0x54, 0x18},
    /* f */
    {0x08, 0x7E, 0x09, 0x01, 0x02},
    /* g */
    {0x08, 0x14, 0x54, 0x54, 0x3C},
    /* h */
    {0x7F, 0x08, 0x04, 0x04, 0x78},
    /* i */
    {0x00, 0x44, 0x7D, 0x40, 0x00},
    /* j */
    {0x20, 0x40, 0x44, 0x3D, 0x00},
    /* k */
    {0x00, 0x7F, 0x10, 0x28, 0x44},
    /* l */
    {0x00, 0x41, 0x7F, 0x40, 0x00},
    /* m */
    {0x7C, 0x04, 0x18, 0x04, 0x78},
    /* n */
    {0x7C, 0x08, 0x04, 0x04, 0x78},
    /* o */
    {0x38, 0x44, 0x44, 0x44, 0x38},
    /* p */
    {0x7C, 0x14, 0x14, 0x14, 0x08},
    /* q */
    {0x08, 0x14, 0x14, 0x18, 0x7C},
    /* r */
    {0x7C, 0x08, 0x04, 0x04, 0x08},
    /* s */
    {0x48, 0x54, 0x54, 0x54, 0x20},
    /* t */
    {0x04, 0x3F, 0x44, 0x40, 0x20},
    /* u */
    {0x3C, 0x40, 0x40, 0x20, 0x7C},
    /* v */
    {0x1C, 0x20, 0x40, 0x20, 0x1C},
    /* w */
    {0x3C, 0x40, 0x30, 0x40, 0x3C},
    /* x */
    {0x44, 0x28, 0x10, 0x28, 0x44},
    /* y */
    {0x0C, 0x50, 0x50, 0x50, 0x3C},
    /* z */
    {0x44, 0x64, 0x54, 0x4C, 0x44},
    /* { */
    {0x00, 0x08, 0x36, 0x41, 0x00},
    /* | */
    {0x00, 0x00, 0x7F, 0x00, 0x00},
    /* } */
    {0x00, 0x41, 0x36, 0x08, 0x00},
    /* ~ */
    {0x08, 0x04, 0x08, 0x10, 0x08},
};

#define FONT_WIDTH 5
#define FONT_HEIGHT 7
#define CHAR_SPACING 2          /* Columns of spacing between chars */
#define COLUMN_SCALE 5          /* Repeat each column this many times for wider chars */
#define SAMPLES_PER_COLUMN 230  /* Match SDL_SCOPE: 5s * 22050 / 480 */

static const unsigned char *get_glyph(char c)
{
    if (c >= ' ' && c <= '~')
        return font_5x7[c - ' '];
    return font_5x7[0];  /* Space for unknown chars */
}

void gen_init_scope(void)
{
    /* Nothing to initialize */
}

/* Map a row position (can be fractional) to amplitude */
static short row_to_amp(float row)
{
    /* Map row 0-6 to y range with scaling */
    float y = 1.0f - (row / (float)(FONT_HEIGHT - 1)) * 2.0f;
    y *= 0.6f;
    /* Clamp to valid range */
    if (y > 1.0f) y = 1.0f;
    if (y < -1.0f) y = -1.0f;
    return (short)(y * 32000);
}

static void fill_blanking(short *samples, int count)
{
    /* Draw at bottom of text range (row 6) */
    short baseline = row_to_amp(FONT_HEIGHT - 1);
    for (int s = 0; s < count; s++)
        samples[s] = baseline;
}

/* Generate samples for one screen column */
static void gen_column(unsigned char bits, gen_write_t write_cb, void *ctx)
{
    short samples[SAMPLES_PER_COLUMN];
    int sample_idx = 0;

    if (bits == 0) {
        /* Empty column - use blanking */
        fill_blanking(samples, SAMPLES_PER_COLUMN);
        write_cb(ctx, samples, SAMPLES_PER_COLUMN);
        return;
    }

    /* Find contiguous runs of lit pixels */
    int runs[FONT_HEIGHT][2];  /* [start_row, end_row] for each run */
    int num_runs = 0;
    int run_start = -1;
    
    for (int row = 0; row <= FONT_HEIGHT; row++) {
        int lit = (row < FONT_HEIGHT) && (bits & (1 << row));
        if (lit && run_start < 0) {
            run_start = row;
        } else if (!lit && run_start >= 0) {
            runs[num_runs][0] = run_start;
            runs[num_runs][1] = row - 1;
            num_runs++;
            run_start = -1;
        }
    }

    /* Distribute samples across runs based on their size */
    int total_rows = 0;
    for (int r = 0; r < num_runs; r++) {
        total_rows += runs[r][1] - runs[r][0] + 1;
    }

    for (int r = 0; r < num_runs && sample_idx < SAMPLES_PER_COLUMN; r++) {
        int start_row = runs[r][0];
        int end_row = runs[r][1];
        int run_height = end_row - start_row + 1;

        /* Samples for this run proportional to its height */
        int run_samples = (r == num_runs - 1) 
                        ? (SAMPLES_PER_COLUMN - sample_idx)
                        : (SAMPLES_PER_COLUMN * run_height / total_rows);

        /* Vertical extent of this run */
        float row_top = start_row - 0.4f;
        float row_bottom = end_row + 0.4f;
        if (row_top < 0) row_top = 0;
        if (row_bottom > FONT_HEIGHT - 1) row_bottom = FONT_HEIGHT - 1;

        /* Sweep up and down within this run multiple times */
        int sweeps = 4;
        int samples_per_sweep = run_samples / (sweeps * 2);
        if (samples_per_sweep < 2) samples_per_sweep = 2;

        for (int sweep = 0; sweep < sweeps && sample_idx < SAMPLES_PER_COLUMN; sweep++) {
            /* Sweep down */
            for (int s = 0; s < samples_per_sweep && sample_idx < SAMPLES_PER_COLUMN; s++) {
                float t = s / (float)(samples_per_sweep - 1);
                float row = row_top + t * (row_bottom - row_top);
                samples[sample_idx++] = row_to_amp(row);
            }
            /* Sweep up */
            for (int s = 0; s < samples_per_sweep && sample_idx < SAMPLES_PER_COLUMN; s++) {
                float t = s / (float)(samples_per_sweep - 1);
                float row = row_bottom - t * (row_bottom - row_top);
                samples[sample_idx++] = row_to_amp(row);
            }
        }
    }

    /* Fill any remaining */
    while (sample_idx < SAMPLES_PER_COLUMN) {
        samples[sample_idx] = samples[sample_idx - 1];
        sample_idx++;
    }

    write_cb(ctx, samples, SAMPLES_PER_COLUMN);
}

int gen_scope(const char *text, int txtlen, gen_write_t write_cb, void *ctx)
{
    if (!text || txtlen <= 0)
        return 0;

    for (int i = 0; i < txtlen; i++) {
        const unsigned char *glyph = get_glyph(text[i]);

        /* Draw each column of the character, scaled up */
        for (int col = 0; col < FONT_WIDTH; col++) {
            unsigned char bits = glyph[col];
            /* Repeat each column COLUMN_SCALE times for wider characters */
            for (int rep = 0; rep < COLUMN_SCALE; rep++) {
                gen_column(bits, write_cb, ctx);
            }
        }

        /* Inter-character spacing */
        for (int sp = 0; sp < CHAR_SPACING * COLUMN_SCALE; sp++) {
            gen_column(0, write_cb, ctx);
        }
    }

    return 0;
}
