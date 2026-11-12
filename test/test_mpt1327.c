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
    return failed;
}
