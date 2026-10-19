/* Standalone: cc -std=gnu11 -I. test/test_flex_address.c bch.c cJSON.c -lm -o test_flex_address */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multimon.h"
#include "../demod_flex_next.c"

int json_mode;
int verb_level;
void _verbprintf(int level, const char *fmt, ...) { (void)level; (void)fmt; }

#undef assert
#define assert(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

static uint32_t encoded(uint32_t data) {
    uint32_t w = bch_flex_encode(data), bits = w, parity = 0;
    while (bits) { parity ^= bits & 1; bits >>= 1; }
    return w | (parity << 31);
}

/* Distinct group instructions make incorrect vector pairing observable in state. */
static void pairing(uint32_t first, uint32_t second, int is_long,
                    int corrupt_word, uint32_t errors, int first_valid) {
    struct Flex_Next *f = Flex_New(FREQ_SAMP);
    assert(f);
    f->Sync.baud = 1600;
    f->Sync.levels = 2;
    f->Decode.long_address = 1; /* A skipped short page must not inherit this. */
    uint32_t words[PHASE_WORDS] = {0};
    int width = is_long ? 2 : 1;
    int vf = width + 2;
    words[0] = vf << 10;
    words[1] = first;
    if (is_long) words[2] = second;
    words[width + 1] = 0x8000 + 222;
    words[vf] = (1 << 4) | (5 << 10); /* slot 0, frame 5 */
    if (is_long) words[vf + 1] = (1 << 4) | (7 << 10) | (2 << 17);
    words[vf + width] = (1 << 4) | (6 << 10) | (1 << 17);
    for (int i = 0; i < PHASE_WORDS; i++)
        f->Data.PhaseA.buf[i] = encoded(words[i]);
    if (corrupt_word) f->Data.PhaseA.buf[corrupt_word] ^= errors;
    decode_phase(f, 'A');
    if (corrupt_word)
        assert(f->Data.PhaseA.bch_err[corrupt_word] == !first_valid);
    assert(flex_group_endpoint(&f->GroupHandler, 1) == 1);
    assert(f->GroupHandler.GroupCodes[1][1] == 222);
    assert(f->GroupHandler.GroupFrame[1] == 6);
    assert(flex_group_endpoint(&f->GroupHandler, 0) == first_valid);
    if (first_valid) {
        int64_t capcode = is_long ? 1 + 32768LL + 2068480LL : first - 0x8000;
        assert(f->GroupHandler.GroupCodes[0][1] == capcode);
    }
    assert(flex_group_endpoint(&f->GroupHandler, 2) == 0);
    assert(!f->Decode.long_address);
    Flex_Delete(f);
}

static void rejected_vector(int is_long, int reason) {
    struct Flex_Next *f = Flex_New(FREQ_SAMP);
    assert(f);
    f->Sync.baud = 1600;
    f->Sync.levels = 2;
    uint32_t words[PHASE_WORDS] = {0};
    int width = is_long ? 2 : 1;
    int vf = width + 2;
    int hdr = is_long ? vf + 1 : 10;
    int pointer = reason == 1 ? PHASE_WORDS : reason == 2 ? PHASE_WORDS - 1 : 10;
    words[0] = vf << 10;
    words[1] = is_long ? 1 : 0x8000 + 111;
    if (is_long) words[2] = 0x1ffffe;
    words[width + 1] = 0x8000 + 222;
    words[vf] = (5 << 4) | (pointer << 7) | (2 << 14);
    words[hdr] = 3 << 11;
    if (reason == 3) words[vf] = 5 << 4; /* Long, zero pointer and length. */
    words[vf + width] = (1 << 4) | (6 << 10) | (1 << 17);
    for (int i = 0; i < PHASE_WORDS; i++)
        f->Data.PhaseA.buf[i] = encoded(words[i]);
    if (reason == 0) f->Data.PhaseA.buf[hdr] ^= 7;
    decode_phase(f, 'A');
    if (reason == 0) assert(f->Data.PhaseA.bch_err[hdr]);
    assert(flex_group_endpoint(&f->GroupHandler, 1) == 1);
    assert(f->GroupHandler.GroupCodes[1][1] == 222);
    assert(f->GroupHandler.GroupFrame[1] == 6);
    assert(flex_group_endpoint(&f->GroupHandler, 0) == 0);
    assert(!f->Decode.long_address);
    Flex_Delete(f);
}

int main(void) {
    for (int is_long = 0; is_long <= 1; is_long++) {
        rejected_vector(is_long, 0); /* Uncorrectable header. */
        rejected_vector(is_long, 1); /* Pointer outside the phase. */
        if (!is_long) rejected_vector(is_long, 2); /* Header at 87, body at 88. */
    }
    rejected_vector(1, 3);
    pairing(0x8000 + 111, 0, 0, 1, 7, 0);
    pairing(0x8000 + 111, 0, 0, 1, 1, 1);
    pairing(0x8000 + 111, 0, 0, 1, 3, 1);
    pairing(0, 0, 0, 0, 0, 0);
    pairing(0x1fffff, 0, 0, 0, 0, 0);
    pairing(1, 0x1ffffe, 1, 2, 7, 0);
    pairing(1, 0x8000 + 111, 1, 0, 0, 0);
    /* Parity-bit corruption preserves the detectable LA1/LA2 prefix. */
    pairing(1, 0x1ffffe, 1, 1, 7u << 21, 0);
    pairing(0x1ffffe, 0x1e0001, 1, 1, 7u << 21, 0);
    pairing(1, 0x1ffffe, 1, 1, 3, 1);
    pairing(1, 0x1ffffe, 1, 2, 3, 1);
    pairing(1, 0x1ffffe, 1, 0, 0, 1);
    /* A truncated long address must not read its second word from the VF. */
    struct Flex_Next *f = Flex_New(FREQ_SAMP);
    assert(f);
    f->Data.PhaseA.buf[0] = encoded(2 << 10);
    f->Data.PhaseA.buf[1] = encoded(1);
    f->Data.PhaseA.buf[2] = encoded((1 << 4) | (5 << 10));
    decode_phase(f, 'A');
    assert(flex_group_endpoint(&f->GroupHandler, 0) == 0);
    Flex_Delete(f);
    puts("FLEX address pairing tests passed");
    return 0;
}
