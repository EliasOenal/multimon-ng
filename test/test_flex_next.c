/* Standalone: cc -std=gnu11 -I. test/test_flex_next.c bch.c cJSON.c -lm -o test_flex_next */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multimon.h"

#undef assert
#define assert(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

static int fail_after = -1;
static int fail_alloc(void) {
    if (fail_after < 0) return 0;
    if (fail_after == 0) { fail_after = -1; return 1; }
    fail_after--;
    return 0;
}
static void *test_malloc(size_t n) { return fail_alloc() ? NULL : malloc(n); }
static void *test_realloc(void *p, size_t n) { return fail_alloc() ? NULL : realloc(p, n); }
static char output[2000000];
static size_t output_len;
static int capture_fprintf(FILE *stream, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n;
    if (stream == stdout) {
        n = vsnprintf(output + output_len, sizeof(output) - output_len, fmt, ap);
        assert(n >= 0 && (size_t)n < sizeof(output) - output_len);
        output_len += n;
    } else n = vfprintf(stream, fmt, ap);
    va_end(ap);
    return n;
}
#define malloc test_malloc
#define realloc test_realloc
#define fprintf capture_fprintf
#include "../demod_flex_next.c"
#undef malloc
#undef realloc
#undef fprintf

int json_mode;
int verb_level;
void _verbprintf(int level, const char *fmt, ...) {
    if (level > 0) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(output + output_len, sizeof(output) - output_len, fmt, ap);
    va_end(ap);
    assert(n >= 0 && (size_t)n < sizeof(output) - output_len);
    output_len += n;
}
static void clear_output(void) { output[0] = 0; output_len = 0; }
static uint32_t encoded(uint32_t data) {
    uint32_t w = bch_flex_encode(data);
    uint32_t bits = w, parity = 0;
    while (bits) { parity ^= bits & 1; bits >>= 1; }
    return w | (parity << 31);
}
static struct Flex_Next *decoder(void) {
    struct Flex_Next *f = Flex_New(FREQ_SAMP);
    assert(f);
    f->Sync.baud = 1600;
    f->Sync.levels = 2;
    return f;
}
/* One short address, one vector, then message words. */
static void page(struct Flex_Next *f, uint32_t capcode, int type, int frag, int cont, int n) {
    memset(&f->Data.PhaseA, 0, sizeof(f->Data.PhaseA));
    uint32_t words[PHASE_WORDS] = {0};
    words[0] = 2 << 10; /* BIW: one BIW, VF starts at word 2 */
    words[1] = capcode + 0x8000;
    words[2] = (type << 4) | (3 << 7) | (3 << 14);
    if (type == FLEX_PAGETYPE_SHORT_INSTRUCTION)
        words[2] = (type << 4) | (1 << 7) | (5 << 10);
    else if (type == FLEX_PAGETYPE_SHORT_MESSAGE)
        words[2] = (type << 4) | (2 << 7); /* tone */
    words[3] = (frag << 11) | (cont << 10) | (n << 13);
    words[4] = ('A' << 7) | ('%' << 14);
    words[5] = 'B' | (3 << 7) | (3 << 14);
    if (type == FLEX_PAGETYPE_BINARY) {
        words[3] = (frag << 13) | (cont << 12) | (n << 15);
        words[4] = 8 << 4;
        words[5] = 0x12345;
    }
    for (int i = 0; i < PHASE_WORDS; i++) f->Data.PhaseA.buf[i] = encoded(words[i]);
}
static cJSON *message(void) {
    const char *end;
    cJSON *j = cJSON_ParseWithOpts(output, &end, 0);
    assert(j);
    return j;
}
static void check_members(cJSON *j, int64_t member) {
    assert(cJSON_IsTrue(cJSON_GetObjectItem(j, "is_group")));
    cJSON *a = cJSON_GetObjectItem(j, "group_capcodes");
    assert(cJSON_GetArraySize(a) == 1);
    assert(cJSON_GetArrayItem(a, 0)->valuedouble == (double)member);
}
static void check_orphan(const char *payload) {
    if (json_mode) {
        cJSON *j = message();
        assert(cJSON_IsTrue(cJSON_GetObjectItem(j, "is_group")));
        cJSON *members = cJSON_GetObjectItem(j, "group_capcodes");
        assert(cJSON_IsArray(members) && cJSON_GetArraySize(members) == 0);
        assert(!strcmp(cJSON_GetObjectItem(j, "fragment")->valuestring, "fragment_orphan"));
        assert(strstr(cJSON_GetObjectItem(j, "message")->valuestring, payload));
        cJSON_Delete(j);
    } else {
        assert(strstr(output, "0002029568|T|"));
        assert(strstr(output, ".ORPHAN|"));
        assert(strstr(output, payload));
        assert(!strstr(output, "0000022222") && !strstr(output, "0000033333"));
    }
}
static void test_dispatch(void) {
    for (int mode = 0; mode < 2; mode++) {
        json_mode = mode;
        for (int type = 0; type <= 7; type++) for (int subtype = 0; subtype < (type == 0 ? 4 : 1); subtype++) {
            struct Flex_Next *f = decoder();
            flex_group_enroll(f, 0, 1, 12345);
            f->FIW.frameno = 1;
            page(f, FLEX_GROUP_ADDR_MIN, type, 3, 0, 7);
            if (type == 0)
                f->Data.PhaseA.buf[3] = encoded((f->Data.PhaseA.buf[3] & 0x1FFFFF) | ((uint32_t)subtype << 19));
            clear_output();
            decode_phase(f, 'A');
            assert(output_len);
            if (mode) {
                cJSON *j = message(); check_members(j, 12345);
                if (type == 0) {
                    const char *tags[] = {"SEC:ALN", "SEC:VEN", "SEC:BIN", "SEC:RSV"};
                    assert(!strcmp(cJSON_GetObjectItem(j, "type_tag")->valuestring, tags[subtype]));
                }
                cJSON_Delete(j);
            }
            else assert(strstr(output, "0002029568 0000012345|T|"));
            assert(f->GroupHandler.GroupFrame[0] == -1);
            assert(flex_group_endpoint(&f->GroupHandler, 0) == 0);
            Flex_Delete(f);
        }
    }
}
static void test_fragments(void) {
    const int types[] = {0, 5, 6};
    for (int mode = 0; mode < 2; mode++) for (int t = 0; t < 3; t++) {
        int type = types[t];
        json_mode = mode;
        struct Flex_Next *f = decoder();
        flex_group_enroll(f, 0, 1, 11111);
        f->FIW.frameno = 1;
        page(f, FLEX_GROUP_ADDR_MIN, type, 3, 1, 7);
        clear_output(); decode_phase(f, 'A');
        int slot = frag_find(f, FLEX_GROUP_ADDR_MIN, type, 7);
        assert(slot >= 0 && f->FragStore.slots[slot].members[0] == 11111);
        assert(f->GroupHandler.GroupDelivering[0]);
        /* Live membership changes cannot change the captured recipients. */
        f->GroupHandler.GroupCodes[0][1] = 99999;
        unsigned char body[FLEX_FRAG_MAX_LEN]; memset(body, 'X', sizeof(body));
        frag_append(f, slot, body, sizeof(body));
        page(f, FLEX_GROUP_ADDR_MIN, type, 0, 0, 7);
        clear_output(); decode_phase(f, 'A');
        if (mode) {
            cJSON *j = message(); check_members(j, 11111);
            assert(!strcmp(cJSON_GetObjectItem(j, "type_tag")->valuestring,
                           type == 0 ? "SEC:ALN" : type == 5 ? "ALN" : "HEX"));
            assert(strlen(cJSON_GetObjectItem(j, "message")->valuestring) > FLEX_FRAG_MAX_LEN);
            cJSON_Delete(j);
        } else assert(strstr(output, "0002029568 0000011111|T|"));
        assert(f->GroupHandler.GroupFrame[0] == -1);
        /* Reassignment with the same N must not join the old fragment. */
        flex_group_enroll(f, 0, 2, 22222);
        f->FIW.frameno = 2;
        page(f, FLEX_GROUP_ADDR_MIN, type, 3, 1, 7);
        clear_output(); decode_phase(f, 'A');
        slot = frag_find(f, FLEX_GROUP_ADDR_MIN, type, 7);
        assert(slot >= 0);
        uint64_t old_generation = f->grp_generation;
        flex_group_enroll(f, 0, 3, 33333);
        assert(f->GroupHandler.generation[0] != old_generation);
        assert(flex_group_snapshot(f, 1, 0));
        assert(frag_find(f, FLEX_GROUP_ADDR_MIN, type, 7) < 0);
        page(f, FLEX_GROUP_ADDR_MIN, type, 0, 0, 7);
        clear_output(); decode_phase(f, 'A');
        /* Preserve payload, but assert the exact empty list, not just absence of one member. */
        check_orphan(type == 6 ? "08000" : "A%B");
        assert(f->FragStore.slots[slot].active);
        assert(f->FragStore.slots[slot].member_count == 1);
        assert(f->FragStore.slots[slot].members[0] == 22222);
        assert(f->GroupHandler.GroupFrame[0] == 3 && !f->GroupHandler.failed[0]);
        clear_output();
        frag_expire(f, 132);
        if (mode) {
            cJSON *j = message(); check_members(j, 22222);
            assert(!strcmp(cJSON_GetObjectItem(j, "type_tag")->valuestring,
                           type == 0 ? "SEC:ALN" : type == 5 ? "ALN" : "HEX"));
            cJSON_Delete(j);
        } else assert(strstr(output, "0002029568 0000022222|T|"));
        assert(f->GroupHandler.GroupFrame[0] == 3);
        Flex_Delete(f);
    }
}
static void fiw(struct Flex_Next *f, unsigned int cycle, unsigned int frame) {
    uint32_t data = (cycle << 4) | (frame << 8);
    unsigned int sum = 0;
    for (int shift = 4; shift <= 20; shift += 4) sum += (data >> shift) & 15;
    f->FIW.rawdata = encoded(data | ((15 - sum) & 15));
    assert(decode_fiw(f) == 0);
}
static void test_bch_and_large_group(void) {
    struct Flex_Next *f = decoder();
    json_mode = 1;
    /* An uncorrectable first vector must consume its own slot, not the next. */
    uint32_t words[PHASE_WORDS] = {0};
    words[0] = 3 << 10;
    words[1] = 0x8000 + 111;
    words[2] = 0x8000 + 222;
    words[3] = (1 << 4) | (5 << 10);
    words[4] = (1 << 4) | (6 << 10) | (1 << 17);
    for (int i = 0; i < PHASE_WORDS; i++) f->Data.PhaseA.buf[i] = encoded(words[i]);
    f->Data.PhaseA.buf[3] ^= 7;
    clear_output(); decode_phase(f, 'A');
    assert(f->Data.PhaseA.bch_err[3]);
    assert(flex_group_endpoint(&f->GroupHandler, 0) == 0);
    assert(flex_group_endpoint(&f->GroupHandler, 1) == 1);
    assert(f->GroupHandler.GroupCodes[1][1] == 222);
    /* Correctable vector errors still enroll the intended recipient. */
    memset(f->Data.PhaseA.bch_err, 0, sizeof(f->Data.PhaseA.bch_err));
    for (int i = 0; i < PHASE_WORDS; i++) f->Data.PhaseA.buf[i] = encoded(words[i]);
    f->Data.PhaseA.buf[3] ^= 3;
    clear_output(); decode_phase(f, 'A');
    assert(!f->Data.PhaseA.bch_err[3]);
    assert(f->GroupHandler.GroupCodes[0][1] == 111);
    flex_group_reset(&f->GroupHandler, 0);
    assert(flex_group_ensure(&f->GroupHandler, 0, GROUP_CODE_COUNT));
    f->GroupHandler.GroupCodes[0][0] = GROUP_MAX_CODES;
    for (int i = 1; i <= GROUP_MAX_CODES; i++) f->GroupHandler.GroupCodes[0][i] = 4297000000LL + i;
    for (int mode = 0; mode < 2; mode++) {
        json_mode = mode;
        f->GroupHandler.GroupCodes[0][0] = GROUP_MAX_CODES;
        page(f, FLEX_GROUP_ADDR_MIN, 5, 3, 0, 7);
        clear_output(); decode_phase(f, 'A');
        if (mode) {
            cJSON *j = message();
            cJSON *a = cJSON_GetObjectItem(j, "group_capcodes");
            assert(cJSON_GetArraySize(a) == GROUP_MAX_CODES);
            assert(cJSON_GetArrayItem(a, GROUP_MAX_CODES - 1)->valuedouble == 4297000000LL + GROUP_MAX_CODES);
            cJSON_Delete(j);
        } else {
            char last[32]; snprintf(last, sizeof(last), " %010lld|T|", 4297000000LL + GROUP_MAX_CODES);
            assert(strstr(output, last));
            assert(strstr(output, "A%B") && !strstr(output, "A%%B"));
        }
    }
    Flex_Delete(f);
}
static void test_rollover_and_failures(void) {
    struct Flex_Next *f = decoder();
    f->FIW.cycleno = 14; f->FIW.frameno = 127;
    flex_group_enroll(f, 0, 1, 11);
    assert(f->GroupHandler.GroupCycle[0] == 0);
    uint64_t generation = f->GroupHandler.generation[0];
    flex_group_enroll(f, 0, 1, 11);
    assert(flex_group_endpoint(&f->GroupHandler, 0) == 1);
    fiw(f, 0, 0);
    assert(f->GroupHandler.GroupFrame[0] == 1);
    fiw(f, 0, 1);
    flex_group_enroll(f, 0, 1, 22);
    assert(f->GroupHandler.generation[0] != generation);
    assert(flex_group_endpoint(&f->GroupHandler, 0) == 1);
    fiw(f, 1, 2);
    assert(f->GroupHandler.GroupFrame[0] == -1);
    flex_group_enroll(f, 0, 3, 22);
    fail_after = 0;
    flex_group_enroll(f, 1, 2, 33);
    assert(f->GroupHandler.failed[1]);
    assert(!flex_group_snapshot(f, 1, 1));
    fail_after = 0;
    assert(!flex_group_snapshot(f, 1, 0));
    assert(f->grp_snapshot_count == 0);
    assert(flex_group_snapshot(f, 1, 0));
    fail_after = 0;
    assert(frag_alloc(f, FLEX_GROUP_ADDR_MIN, 5, 1, 1) == -1);
    fail_after = 0;
    assert(!flex_prefix(f, 123, 'A', 'S', NULL, 0));
    fail_after = 1;
    assert(!flex_prefix(f, 123, 'A', 'S', NULL, 0));
    assert(flex_prefix(f, 123, 'A', 'S', NULL, 0));
    assert(f->field_cap < 1024 && f->prefix_cap < 1024);
    assert(flextime_age_frames(0, 1919) == 1);
    flex_group_snapshot(f, 0, 0);
    f->Decode.phase = 'A'; f->Decode.addr_type = 'S'; f->line_type_tag = "ALN";
    json_mode = 0; clear_output();
    for (int i = 0; i < FLEX_FRAG_MAX_SLOTS; i++) {
        int s = frag_alloc(f, 100 + i, 5, i, i == 0 ? 1918 : 0);
        assert(s == i);
        frag_append(f, s, (unsigned char *)"partial", 7);
    }
    assert(frag_alloc(f, 999, 5, 1, 2) == 0);
    assert(strstr(output, "|0000000100|"));
    unsigned char chars[MAX_ALN] = {0};
    assert(add_ch('%', chars, 0) == 1 && chars[0] == '%');
    cJSON_Hooks hooks = {test_malloc, free};
    cJSON_InitHooks(&hooks);
    int64_t member = 12;
    for (int fail = 0; fail < 7; fail++) {
        cJSON *j = cJSON_CreateObject(); assert(j);
        fail_after = fail;
        assert(!flex_json_group(j, FLEX_GROUP_ADDR_MIN, &member, 1));
        fail_after = -1;
        cJSON_Delete(j);
    }
    cJSON_InitHooks(NULL);
    Flex_Delete(f);
    fail_after = 0; assert(!Flex_New(FREQ_SAMP));
}
static void test_group_eviction(void) {
    for (int mode = 0; mode < 2; mode++) {
        json_mode = mode;
        struct Flex_Next *f = decoder();
        flex_group_enroll(f, 0, 1, 44444);
        f->Decode.phase = 'A'; f->Decode.addr_type = 'T'; f->line_type_tag = "HEX";
        assert(flex_group_snapshot(f, 1, 0));
        int s = frag_alloc(f, FLEX_GROUP_ADDR_MIN, 6, 7, 1918);
        assert(s == 0);
        frag_append(f, s, (unsigned char *)"CAFE", 4);
        flex_group_enroll(f, 0, 2, 55555);
        flex_group_snapshot(f, 0, 0);
        for (int i = 1; i < FLEX_FRAG_MAX_SLOTS; i++)
            assert(frag_alloc(f, i, 5, i, 0) == i);
        clear_output();
        assert(frag_alloc(f, 999, 5, 0, 2) == 0);
        if (mode) {
            cJSON *j = message(); check_members(j, 44444);
            assert(!strcmp(cJSON_GetObjectItem(j, "type_tag")->valuestring, "HEX"));
            cJSON_Delete(j);
        } else assert(strstr(output, "0002029568 0000044444|T|HEX|"));
        assert(f->GroupHandler.GroupFrame[0] == 2);
        assert(f->GroupHandler.GroupCodes[0][1] == 55555);
        Flex_Delete(f);
    }
}
static void test_failed_dispatch(void) {
    struct Flex_Next *f = decoder();
    json_mode = 0;
    flex_group_enroll(f, 0, 1, 12345);
    page(f, FLEX_GROUP_ADDR_MIN, 5, 3, 0, 7);
    fail_after = 0;
    clear_output(); decode_phase(f, 'A');
    assert(!output_len); /* Snapshot failure cannot emit an empty member list. */
    assert(flex_group_snapshot(f, 1, 0));
    assert(flex_prefix(f, FLEX_GROUP_ADDR_MIN, 'A', 'T', f->grp_snapshot, f->grp_snapshot_count));
    page(f, FLEX_GROUP_ADDR_MIN, 5, 3, 1, 7);
    fail_after = 0;
    clear_output(); decode_phase(f, 'A');
    assert(!output_len && f->GroupHandler.failed[0]);
    assert(!f->FragStore.slots[0].active);
    page(f, FLEX_GROUP_ADDR_MIN, 5, 0, 0, 7);
    clear_output(); decode_phase(f, 'A');
    assert(!output_len);
    Flex_Delete(f);
}
static void test_tone_only(void) {
    struct Flex_Next *f = decoder();
    flex_group_enroll(f, 0, 1, 12345);
    uint32_t words[PHASE_WORDS] = {0};
    words[0] = 63 << 10;
    for (int i = 1; i <= 25; i++) words[i] = 0x8000 + 100 + i;
    words[26] = 0x8000 + FLEX_GROUP_ADDR_MIN;
    for (int i = 63; i < PHASE_WORDS; i++) words[i] = (2 << 4) | (2 << 7);
    for (int i = 0; i < PHASE_WORDS; i++) f->Data.PhaseA.buf[i] = encoded(words[i]);
    json_mode = 1;
    clear_output(); decode_phase(f, 'A');
    const char *p = output;
    int found = 0;
    for (;;) {
        cJSON *j = cJSON_ParseWithOpts(p, &p, 0);
        if (!j) break;
        cJSON *tag = cJSON_GetObjectItem(j, "type_tag");
        if (tag && !strcmp(tag->valuestring, "TON")) { check_members(j, 12345); found++; }
        cJSON_Delete(j);
    }
    assert(found == 1 && f->GroupHandler.GroupFrame[0] == -1);
    Flex_Delete(f);
}
static void test_partial_metadata(void) {
    struct Flex_Next *f = decoder();
    json_mode = 1;
    f->Decode.phase = 'A'; f->Decode.addr_type = 'O';
    f->Decode.is_priority = 1;
    f->Decode.sec_subtype = "alpha";
    f->Decode.opr_category = "SysMsg";
    f->line_type_tag = "SEC:ALN";
    int slot = frag_alloc(f, 2029584, 0, 7, 0);
    assert(slot >= 0);
    frag_append(f, slot, (const unsigned char *)"old", 3);
    f->Decode.is_priority = 0;
    f->Decode.sec_subtype = "binary";
    f->Decode.opr_category = NULL;
    clear_output(); frag_partial(f, slot);
    cJSON *j = message();
    assert(cJSON_IsTrue(cJSON_GetObjectItem(j, "is_priority")));
    assert(!strcmp(cJSON_GetObjectItem(j, "sec_subtype")->valuestring, "alpha"));
    assert(!strcmp(cJSON_GetObjectItem(j, "opr_category")->valuestring, "SysMsg"));
    assert(!f->Decode.is_priority && !strcmp(f->Decode.sec_subtype, "binary"));
    cJSON_Delete(j);
    Flex_Delete(f);
}
static void test_instruction_failures(void) {
    /* Fail each text prefix buffer, then deliver the same assignment's two members. */
    for (int failure = 0; failure < 2; failure++) {
        struct Flex_Next *f = decoder();
        json_mode = 0;
        page(f, 11111, 1, 3, 0, 0);
        f->Data.PhaseA.buf[2] = encoded((1 << 4) | (5 << 10));
        fail_after = failure;
        clear_output(); decode_phase(f, 'A');
        assert(!output_len && !f->GroupHandler.failed[0]);
        assert(flex_group_endpoint(&f->GroupHandler, 0) == 1);
        page(f, 22222, 1, 3, 0, 0);
        f->Data.PhaseA.buf[2] = encoded((1 << 4) | (5 << 10));
        clear_output(); decode_phase(f, 'A');
        f->FIW.frameno = 5;
        page(f, FLEX_GROUP_ADDR_MIN, 5, 3, 0, 7);
        clear_output(); decode_phase(f, 'A');
        assert(strstr(output, "0002029568 0000011111 0000022222|T|"));
        Flex_Delete(f);
    }
    /* A receiving-group snapshot failure must not lose the target enrollment. */
    for (int mode = 0; mode < 2; mode++) {
        struct Flex_Next *f = decoder();
        json_mode = mode;
        flex_group_enroll(f, 0, 1, 11111);
        page(f, FLEX_GROUP_ADDR_MIN, 1, 3, 0, 0);
        f->Data.PhaseA.buf[2] = encoded((1 << 4) | (5 << 10) | (7 << 17));
        fail_after = 0;
        clear_output(); decode_phase(f, 'A');
        if (mode) assert(!strstr(output, "\"instruction\""));
        else assert(!output_len);
        assert(f->GroupHandler.GroupFrame[0] == -1);
        assert(flex_group_endpoint(&f->GroupHandler, 7) == 1);
        assert(f->GroupHandler.GroupCodes[7][1] == FLEX_GROUP_ADDR_MIN);
        Flex_Delete(f);
    }
}
static void test_instruction_target(void) {
    json_mode = 1;
    for (int group = 0; group < 2; group++) for (int instr = 0; instr < 3; instr++) {
        struct Flex_Next *f = decoder();
        if (group) flex_group_enroll(f, 0, 1, 11111);
        page(f, group ? FLEX_GROUP_ADDR_MIN : 11111, 1, 3, 0, 0);
        f->Data.PhaseA.buf[2] = encoded((1 << 4) | (instr << 7) | (5 << 10) | (7 << 17));
        clear_output(); decode_phase(f, 'A');
        cJSON *j = message();
        assert(cJSON_GetObjectItem(j, "target_group_slot")->valueint == 7);
        assert(cJSON_GetObjectItem(j, "group_slot")->valueint == (group ? 0 : 7));
        if (group) check_members(j, 11111);
        cJSON_Delete(j);
        Flex_Delete(f);
    }
}
static void test_orphan_groups(void) {
    const int types[] = {5, 6, 0, 0, 0, 0};
    const int subtypes[] = {0, 0, 0, 1, 2, 3};
    for (int mode = 0; mode < 2; mode++) for (int t = 0; t < 6; t++)
    for (int assigned = 0; assigned < 3; assigned++) {
        struct Flex_Next *f = decoder();
        json_mode = mode;
        const char *payload = types[t] == 6 ? "08000" :
                              types[t] == 0 && subtypes[t] != 0 ? "08069" : "A%B";
        if (assigned) flex_group_enroll(f, 0, 1, 22222);
        int old_slot = -1;
        if (assigned == 2) {
            page(f, FLEX_GROUP_ADDR_MIN, types[t], 3, 1, 7);
            if (types[t] == 0)
                f->Data.PhaseA.buf[3] = encoded((f->Data.PhaseA.buf[3] & 0x1FFFFF) | (subtypes[t] << 19));
            clear_output(); decode_phase(f, 'A');
            old_slot = frag_find(f, FLEX_GROUP_ADDR_MIN, types[t], 7);
            assert(old_slot >= 0);
            flex_group_enroll(f, 0, 2, 33333);
        }
        uint64_t generation = f->GroupHandler.generation[0];
        /* Middle and final pieces must neither join old data nor change the live assignment. */
        for (int cont = 1; cont >= 0; cont--) {
            page(f, FLEX_GROUP_ADDR_MIN, types[t], 0, cont, 7);
            if (types[t] == 0)
                f->Data.PhaseA.buf[3] = encoded((f->Data.PhaseA.buf[3] & 0x1FFFFF) | (subtypes[t] << 19));
            clear_output(); decode_phase(f, 'A');
            check_orphan(payload);
            assert(f->GroupHandler.generation[0] == generation);
            assert(f->GroupHandler.GroupFrame[0] == (assigned ? assigned : -1));
            assert(!f->GroupHandler.failed[0] && !f->GroupHandler.GroupDelivering[0]);
            for (int i = 0; i < FLEX_FRAG_MAX_SLOTS; i++)
                assert(f->FragStore.slots[i].active == (i == old_slot));
            if (!cont && !assigned) {
                /* Unknown middle pieces cannot become attributed after enrollment. */
                flex_group_enroll(f, 0, 3, 33333);
                page(f, FLEX_GROUP_ADDR_MIN, types[t], 1, 0, 7);
                if (types[t] == 0)
                    f->Data.PhaseA.buf[3] = encoded((f->Data.PhaseA.buf[3] & 0x1FFFFF) | (subtypes[t] << 19));
                clear_output(); decode_phase(f, 'A');
                check_orphan(payload);
                assert(f->GroupHandler.GroupFrame[0] == 3);
            }
        }
        Flex_Delete(f);
    }
}
static void test_unassigned_initial_groups(void) {
    const int types[] = {5, 6, 0, 0, 0, 0};
    const int subtypes[] = {0, 0, 0, 1, 2, 3};
    const char *tags[] = {"ALN", "HEX", "SEC:ALN", "SEC:VEN", "SEC:BIN", "SEC:RSV"};
    for (int mode = 0; mode < 2; mode++) for (int t = 0; t < 6; t++) {
        struct Flex_Next *f = decoder();
        json_mode = mode;
        const char *payload = types[t] == 6 ? "08000" :
                              types[t] == 0 && subtypes[t] != 0 ? "08069" : "A%B";
        /* Reception can start after enrollment, before the initial fragment. */
        for (int piece = 0; piece < 4; piece++) {
            int frag = piece == 0 || piece == 3 ? 3 : piece - 1;
            page(f, FLEX_GROUP_ADDR_MIN, types[t], frag, piece < 2, piece == 3 ? 8 : 7);
            if (types[t] == 0)
                f->Data.PhaseA.buf[3] = encoded((f->Data.PhaseA.buf[3] & 0x1FFFFF) | (subtypes[t] << 19));
            clear_output(); decode_phase(f, 'A');
            assert(f->GroupHandler.GroupFrame[0] == -1);
            assert(!f->GroupHandler.failed[0] && !f->GroupHandler.GroupDelivering[0]);
            if (piece == 1 || piece == 2) check_orphan(payload);
            if (mode) {
                cJSON *j = message();
                cJSON *members = cJSON_GetObjectItem(j, "group_capcodes");
                cJSON *body = cJSON_GetObjectItem(j, "message");
                assert(cJSON_IsArray(members) && cJSON_GetArraySize(members) == 0);
                assert(cJSON_IsString(body) && body->valuestring[0]);
                assert(!strcmp(cJSON_GetObjectItem(j, "type_tag")->valuestring, tags[t]));
                cJSON_Delete(j);
            } else assert(strstr(output, "0002029568|T|"));
        }
        Flex_Delete(f);
    }
}
int main(void) {
    test_dispatch();
    test_fragments();
    test_rollover_and_failures();
    test_bch_and_large_group();
    test_group_eviction();
    test_failed_dispatch();
    test_tone_only();
    test_partial_metadata();
    test_instruction_failures();
    test_instruction_target();
    test_orphan_groups();
    test_unassigned_initial_groups();
    puts("FLEX_NEXT decoder unit tests passed");
    return 0;
}
