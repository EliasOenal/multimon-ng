/* Standalone: cc -std=gnu11 -I. test/test_fms.c -o test_fms */
#include <stdarg.h>
#include <stdio.h>
#include "multimon.h"
#include "../fms.c"

static int printed_check_bits;

void _verbprintf(int level, const char *fmt, ...) {
    (void)level;
    if (strcmp(fmt, "CRC INCORRECT (%x)") == 0) {
        va_list args;
        va_start(args, fmt);
        printed_check_bits = va_arg(args, int);
        va_end(args);
    }
}

/* Feed the sync pattern and a message through fms_rxbit and return the decoded message. */
static uint64_t receive(uint64_t message) {
    struct demod_state s = {0};
    fms_init(&s);
    for (int i = 18; i >= 0; i--)
        fms_rxbit(&s, (0x7FF1A >> i) & 1);
    for (int i = 16; i < 64; i++)
        fms_rxbit(&s, (message >> i) & 1);
    return s.l2.fmsfsk.rxbitstream;
}

/* Return the check bits fms_disp_packet prints for a message that fails the CRC. */
static int check_bits(uint64_t message, bool justhex) {
    fms_justhex = justhex;
    printed_check_bits = -1;
    fms_disp_packet(message);
    return printed_check_bits;
}

int main(void) {
    const uint64_t message = 0x68123456789A0000ULL;
    int failed = 0;
    if (receive(message) != message) {
        fprintf(stderr, "valid message not received unchanged\n");
        failed = 1;
    }
    /* A corrected message has its lowest bit set. */
    for (int i = 0; i < 48; i++) {
        if (receive(message ^ (1ULL << (16 + i))) != (message | 1)) {
            fprintf(stderr, "single-bit error at position %d not corrected\n", i);
            failed = 1;
        }
    }
    /* Telegram bits 41-47 (message bits 56-62) are the check bits, here 0x68. */
    for (int justhex = 0; justhex <= 1; justhex++) {
        if (check_bits(message ^ (1ULL << 16), justhex) != 0x68) {
            fprintf(stderr, "wrong check bits printed (justhex %d)\n", justhex);
            failed = 1;
        }
    }
    return failed;
}
