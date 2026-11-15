/* Standalone: cc -std=gnu11 -I. test/test_mpt1327.c -lm -o test_mpt1327 */
#include <stdarg.h>
#include <stdio.h>
#include "multimon.h"
#include "../demod_mpt1327.c"

static char printed[256];
int json_mode;

void addJsonTimestamp(cJSON *json_output) { (void)json_output; }

void printJson(cJSON *json_output) {
    char *json = cJSON_PrintUnformatted(json_output);
    snprintf(printed, sizeof(printed), "%s\n", json);
    cJSON_free(json);
}

void _verbprintf(int level, const char *fmt, ...) {
    va_list args;
    size_t used = strlen(printed);
    (void)level;
    va_start(args, fmt);
    vsnprintf(printed + used, sizeof(printed) - used, fmt, args);
    va_end(args);
}

static int failed;

static void check_line(uint64_t cw, const char *expected) {
    printed[0] = '\0';
    mpt1327_print(cw);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "codeword %016" PRIX64 ": printed \"%s\", expected \"%s\"\n",
                cw, printed, expected);
        failed = 1;
    }
}

static void check_json(uint64_t cw, const char *expected) {
    json_mode = 1;
    check_line(cw, expected);
    json_mode = 0;
}

/* Feeds the n low bits of value to the receiver, most significant first */
static void feed(struct demod_state *s, uint64_t value, int n) {
    while (n-- > 0)
        mpt1327_rxbit(s, (int)((value >> n) & 1));
}

/* Traffic channel, multiple message format (3.3.2): each message has bit reversals
 * and its own SYNT */
static void check_traffic_messages(void) {
    static struct demod_state s;
    const char *expected =
        "MPT1327: MAINT PFIX=12 IDENT1=345 CHAN=123 OPER=0\n"
        "MPT1327: AHY PFIX=12 IDENT1=345 IDENT2=678 D=0 POINT=0 CHECK=0 E=0 AD=0\n";

    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, 0xAAAA, 16);
    feed(&s, SYNT, 16);
    feed(&s, 0x8C0ACC647B009D61ULL, 64);
    feed(&s, 0xAAAA, 16);
    feed(&s, SYNT, 16);
    feed(&s, 0x8C0ACC4054C0DEF1ULL, 64);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "traffic channel messages: printed \"%s\", expected \"%s\"\n",
                printed, expected);
        failed = 1;
    }
}

/* System codewords on control vs. data channels are distinguished even when
 * SYS is unchanged; repeated ones should still be suppressed. */
/* The following vectors were provided in upstream ece8954. A CRC-valid
 * word spanning the next preamble/SYNT is not proof of continuous framing;
 * another CRC-valid word on the old alignment is needed to break the tie. */
static void check_explicit_boundary_lookahead(void) {
    static struct demod_state s;
    const uint64_t maint = 0x8C0ACC647B009D61ULL;
    const uint64_t ahy = 0x838E2C4054C09151ULL;
    const uint64_t gtc = 0x8C0ACA3D8A9A901AULL;
    const char *expected =
        "MPT1327: MAINT PFIX=12 IDENT1=345 CHAN=123 OPER=0\n"
        "MPT1327: AHY PFIX=3 IDENT1=4549 IDENT2=678 D=0 POINT=0 CHECK=0 E=0 AD=0\n";
    const char *eof_ambiguous =
        "MPT1327: MAINT PFIX=12 IDENT1=345 CHAN=123 OPER=0\n"
        "MPT1327: GTC PFIX=42 IDENT1=5447 D=1 CHAN=593 IDENT2=227 N=2\n";

    if (!mpt1327_valid(maint) || !mpt1327_valid(ahy) ||
        !mpt1327_valid(0xAAAA3B28838E2C40ULL)) {
        fprintf(stderr, "invalid upstream ambiguity fixture\n");
        failed = 1;
        return;
    }
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, 0xAAAA, 16);
    feed(&s, SYNT, 16);
    feed(&s, maint, 64);
    feed(&s, 0xAAAA, 16);
    feed(&s, SYNT, 16);
    feed(&s, ahy, 64);
    /* EOF offers no tie-break evidence. Keep the previously validated word,
     * even though this fixture was constructed from a real AHY boundary. */
    mpt1327_deinit(&s);
    if (strcmp(printed, eof_ambiguous) != 0) {
        fprintf(stderr, "upstream ambiguous EOF: got \"%s\", expected \"%s\"\n", printed, eof_ambiguous);
        failed = 1;
    }

    /* Drive the old alignment to its next CRC check. In this real-boundary
     * case that check fails and the buffered AHY must win before EOF. */
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, 0xAAAA, 16);
    feed(&s, SYNT, 16);
    feed(&s, maint, 64);
    feed(&s, 0xAAAA, 16);
    feed(&s, SYNT, 16);
    feed(&s, ahy, 64);
    feed(&s, SYNT, 16);
    feed(&s, gtc >> 48, 16);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "late traffic arbitration: got \"%s\", expected \"%s\"\n", printed, expected);
        failed = 1;
    }
    feed(&s, gtc & 0xFFFFFFFFFFFFULL, 48);
    mpt1327_deinit(&s);
    if (strcmp(printed, "MPT1327: MAINT PFIX=12 IDENT1=345 CHAN=123 OPER=0\n"
                        "MPT1327: AHY PFIX=3 IDENT1=4549 IDENT2=678 D=0 POINT=0 CHECK=0 E=0 AD=0\n"
                        "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n") != 0) {
        fprintf(stderr, "third traffic message lost after arbitration: %s\n", printed);
        failed = 1;
    }
}

/* Also preserve the upstream regression for a false DCSC between messages,
 * and recognition of a CCSC whose information begins with SYNT. */
static void check_upstream_system_collisions(void) {
    static struct demod_state s;
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, 0xAAAA, 16); feed(&s, SYNT, 16);
    feed(&s, 0xB0A31E793403DF2EULL, 64);
    feed(&s, 0xAAAA, 16); feed(&s, SYNT, 16);
    feed(&s, 0x8B904FE969A5A5A9ULL, 64);
    mpt1327_deinit(&s);
    if (strcmp(printed, "MPT1327: CAT=100 PFIX=48 IDENT1=5219 PARAMS=0x793403\n"
                        "MPT1327: CAT=111 PFIX=11 IDENT1=4617 PARAMS=0x6969a5\n") != 0) {
        fprintf(stderr, "false DCSC across message boundary: %s\n", printed);
        failed = 1;
    }
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, 0x3B289536AAAAC4D7ULL, 64);
    feed(&s, 0x8C0ACA3D8A9A901AULL, 64);
    mpt1327_deinit(&s);
    if (strcmp(printed, "MPT1327: CCSC SYS=3B28\n"
                        "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n") != 0) {
        fprintf(stderr, "CCSC begins SYNT: %s\n", printed);
        failed = 1;
    }
}

static void check_system_codewords(void) {
    static struct demod_state s;
    const uint64_t ccsc = 0x12344221AAAAC4D7ULL;
    const uint64_t dcsc = 0x1234013BAAAA3B28ULL;
    const char *expected =
        "MPT1327: CCSC SYS=1234\n"
        "MPT1327: DCSC SYS=1234\n"
        "MPT1327: CCSC SYS=1234\n";

    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, ccsc, 64); /* the final SYNC also establishes framing */
    feed(&s, dcsc, 64);
    feed(&s, dcsc, 64);
    feed(&s, ccsc, 64);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "system codewords: printed \"%s\", expected \"%s\"\n",
                printed, expected);
        failed = 1;
    }

    /* SYNT search must also recognize the last 16 bits of a DCSC. */
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, dcsc, 64);
    if (strcmp(printed, "MPT1327: DCSC SYS=1234\n") != 0) {
        fprintf(stderr, "DCSC acquisition failed: %s\n", printed);
        failed = 1;
    }
}

/* Exercise every CAT/TYPE/FUNC combination, including reserved and spare
 * values, with maximal idents. All JSON results must have a deliberate
 * classification; no uninitialized/default message buffer is involved. */
static void check_json_all_functions(void) {
    unsigned int cat, type, func;

    json_mode = 1;
    for (cat = 0; cat < 8; cat++) {
        for (type = 0; type < 4; type++) {
            for (func = 0; func < 8; func++) {
                uint64_t info = (1ULL << 47) | (127ULL << 40) |
                    (8191ULL << 27) | (1ULL << 26) | ((uint64_t)cat << 23) |
                    ((uint64_t)type << 21) | ((uint64_t)func << 18) | 0x3FFFF;
                cJSON *json, *message;

                printed[0] = '\0';
                mpt1327_print(info << 16); /* test presentation, not CRC */
                json = cJSON_Parse(printed);
                message = json ? cJSON_GetObjectItemCaseSensitive(json, "message") : NULL;
                if (!cJSON_IsString(message) || !message->valuestring ||
                    !message->valuestring[0]) {
                    fprintf(stderr, "missing JSON message for CAT=%u TYPE=%u FUNC=%u: %s\n",
                            cat, type, func, printed);
                    failed = 1;
                }
                if (cat != 0 && cJSON_IsString(message) &&
                    strcmp(message->valuestring, "UNDECODED") != 0) {
                    fprintf(stderr, "invented mnemonic for CAT=%u: %s\n", cat, printed);
                    failed = 1;
                }
                cJSON_Delete(json);
            }
        }
    }
    json_mode = 0;
}

/* A real traffic-channel SYNT may arrive while an earlier false SYNC has
 * established tentative 64-bit framing. The overlapping block is even a
 * valid codeword, so CRC validity alone must not defeat explicit framing. */
static void check_traffic_boundary_preempts_false_frame(void) {
    static struct demod_state s;
    const uint64_t gtc = 0x8C0ACA3D8A9A901AULL;
    const uint64_t misleading = 0xD37FAAAA3B288C0AULL;
    const char *expected =
        "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n";

    if (!mpt1327_valid(misleading) ||
        (uint16_t)misleading != (uint16_t)(gtc >> 48)) {
        fprintf(stderr, "invalid overlapping traffic test fixture\n");
        failed = 1;
        return;
    }
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, SYNC, 16);      /* false provisional acquisition */
    feed(&s, 0xD37F, 16);
    feed(&s, 0xAAAA, 16);    /* real traffic preamble */
    feed(&s, SYNT, 16);      /* must supersede tentative framing */
    feed(&s, gtc, 64);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "traffic boundary: got \"%s\", expected \"%s\"\n",
                printed, expected);
        failed = 1;
    }
}

/* Even when a false framing position produces a CRC-valid codeword,
 * check the sliding 64-bit window whenever a CCSC's trailing SYNC arrives.
 * This must not depend on whether the provisional 64-bit counter is idle. */
static void check_ccsc_during_tentative_frame(void) {
    static struct demod_state s;
    const uint64_t ccsc = 0x12344221AAAAC4D7ULL;
    const uint64_t misleading = 0xB24012344221AAAAULL;

    if (!mpt1327_valid(misleading)) {
        fprintf(stderr, "invalid overlapping CCSC test fixture\n");
        failed = 1;
        return;
    }
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, SYNC, 16);    /* false provisional acquisition */
    feed(&s, 0xB240, 16);
    feed(&s, ccsc, 64);
    if (strcmp(printed, "MPT1327: CCSC SYS=1234\n") != 0) {
        fprintf(stderr, "spurious or missing word during CCSC acquisition: %s\n", printed);
        failed = 1;
    }
}

/* TSC messages may follow SYNT without bit reversals (3.3.2). A valid
 * misaligned codeword must not hide the actual address word that follows. */
static void check_bare_synt_supersedes_false_frame(void) {
    static struct demod_state s;
    const uint64_t gtc = 0x8C0ACA3D8A9A901AULL;
    const uint64_t misleading = 0xABCDA3D73B288C0AULL;
    const char *expected =
        "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n";

    if (!mpt1327_valid(misleading)) {
        fprintf(stderr, "invalid bare-SYNT overlapping test fixture\n");
        failed = 1;
        return;
    }
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, SYNC, 16);
    feed(&s, 0xABCDA3D7ULL, 32);
    feed(&s, SYNT, 16);     /* no preceding bit reversals */
    feed(&s, gtc, 64);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "bare SYNT: got \"%s\", expected \"%s\"\n",
                printed, expected);
        failed = 1;
    }
}

/* An incidental SYNT inside a valid DATA codeword must not discard it when
 * the alternative alignment does not validate as a new address message. */
static void check_incidental_synt_preserves_frame(void) {
    static struct demod_state s;
    const uint64_t data = 0x12343B285678FBBCULL;
    const uint64_t gtc = 0x8C0ACA3D8A9A901AULL;
    const char *expected =
        "MPT1327: DATA 0x12343b285678\n"
        "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n";

    if (!mpt1327_valid(data)) {
        fprintf(stderr, "invalid incidental-SYNT test fixture\n");
        failed = 1;
        return;
    }
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, SYNC, 16);
    feed(&s, data, 64);
    feed(&s, gtc, 64);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "incidental SYNT: got \"%s\", expected \"%s\"\n",
                printed, expected);
        failed = 1;
    }
}

/* A false SYNT inside a DATA codeword must not prevent recognizing
 * the actual bare SYNT that starts the following traffic message. */
static void check_overlapping_synt_candidates(void) {
    static struct demod_state s;
    const uint64_t data = 0x12343B285678FBBCULL;
    const uint64_t gtc = 0x8C0ACA3D8A9A901AULL;
    const char *expected =
        "MPT1327: DATA 0x12343b285678\n"
        "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n";
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, SYNC, 16);
    feed(&s, data, 64);
    feed(&s, SYNT, 16);
    feed(&s, gtc, 64);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "overlapping SYNT candidates: got \"%s\", expected \"%s\"\n",
                printed, expected);
        failed = 1;
    }
}

/* The check bits of a valid DATA word may coincidentally equal SYNT.
 * That must not delete the DATA when the next aligned word is valid. */
static void check_synt_in_valid_codeword_trailer(void) {
    static struct demod_state s;
    const uint64_t data = 0x01234567DBA73B28ULL;
    const uint64_t gtc = 0x8C0ACA3D8A9A901AULL;
    const char *expected =
        "MPT1327: DATA 0x01234567dba7\n"
        "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n";
    if (!mpt1327_valid(data) || (data & 0xffff) != SYNT) {
        fprintf(stderr, "invalid SYNT trailer fixture\n");
        failed = 1;
        return;
    }
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, SYNC, 16);
    feed(&s, data, 64);
    feed(&s, gtc, 64);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "incidental trailer SYNT: got \"%s\", expected \"%s\"\n",
                printed, expected);
        failed = 1;
    }
}

/* Multiple traffic messages may follow one another with no preamble. */
static void check_contiguous_traffic_messages(void) {
    static struct demod_state s;
    const char *expected =
        "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n"
        "MPT1327: MAINT PFIX=12 IDENT1=345 CHAN=123 OPER=0\n";
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, SYNT, 16);
    feed(&s, 0x8C0ACA3D8A9A901AULL, 64);
    feed(&s, SYNT, 16);
    feed(&s, 0x8C0ACC647B009D61ULL, 64);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "contiguous traffic: got \"%s\", expected \"%s\"\n",
                printed, expected);
        failed = 1;
    }
}

/* Vary an incidental SYNT through all possible positions of the 48-bit
 * DATA information field. Every following true bare SYNT must be found. */
static void check_synt_in_all_data_positions(void) {
    static struct demod_state s;
    const uint64_t address = 0x8C0ACA3D8A9A901AULL;
    unsigned int i;
    for (i = 0; i <= 32; i++) {
        uint64_t info = 0x0123456789ABULL;
        uint64_t data;
        uint64_t word;
        unsigned int check;
        char expected_data[48];
        info = (info & ~(0xFFFFULL << i)) | (0x3B28ULL << i);
        word = (info << 15) | (mpt1327_remainder(info) ^ 1);
        check = 0;
        for (data = word; data; data >>= 1)
            check ^= (unsigned int)(data & 1);
        data = (word << 1) | check;
        if (!mpt1327_valid(data)) {
            fprintf(stderr, "incorrect generated DATA fixture at bit %u\n", i);
            failed = 1;
            return;
        }
        mpt1327_init(&s);
        printed[0] = '\0';
        feed(&s, SYNC, 16);
        feed(&s, data, 64);
        feed(&s, SYNT, 16);
        feed(&s, address, 64);
        snprintf(expected_data, sizeof(expected_data), "MPT1327: DATA 0x%012" PRIx64,
                 info);
        if (!strstr(printed, expected_data) ||
            !strstr(printed, "MPT1327: GTC PFIX=12 IDENT1=345")) {
            fprintf(stderr, "incidental SYNT at DATA bit %u: got \"%s\"\n", i, printed);
            failed = 1;
        }
    }
}

/* SYNC with a preceding alternating-bit pattern is not proof of framing:
 * all 16 possible bit positions within a DATA information word are legal. */
static void check_incidental_preamble_preserves_frame(void) {
    static struct demod_state s;
    const uint64_t gtc = 0x8C0ACA3D8A9A901AULL;
    unsigned int i;
    for (i = 0; i < 16; i++) {
        uint64_t info = (0x0123456789abULL & ~(0xFFFFFFFFULL << i)) |
                        (0xAAAAC4D7ULL << i);
        uint64_t raw = (info << 15) | (mpt1327_remainder(info) ^ 1);
        uint64_t cw, v;
        unsigned int parity = 0;
        char expected[64];
        for (v = raw; v; v >>= 1)
            parity ^= (unsigned int)(v & 1);
        cw = (raw << 1) | parity;
        if (!mpt1327_valid(cw)) {
            fprintf(stderr, "invalid preamble regression vector %u\n", i);
            failed = 1;
            return;
        }
        mpt1327_init(&s);
        printed[0] = '\0';
        feed(&s, SYNC, 16);
        feed(&s, cw, 64);
        feed(&s, gtc, 64);
        feed(&s, 0xAAAA, 16);
        snprintf(expected, sizeof(expected), "MPT1327: DATA 0x%012" PRIx64, info);
        if (!strstr(printed, expected) ||
            !strstr(printed, "MPT1327: GTC PFIX=12 IDENT1=345")) {
            fprintf(stderr, "incidental preamble at bit %u: got \"%s\"\n", i, printed);
            failed = 1;
        }
    }
}

/* A false SYNC can establish a checksum-valid provisional alignment.
 * A real control-channel preamble + SYNC must still win when its following
 * address codeword checks out, rather than printing the misleading word. */
static void check_control_boundary_preempts_false_frame(void) {
    static struct demod_state s;
    const uint64_t misleading = 0xF8FBAAAAC4D78C0AULL;
    const uint64_t gtc = 0x8C0ACA3D8A9A901AULL;
    const char *expected =
        "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n";
    if (!mpt1327_valid(misleading)) {
        fprintf(stderr, "invalid control-boundary regression vector\n");
        failed = 1;
        return;
    }
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, SYNC, 16); /* false framing marker */
    feed(&s, 0xF8FB, 16);
    feed(&s, 0xAAAA, 16);
    feed(&s, SYNC, 16); /* real control-channel boundary */
    feed(&s, gtc, 64);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "control boundary: got \"%s\", expected \"%s\"\n",
                printed, expected);
        failed = 1;
    }
}

/* A checksum-valid last DATA word containing SYNT is held while competing
 * framing is checked. At EOF it must be delivered, not silently discarded. */
static void check_deferred_end_of_input(void) {
    static struct demod_state s;
    const uint64_t data = 0x12343B285678FBBCULL;
    const uint64_t gtc = 0x8C0ACA3D8A9A901AULL;
    const char *first =
        "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n";
    const char *both =
        "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n"
        "MPT1327: DATA 0x12343b285678\n";
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, 0xAAAA, 16);
    feed(&s, SYNC, 16);
    feed(&s, gtc, 64);  /* established and checksum-confirmed alignment */
    feed(&s, data, 64); /* contains an incidental SYNT */
    if (strcmp(printed, first) != 0) {
        fprintf(stderr, "prematurely printed pending DATA: %s\n", printed);
        failed = 1;
    }
    mpt1327_deinit(&s);
    if (strcmp(printed, both) != 0) {
        fprintf(stderr, "missing DATA flush at EOF: %s\n", printed);
        failed = 1;
    }
    mpt1327_deinit(&s);
    if (strcmp(printed, both) != 0) {
        fprintf(stderr, "duplicate DATA flush at EOF: %s\n", printed);
        failed = 1;
    }
}

/* A legal pair of DATA codewords can contain a second, overlapping valid
 * codeword: either a SYNT-aligned address, a CCSC, or a DCSC. The decoder
 * must keep both DATA words on the confirmed framing, not invent a message. */
static void check_ambiguous_data_overlaps(void) {
    static struct demod_state s;
    static const struct {
        const char *name;
        uint64_t first;
        uint64_t second;
        uint64_t overlap;
        unsigned int offset; /* bit index where overlapping word begins */
    } cases[] = {
        { "SYNT-address", 0x12343B28D6789323ULL, 0x000073DCA1230609ULL,
          0xD6789323000073DCULL, 32 },
        { "CCSC", 0x0123456718671234ULL, 0x4221AAAAC4D7843EULL,
          0x12344221AAAAC4D7ULL, 48 },
        { "DCSC", 0x0123456718671234ULL, 0x013BAAAA3B285414ULL,
          0x1234013BAAAA3B28ULL, 48 },
    };
    unsigned int i;

    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        const uint64_t first = cases[i].first;
        const uint64_t second = cases[i].second;
        const unsigned int offset = cases[i].offset;
        const uint64_t reconstructed =
            ((first & ((1ULL << (64 - offset)) - 1)) << offset) |
            (second >> (64 - offset));
        char expected[256];

        if (!mpt1327_valid(first) || !mpt1327_valid(second) ||
            !mpt1327_valid(cases[i].overlap) ||
            reconstructed != cases[i].overlap ||
            (i == 0 && mpt1327_field(cases[i].overlap, 1, 1) != 1) ||
            (i == 1 && !mpt1327_is_ccsc(cases[i].overlap)) ||
            (i == 2 && !mpt1327_is_dcsc(cases[i].overlap))) {
            fprintf(stderr, "invalid overlapping %s test vector\n", cases[i].name);
            failed = 1;
            return;
        }
        mpt1327_init(&s);
        printed[0] = '\0';
        feed(&s, 0xAAAA, 16);
        feed(&s, SYNC, 16);
        feed(&s, 0x8C0ACA3D8A9A901AULL, 64);
        feed(&s, first, 64);
        feed(&s, second, 64);
        feed(&s, 0x12344221AAAAC4D7ULL, 64);
        mpt1327_deinit(&s);
        snprintf(expected, sizeof(expected),
                 "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n"
                 "MPT1327: DATA 0x%012" PRIx64 "\n"
                 "MPT1327: DATA 0x%012" PRIx64 "\n"
                 "MPT1327: CCSC SYS=1234\n", first >> 16, second >> 16);
        if (strcmp(printed, expected) != 0) {
            fprintf(stderr, "overlapping %s: got \"%s\", expected \"%s\"\n",
                    cases[i].name, printed, expected);
            failed = 1;
        }
    }
}

/* Even a genuine SYNT/SYNC + valid address can look like a one-bit-
 * correctable word on the old alignment. At the exact old word boundary,
 * explicit message synchronization has priority over that false word. */
static void check_real_boundaries_shadowed_by_correctable_words(void) {
    static struct demod_state s;
    static const struct {
        unsigned int preamble_bits;
        unsigned int marker;
        uint64_t address;
        uint64_t misleading;
        const char *decoded;
    } cases[] = {
        { 0, SYNT, 0xCB83E80C583B073BULL, 0x3B28CB83E80C583BULL,
          "MPT1327: GTC PFIX=75 IDENT1=4221 D=0 CHAN=24 IDENT2=5646 N=3\n" },
        { 16, SYNT, 0x96B9E826EEA0D1CAULL, 0xAAAA3B2896B9E826ULL,
          "MPT1327: GTC PFIX=22 IDENT1=5949 D=0 CHAN=77 IDENT2=7080 N=0\n" },
        { 16, SYNC, 0xD7C392C7CD90B63AULL, 0xAAAAC4D7D7C392C7ULL,
          "MPT1327: GTC PFIX=87 IDENT1=6258 D=1 CHAN=399 IDENT2=4964 N=0\n" },
        { 32, SYNT, 0x8073123456789D81ULL, 0xAAAAAAAA3B288073ULL,
          "MPT1327: GTC PFIX=0 IDENT1=3682 D=1 CHAN=104 IDENT2=5534 N=0\n" },
        { 32, SYNC, 0x8080123456789A70ULL, 0xAAAAAAAAC4D78080ULL,
          "MPT1327: GTC PFIX=0 IDENT1=4098 D=1 CHAN=104 IDENT2=5534 N=0\n" },
    };
    unsigned int i;

    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        uint64_t corrected = cases[i].misleading;
        char expected[256];
        if (mpt1327_correct(&corrected) != 1 ||
            !mpt1327_valid(cases[i].address)) {
            fprintf(stderr, "invalid shadow-boundary fixture %u\n", i);
            failed = 1;
            return;
        }
        mpt1327_init(&s);
        printed[0] = '\0';
        feed(&s, 0xAAAA, 16);
        feed(&s, SYNC, 16);
        feed(&s, 0x8C0ACA3D8A9A901AULL, 64);
        if (cases[i].preamble_bits == 16)
            feed(&s, 0xAAAA, 16);
        else if (cases[i].preamble_bits == 32)
            feed(&s, 0xAAAAAAAAu, 32);
        feed(&s, cases[i].marker, 16);
        feed(&s, cases[i].address, 64);
        /* At EOF the two CRC-valid interpretations cannot be distinguished.
         * Provide enough bits to reject the misleading old alignment. */
        feed(&s, 0, 48);
        mpt1327_deinit(&s);
        snprintf(expected, sizeof(expected),
                 "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n%s",
                 cases[i].decoded);
        if (strcmp(printed, expected) != 0) {
            fprintf(stderr, "shadowed boundary %u: got \"%s\", expected \"%s\"\n",
                    i, printed, expected);
            failed = 1;
        }
    }
}

/* Deterministic property regression: varied DATA payloads, deliberate
 * SYNT/SYNC-like patterns, optional one-bit errors, and real channel/message
 * boundaries must preserve the exact validated codeword stream. */
static uint32_t mpt1327_test_random(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static uint64_t mpt1327_test_codeword(uint64_t info) {
    uint64_t raw = (info << 15) | (mpt1327_remainder(info) ^ 1);
    uint64_t tmp = raw;
    unsigned int parity = 0;
    while (tmp) {
        parity ^= (unsigned int)(tmp & 1);
        tmp >>= 1;
    }
    return (raw << 1) | parity;
}

static void check_ambiguous_streams(void) {
    static struct demod_state s;
    const uint64_t gtc = 0x8C0ACA3D8A9A901AULL;
    const uint64_t ccsc = 0x12344221AAAAC4D7ULL;
    uint32_t rng = 0x96B5AC13u;
    unsigned int i;

    for (i = 0; i < 1024; i++) {
        uint64_t info1, info2;
        uint64_t first, second;
        char expected[256];
        unsigned int offset = (i / 3) % 33;

        /* Each PRNG update must be sequenced separately (no unsequenced
         * modifications of rng across operands of a bitwise expression). */
        info1 = (uint64_t)mpt1327_test_random(&rng) << 32;
        info1 |= mpt1327_test_random(&rng);
        info1 &= 0x7FFFFFFFFFFFULL;
        info2 = (uint64_t)mpt1327_test_random(&rng) << 32;
        info2 |= mpt1327_test_random(&rng);
        info2 &= 0x7FFFFFFFFFFFULL;

        if (i % 3 == 0)
            info1 = (info1 & ~(0xFFFFULL << offset)) | (0x3B28ULL << offset);
        else if (i % 3 == 1)
            info2 = (info2 & ~(0xFFFFULL << offset)) | (0x3B28ULL << offset);
        else
            info1 = (info1 & ~(0xFFFFFFFFULL << (offset % 16))) |
                    (0xAAAAC4D7ULL << (offset % 16));
        first = mpt1327_test_codeword(info1);
        second = mpt1327_test_codeword(info2);
        if (i % 5 == 0)
            first ^= 1ULL << (mpt1327_test_random(&rng) % 64);
        if (i % 7 == 0)
            second ^= 1ULL << (mpt1327_test_random(&rng) % 64);

        mpt1327_init(&s);
        printed[0] = '\0';
        feed(&s, 0xAAAA, 16);
        feed(&s, SYNC, 16);
        feed(&s, gtc, 64);
        feed(&s, first, 64);
        feed(&s, second, 64);
        if (i & 1) {
            feed(&s, SYNT, 16);
            feed(&s, gtc, 64);
        } else {
            feed(&s, ccsc, 64);
        }
        mpt1327_deinit(&s);
        snprintf(expected, sizeof(expected),
                 "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n"
                 "MPT1327: DATA 0x%012" PRIx64 "\n"
                 "MPT1327: DATA 0x%012" PRIx64 "\n%s",
                 info1, info2, (i & 1) ?
                 "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n" :
                 "MPT1327: CCSC SYS=1234\n");
        if (strcmp(printed, expected) != 0) {
            fprintf(stderr, "ambiguous stream %u: got \"%s\", expected \"%s\"\n",
                    i, printed, expected);
            failed = 1;
            return;
        }
    }
}

/* An incidental marker at a codeword boundary can overlap two legitimate
 * words to form another checksum-perfect address. Do not discard the known
 * framing on a tie, including when both interpretations require correction. */
static void check_clean_boundary_collisions(void) {
    static struct demod_state s;
    static const struct {
        uint64_t first, second, false_address, marker_prefix;
        unsigned int prefix_bits;
        const char *description;
    } cases[] = {
        { 0x3B28A51439AB577AULL, 0x062C66085FDBB105ULL,
          0xA51439AB577A062CULL, SYNT, 16, "bare SYNT" },
        { 0xAAAAC4D7C5870A45ULL, 0xF51ACAB2CC951204ULL,
          0xC5870A45F51ACAB2ULL, 0xAAAAC4D7u, 32, "16-bit preamble + SYNC" },
        { 0xAAAA3B28A87E72C3ULL, 0x8D29192F40FCD4CAULL,
          0xA87E72C38D29192FULL, 0xAAAA3B28u, 32, "16-bit preamble + SYNT" },
        { 0xAAAAAAAA3B28C679ULL, 0x61FC6F9C0EEA2CA5ULL,
          0xC67961FC6F9C0EEAULL, 0xAAAAAAAA3B28ULL, 48, "32-bit preamble + SYNT" },
        { 0xAAAAAAAAC4D7C68AULL, 0x6DFCAAFB46302D43ULL,
          0xC68A6DFCAAFB4630ULL, 0xAAAAAAAAC4D7ULL, 48, "32-bit preamble + SYNC" }
    };
    const uint64_t address = 0x8C0ACA3D8A9A901AULL;
    unsigned int i, error;

    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        uint64_t first = cases[i].first, second = cases[i].second;
        uint64_t false_address = cases[i].false_address;
        unsigned int shift = cases[i].prefix_bits;
        char expected[256];

        if (!mpt1327_valid(first) || !mpt1327_valid(second) ||
            !mpt1327_valid(false_address) ||
            mpt1327_field(false_address, 1, 1) != 1 ||
            (first >> (64 - shift)) != cases[i].marker_prefix ||
            ((first << shift) | (second >> (64 - shift))) != false_address) {
            fprintf(stderr, "invalid collision fixture: %s\n", cases[i].description);
            failed = 1;
            return;
        }
        printed[0] = '\0';
        mpt1327_print(address);
        mpt1327_print(first);
        mpt1327_print(second);
        snprintf(expected, sizeof(expected), "%s", printed);

        for (error = 0; error <= 1; error++) {
            uint64_t on_air = first ^ (error ? 1ULL << 10 : 0);
            mpt1327_init(&s);
            printed[0] = '\0';
            feed(&s, 0xAAAA, 16);
            feed(&s, SYNC, 16);
            feed(&s, address, 64);
            feed(&s, on_air, 64);
            feed(&s, second, 64);
            mpt1327_deinit(&s);
            if (strcmp(printed, expected) != 0) {
                fprintf(stderr, "false %s framing (bit error=%u): got \"%s\", "
                        "expected \"%s\"\n", cases[i].description, error,
                        printed, expected);
                failed = 1;
            }
        }
    }
}

/* A valid old-aligned word must not become a false address at EOF merely
 * because a shifted, CRC-valid address is present before the next old word
 * completes.  Test EOF exactly when the competing word completes. */
static void check_ambiguous_eof_prefers_established(void) {
    static struct demod_state s;
    static const struct {
        uint64_t first, second;
        unsigned int shifted_bits;
        const char *name;
    } cases[] = {
        { 0x3B28A51439AB577AULL, 0x062C66085FDBB105ULL, 32, "bare SYNT" },
        { 0xAAAAC4D7C5870A45ULL, 0xF51ACAB2CC951204ULL, 32, "preamble SYNC" },
        { 0xAAAA3B28A87E72C3ULL, 0x8D29192F40FCD4CAULL, 32, "preamble SYNT" },
        { 0xAAAAAAAA3B28C679ULL, 0x61FC6F9C0EEA2CA5ULL, 16, "long preamble SYNT" },
        { 0xAAAAAAAAC4D7C68AULL, 0x6DFCAAFB46302D43ULL, 16, "long preamble SYNC" }
    };
    unsigned int i, bit_error;
    const uint64_t gtc = 0x8C0ACA3D8A9A901AULL;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        for (bit_error = 0; bit_error < 2; ++bit_error) {
            char expected[256];
            uint64_t first = cases[i].first;
            if (bit_error) first ^= 1ULL << 10;
            /* Format the two original words independently of arbitration;
             * some are addresses, others are DATA. */
            printed[0] = '\0';
            mpt1327_print(gtc);
            mpt1327_print(cases[i].first);
            snprintf(expected, sizeof(expected), "%s", printed);
            mpt1327_init(&s);
            printed[0] = '\0';
            feed(&s, 0xAAAA, 16);
            feed(&s, SYNC, 16);
            feed(&s, gtc, 64);
            feed(&s, first, 64);
            feed(&s, cases[i].second >> (64 - cases[i].shifted_bits),
                 (int)cases[i].shifted_bits);
            mpt1327_deinit(&s);
            if (strcmp(printed, expected) != 0) {
                fprintf(stderr, "ambiguous EOF %s (error=%u): got '%s', expected '%s'\n",
                        cases[i].name, bit_error, printed, expected);
                failed = 1;
            }
        }
    }
}

/* A failed tentative word may contain a later exact SYNC. Recover at that
 * marker rather than discarding the rest of its following codeword. */
static void check_sync_inside_failed_word(void) {
    static struct demod_state s;
    const uint64_t data = 0x0123456718671234ULL;
    const char *expected = "MPT1327: DATA 0x012345671867\n";

    if (!mpt1327_valid(data)) {
        fprintf(stderr, "invalid nested-sync fixture\n");
        failed = 1;
        return;
    }
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, SYNC, 16);
    feed(&s, 0, 32);
    feed(&s, SYNC, 16);
    feed(&s, data, 64);
    mpt1327_deinit(&s);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "nested SYNC recovery: got \"%s\", expected \"%s\"\n", printed, expected);
        failed = 1;
    }

    /* An earlier incidental SYNT must not prevent retrying the later SYNC
     * if the first candidate also fails its check bits. */
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, SYNC, 16);
    feed(&s, 0, 16);
    feed(&s, SYNT, 16);
    feed(&s, SYNC, 16);
    feed(&s, data, 64);
    mpt1327_deinit(&s);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "multiple nested markers: got \"%s\", expected \"%s\"\n", printed, expected);
        failed = 1;
    }
}

/* A marker-only acquisition may have one good word followed by corruption
 * or EOF. The bounded hold must not silently suppress that lone word. */
static void check_tentative_word_completion(void) {
    static struct demod_state s;
    const uint64_t gtc = 0x8C0ACA3D8A9A901AULL;
    const char *expected =
        "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n";

    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, SYNC, 16); /* no preamble: marker-only provisional alignment */
    feed(&s, gtc, 64);
    if (printed[0] != '\0') {
        fprintf(stderr, "premature provisional word: %s\n", printed);
        failed = 1;
    }
    mpt1327_deinit(&s);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "lost provisional word at EOF: %s\n", printed);
        failed = 1;
    }

    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, SYNC, 16);
    feed(&s, gtc, 64);
    feed(&s, 0, 64); /* invalid, marker-free next word */
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "lost provisional word after timeout: %s\n", printed);
        failed = 1;
    }
    mpt1327_deinit(&s);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "duplicate provisional word at EOF: %s\n", printed);
        failed = 1;
    }
}

/* Sync markers have no parity/CRC of their own. An errored SYNT must not
 * be guessed as a frame; the next exact SYNT should recover decoding. */
static void check_corrupted_sync_reacquisition(void) {
    static struct demod_state s;
    const uint64_t gtc = 0x8C0ACA3D8A9A901AULL;
    const char *expected =
        "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n";

    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, SYNT ^ 1, 16);
    feed(&s, gtc, 64);
    if (printed[0] != '\0') {
        fprintf(stderr, "guessed corrupted SYNT: %s\n", printed);
        failed = 1;
    }
    feed(&s, SYNT, 16);
    feed(&s, gtc, 64);
    mpt1327_deinit(&s);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "failed reacquisition after bad SYNT: %s\n", printed);
        failed = 1;
    }
}

/* The old alignment can be one-bit correctable by changing a bit of the
 * actual preamble/SYNT. Arbitration must inspect the received marker, not
 * the corrected data word, or the genuine corrected address is discarded. */
static void check_corrected_word_does_not_erase_sync(void) {
    static struct demod_state s;
    const uint64_t first = 0xE805DAD925DFEEEEULL;
    const uint64_t second = 0xC8604257E54C9FCCULL;
    const uint64_t second_errored = second ^ 0x400ULL;
    uint64_t overlapping = 0xAAAA3B28C8604257ULL;
    uint64_t repaired = second_errored;
    const char *expected =
        "MPT1327: GTC PFIX=104 IDENT1=187 D=1 CHAN=434 IDENT2=2423 N=3\n"
        "MPT1327: GTC PFIX=72 IDENT1=3080 D=1 CHAN=175 IDENT2=6483 N=0\n";
    const char *eof_ambiguous =
        "MPT1327: GTC PFIX=104 IDENT1=187 D=1 CHAN=434 IDENT2=2423 N=3\n"
        "MPT1327: GTC PFIX=42 IDENT1=5447 D=0 CHAN=593 IDENT2=4632 N=0\n";

    if (!mpt1327_valid(first) || mpt1327_correct(&overlapping) != 1 ||
        mpt1327_correct(&repaired) != 1 || repaired != second) {
        fprintf(stderr, "invalid corrected-marker collision fixture\n");
        failed = 1;
        return;
    }
    /* A clean new address beats a one-bit-corrected old-aligned word at
     * EOF; this is not the equal-confidence case below. */
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, 0xAAAA, 16); feed(&s, SYNT, 16); feed(&s, first, 64);
    feed(&s, 0xAAAA, 16); feed(&s, SYNT, 16); feed(&s, second, 64);
    mpt1327_deinit(&s);
    if (strcmp(printed, expected) != 0) {
        fprintf(stderr, "clean address versus corrected old word at EOF: got \"%s\"\n", printed);
        failed = 1;
    }
    /* Both overlapping words now require one correction, so the old
     * alignment wins at EOF. */
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, 0xAAAA, 16); feed(&s, SYNT, 16); feed(&s, first, 64);
    feed(&s, 0xAAAA, 16); feed(&s, SYNT, 16); feed(&s, second_errored, 64);
    mpt1327_deinit(&s);
    if (strcmp(printed, eof_ambiguous) != 0) {
        fprintf(stderr, "corrected marker collision at EOF: got \"%s\"\n", printed);
        failed = 1;
    }
    /* Continuing reception must also recover the next traffic message. */
    mpt1327_init(&s);
    printed[0] = '\0';
    feed(&s, 0xAAAA, 16); feed(&s, SYNT, 16); feed(&s, first, 64);
    feed(&s, 0xAAAA, 16); feed(&s, SYNT, 16); feed(&s, second_errored, 64);
    feed(&s, 0xAAAA, 16); feed(&s, SYNT, 16);
    feed(&s, 0x8C0ACA3D8A9A901AULL, 64);
    mpt1327_deinit(&s);
    if (strcmp(printed,
               "MPT1327: GTC PFIX=104 IDENT1=187 D=1 CHAN=434 IDENT2=2423 N=3\n"
               "MPT1327: GTC PFIX=72 IDENT1=3080 D=1 CHAN=175 IDENT2=6483 N=0\n"
               "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n") != 0) {
        fprintf(stderr, "corrected marker recovery: got \"%s\"\n", printed);
        failed = 1;
    }
}

int main(void) {
    /* CCSC of system 1234 hex, completion sequence by the algorithm of MPT 1327 appendix 3 */
    const uint64_t ccsc = 0x12344221AAAAC4D7ULL;
    const uint64_t dcsc = 0x1234013BAAAA3B28ULL;
    const uint64_t gtc = 0x8C0ACA3D8A9A901AULL;
    uint64_t cw;
    int a, b;

    /* All information bits zero: the remainder is zero, its last bit is inverted,
     * and the parity bit makes the count of ones even */
    if (!mpt1327_valid(0x3)) {
        fprintf(stderr, "codeword 3 not valid\n");
        failed = 1;
    }
    if (!mpt1327_valid(ccsc) || !mpt1327_is_ccsc(ccsc) || mpt1327_is_dcsc(ccsc)) {
        fprintf(stderr, "CCSC not recognised\n");
        failed = 1;
    }
    if (!mpt1327_valid(dcsc) || !mpt1327_is_dcsc(dcsc) || mpt1327_is_ccsc(dcsc)) {
        fprintf(stderr, "DCSC not recognised\n");
        failed = 1;
    }
    for (a = 0; a < 64; a++) {
        cw = gtc ^ (1ULL << a);
        if (mpt1327_correct(&cw) != 1 || cw != gtc) {
            fprintf(stderr, "single-bit error at bit %d not corrected\n", a);
            failed = 1;
        }
        for (b = a + 1; b < 64; b++) {
            cw = gtc ^ (1ULL << a) ^ (1ULL << b);
            if (mpt1327_correct(&cw) != -1) {
                fprintf(stderr, "double-bit error at bits %d, %d not rejected\n", a, b);
                failed = 1;
            }
        }
    }

    /* The helper is private and currently receives only 13-bit values,
     * but must remain bounds-safe if a future caller passes a larger one. */
    {
        char buf[16];
        if (strcmp(mpt1327_ident(buf, sizeof(buf), 8191), "ALLI") != 0 ||
            strcmp(mpt1327_ident(buf, sizeof(buf), 8192), "8192") != 0) {
            fprintf(stderr, "special ident lookup bounds are incorrect\n");
            failed = 1;
        }
    }
    check_line(ccsc, "MPT1327: CCSC SYS=1234\n");
    check_line(dcsc, "MPT1327: DCSC SYS=1234\n");
    check_line(gtc, "MPT1327: GTC PFIX=12 IDENT1=345 D=1 CHAN=123 IDENT2=678 N=2\n");
    check_line(0x80000402B800565EULL, "MPT1327: ALH PFIX=0 IDENT1=DUMMYI CHAN4=10 WT=7 M=0 N=0\n");
    check_line(0x8C0ACC5BFFD1B4DDULL, "MPT1327: AHYQ PFIX=12 IDENT1=345 IDENT2=TSCI STATUS=17\n");
    check_line(0x96190C70CC0004CEULL, "MPT1327: BCAST SYSDEF=5 SYS=4321 CHAN=204 ADJSITE=0\n");
    check_line(0x0123456789ABC13DULL, "MPT1327: DATA 0x0123456789ab\n");
    check_line(0x8C0ACC8400009340ULL, "MPT1327: CAT=001 PFIX=12 IDENT1=345 PARAMS=0x040000\n");
    check_line(0x8C0ACC1C0000FEECULL,
               "MPT1327: CAT=000 TYPE=00 FUNC=111 PFIX=12 IDENT1=345 PARAMS=0x00000\n");

    check_json(ccsc, "{\"demod_name\":\"MPT1327\",\"message\":\"CCSC\",\"sys\":\"1234\"}\n");
    check_json(dcsc, "{\"demod_name\":\"MPT1327\",\"message\":\"DCSC\",\"sys\":\"1234\"}\n");
    check_json(gtc, "{\"demod_name\":\"MPT1327\",\"message\":\"GTC\",\"pfix\":12,\"ident1\":345,"
                    "\"d\":1,\"chan\":123,\"ident2\":678,\"n\":2}\n");
    check_json(0x8C0ACC5BFFD1B4DDULL,
               "{\"demod_name\":\"MPT1327\",\"message\":\"AHYQ\",\"pfix\":12,"
               "\"ident1\":345,\"ident2\":8190,\"status\":17}\n");
    check_json(0x0123456789ABC13DULL,
               "{\"demod_name\":\"MPT1327\",\"message\":\"DATA\",\"info\":\"0x0123456789ab\"}\n");

    check_json(0x8C0ACC8400009340ULL,
               "{\"demod_name\":\"MPT1327\",\"message\":\"UNDECODED\",\"cat\":\"001\","
               "\"pfix\":12,\"ident1\":345,\"params\":\"0x040000\"}\n");
    check_json(0x8C0ACC1C0000FEECULL,
               "{\"demod_name\":\"MPT1327\",\"message\":\"UNDECODED\",\"cat\":\"000\","
               "\"type\":\"00\",\"func\":\"111\",\"pfix\":12,\"ident1\":345,"
               "\"params\":\"0x00000\"}\n");
    check_traffic_messages();
    check_system_codewords();
    check_json_all_functions();
    check_traffic_boundary_preempts_false_frame();
    check_ccsc_during_tentative_frame();
    check_bare_synt_supersedes_false_frame();
    check_incidental_synt_preserves_frame();
    check_overlapping_synt_candidates();
    check_synt_in_valid_codeword_trailer();
    check_contiguous_traffic_messages();
    check_synt_in_all_data_positions();
    check_incidental_preamble_preserves_frame();
    check_control_boundary_preempts_false_frame();
    check_deferred_end_of_input();
    check_ambiguous_data_overlaps();
    check_real_boundaries_shadowed_by_correctable_words();
    check_ambiguous_streams();
    check_clean_boundary_collisions();
    check_ambiguous_eof_prefers_established();
    check_explicit_boundary_lookahead();
    check_corrected_word_does_not_erase_sync();
    check_upstream_system_collisions();
    check_tentative_word_completion();
    check_corrupted_sync_reacquisition();
    check_sync_inside_failed_word();
    return failed;
}
