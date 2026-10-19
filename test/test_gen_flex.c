/* Run with: cc -I. test/test_gen_flex.c gen_flex.c bch.c -o test_gen_flex */
#include "gen.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#undef assert
#define assert(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

#define FRAME_BITS 3992
#define FIW_OFFSET (960 + 64 + 16)
#define DATA_OFFSET (FIW_OFFSET + 32 + 40)

static uint32_t word_at(const unsigned char *bits, int offset, int stride)
{
    uint32_t word = 0;
    for (int bit = 0; bit < 32; bit++)
        word |= (uint32_t)bits[offset + bit * stride] << bit;
    return word;
}

int main(void)
{
    struct gen_params p = {0};
    struct gen_state clean, noisy;
    uint32_t members[93];
    strcpy(p.p.flex.message, "rollover");
    p.p.flex.capcode = 12345;
    for (int i = 0; i < 93; i++) members[i] = 1000000 + i;
    p.p.flex.group_capcodes = members;
    p.p.flex.group_count = 93;
    /* Cross both an ordinary cycle boundary and the 14 -> 0 boundary. */
    for (int cycle = 3; cycle <= 14; cycle += 11) {
        p.p.flex.cycle = cycle;
        p.p.flex.frame = 126;
        gen_init_flex(&p, &clean);
        assert(clean.s.flex.data && clean.s.flex.datalen == 4 * FRAME_BITS);
        for (int frame = 0; frame < 4; frame++) {
            const unsigned char *bits = clean.s.flex.data + frame * FRAME_BITS;
            uint32_t fiw = word_at(bits, FIW_OFFSET, 1);
            assert(((fiw >> 8) & 127) == (unsigned)(126 + frame) % 128);
            assert(((fiw >> 4) & 15) == (unsigned)(cycle + (126 + frame) / 128) % 15);
            if (frame < 3) {
                int count = frame == 2 ? 7 : 43;
                int vw = 1 + count;
                int offset = DATA_OFFSET + (vw / 8) * 256 + vw % 8;
                uint32_t vector = word_at(bits, offset, 8);
                assert(((vector >> 10) & 127) == 1);
            }
        }
        /* Group frames retain the existing behavior: -e is plain-only. */
        for (int errors = 1; errors <= 3; errors++) {
            p.p.flex.errors = errors;
            gen_init_flex(&p, &noisy);
            assert(clean.s.flex.datalen == noisy.s.flex.datalen);
            assert(memcmp(clean.s.flex.data, noisy.s.flex.data, clean.s.flex.datalen) == 0);
            free(noisy.s.flex.data);
        }
        p.p.flex.errors = 0;
        free(clean.s.flex.data);
    }
    p.p.flex.group_count = 0;
    p.p.flex.frame = 42;
    p.p.flex.cycle = 3;
    gen_init_flex(&p, &clean);
    assert(((word_at(clean.s.flex.data, FIW_OFFSET, 1) >> 8) & 127) == 42);
    /* Master PRNG masks from seed 12345: ten data words, then FIW.
     * Duplicate positions consume another PRNG step within each word. */
    static const uint32_t data_masks[3][10] = {
        {0x00020000u, 0x00000080u, 0x00008000u, 0x04000000u, 0x00000004u,
         0x00040000u, 0x04000000u, 0x00000004u, 0x00000200u, 0x00020000u},
        {0x00020080u, 0x04008000u, 0x00040004u, 0x04000004u, 0x00020200u,
         0x00140000u, 0x01000080u, 0x00200010u, 0x00800100u, 0x00400040u},
        {0x00028080u, 0x04040004u, 0x04000204u, 0x00160000u, 0x01200080u,
         0x00800110u, 0x00400140u, 0x00000812u, 0x0A000004u, 0x00140040u}
    };
    static const uint32_t fiw_masks[3] = {0x00100000u, 0x00000102u, 0};
    for (int errors = 1; errors <= 3; errors++) {
        p.p.flex.errors = errors;
        gen_init_flex(&p, &noisy);
        assert(clean.s.flex.datalen == FRAME_BITS);
        assert(noisy.s.flex.datalen == clean.s.flex.datalen);
        assert((word_at(clean.s.flex.data, FIW_OFFSET, 1) ^
                word_at(noisy.s.flex.data, FIW_OFFSET, 1)) == fiw_masks[errors - 1]);
        /* Preamble, SYNC1, dotting, SYNC2 and trailing idle stay unchanged. */
        assert(memcmp(clean.s.flex.data, noisy.s.flex.data, FIW_OFFSET) == 0);
        assert(memcmp(clean.s.flex.data + FIW_OFFSET + 32,
                      noisy.s.flex.data + FIW_OFFSET + 32, 40) == 0);
        assert(memcmp(clean.s.flex.data + FRAME_BITS - 64,
                      noisy.s.flex.data + FRAME_BITS - 64, 64) == 0);
        for (int cw = 0; cw < 88; cw++) {
            int offset = DATA_OFFSET + (cw / 8) * 256 + cw % 8;
            uint32_t diff = word_at(clean.s.flex.data, offset, 8) ^
                            word_at(noisy.s.flex.data, offset, 8);
            assert(diff == (cw < 10 ? data_masks[errors - 1][cw] : 0));
        }
        free(noisy.s.flex.data);
    }
    free(clean.s.flex.data);
    puts("FLEX generator frame/cycle rollover and error injection passed");
    return 0;
}
