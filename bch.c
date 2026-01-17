/*
 * bch.c - BCH(31,21,2) Error Correction for FLEX and POCSAG
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

#include "bch.h"
#include <string.h>

/*
 * BCH(31,21,2) can correct up to 2 bit errors in a 31-bit codeword.
 * 
 * FLEX uses GF(2^5) with primitive polynomial x^5+x^2+1 (0x25).
 * Error correction uses syndromes S1 (alpha^i) and S3 (alpha^3i).
 * 
 * POCSAG uses generator polynomial 0x769 (octal 03551) with simple
 * polynomial division for syndrome calculation.
 * 
 * All lookup tables are computed once at initialization for O(1) runtime.
 */

/* BCH code parameters */
#define BCH_DATA_BITS   21
#define BCH_PARITY_BITS 10
#define BCH_CODE_LEN    31  /* 2^5 - 1 */

/* Primitive polynomial for GF(2^5): x^5 + x^2 + 1 */
#define FLEX_PRIM_POLY  0x25

/* Generator polynomial for POCSAG (octal 03551) */
#define POCSAG_POLY     0x769

/* GF(2^5) field tables - computed at init */
static unsigned char flex_exp_tbl[32];      /* alpha^i -> polynomial representation */
static unsigned char flex_log_tbl[32];      /* inverse: polynomial -> exponent */

/* Generator polynomial for FLEX BCH - coefficients are GF(2^5) elements */
static unsigned char flex_gen_poly[BCH_PARITY_BITS + 1];

/* Syndrome tables for FLEX - computed at init from exp table */
static unsigned char flex_s1_tbl[BCH_CODE_LEN];  /* alpha^i for syndrome S1 */
static unsigned char flex_s3_tbl[BCH_CODE_LEN];  /* alpha^(3i) for syndrome S3 */

/* 
 * FLEX parity table - computed at init from generator polynomial.
 * flex_parity_tbl[i] = 10-bit parity when only data bit i is set.
 * Encoding: parity = XOR of flex_parity_tbl[i] for all set data bits.
 */
static unsigned short flex_parity_tbl[BCH_DATA_BITS];

/* 
 * POCSAG parity table - computed at init from generator polynomial.
 * pocsag_parity_tbl[i] = 10-bit BCH parity when only data bit i is set.
 * Encoding: parity = XOR of pocsag_parity_tbl[i] for all set data bits.
 */
static unsigned short pocsag_parity_tbl[BCH_DATA_BITS];

/* Error correction lookup tables - computed at init */
static unsigned int flex_err_tbl[1024];     /* (S1<<5)|S3 -> error pattern */
static unsigned int pocsag_err_tbl[2048];   /* syndrome -> error pattern */

/* Single-bit syndrome tables for efficient table building and syndrome computation */
static unsigned int flex_bit_key[BCH_CODE_LEN];  /* (S1<<5)|S3 for single bit */
static unsigned short pocsag_syn_tbl[32];        /* 10-bit BCH syndrome for single bit */

static int bch_initialized = 0;

/* ========== Utility Functions ========== */

static inline unsigned int parity32(unsigned int x)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_parity(x);
#else
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return x & 1;
#endif
}

static inline int popcount32(unsigned int x)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcount(x);
#else
    x = x - ((x >> 1) & 0x55555555);
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    x = (x + (x >> 4)) & 0x0f0f0f0f;
    return (x * 0x01010101) >> 24;
#endif
}

/* Count trailing zeros - position of lowest set bit */
static inline int ctz32(unsigned int x)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctz(x);
#else
    /* De Bruijn sequence method */
    static const int debruijn32[32] = {
        0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
        31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
    };
    return debruijn32[((x & -x) * 0x077CB531u) >> 27];
#endif
}

/* ========== FLEX Implementation ========== */

/*
 * FLEX codeword layout (31 bits):
 *   Bits 0-20:  Data (21 bits)
 *   Bits 21-30: Parity (10 bits)
 *
 * For FLEX, recv[i] in BCHCode.c corresponds to bit (30-i) of the uint32_t.
 * So bit 30 of uint32_t = recv[0], bit 0 of uint32_t = recv[30].
 */

/* Compute FLEX syndrome key (S1<<5)|S3 using GF(2^5) field tables */
static unsigned int flex_syndrome_key(unsigned int codeword)
{
    unsigned int s1 = 0, s3 = 0;
    
    /* Iterate only over set bits for efficiency */
    while (codeword) {
        int bit = ctz32(codeword);
        int recv_idx = 30 - bit;
        s1 ^= flex_s1_tbl[recv_idx];
        s3 ^= flex_s3_tbl[recv_idx];
        codeword &= codeword - 1;  /* Clear lowest set bit */
    }
    
    return (s1 << 5) | s3;
}

unsigned int bch_flex_encode(unsigned int data)
{
    if (!bch_initialized)
        bch_init();
    
    unsigned int parity = 0;
    unsigned int d = data & 0x1FFFFF;  /* Mask to 21 bits */
    
    /* Iterate only over set bits for efficiency */
    while (d) {
        int bit = ctz32(d);
        parity ^= flex_parity_tbl[bit];
        d &= d - 1;  /* Clear lowest set bit */
    }
    
    return (data & 0x1FFFFF) | (parity << BCH_DATA_BITS);
}

int bch_flex_correct(unsigned int *codeword)
{
    if (!bch_initialized)
        bch_init();
    
    unsigned int key = flex_syndrome_key(*codeword);
    
    if (key == 0)
        return 0;
    
    unsigned int error = flex_err_tbl[key];
    
    if (error == 0)
        return -1;
    
    *codeword ^= error;
    return popcount32(error);
}

/* ========== POCSAG Implementation ========== */

/*
 * POCSAG codeword layout (32 bits):
 *   Bits 31-11: Data (21 bits)
 *   Bits 10-1:  BCH parity (10 bits)
 *   Bit 0:      Overall even parity
 */

/* Compute POCSAG 11-bit syndrome using lookup table */
static unsigned int pocsag_syndrome(unsigned int codeword)
{
    unsigned int syn = 0;
    
    /* Strip parity bit and compute BCH syndrome over bits 1-31 */
    unsigned int bits = codeword >> 1;
    while (bits) {
        int bit = ctz32(bits);
        syn ^= pocsag_syn_tbl[bit];
        bits &= bits - 1;  /* Clear lowest set bit */
    }
    
    /* Add overall parity bit to syndrome (bit 10) */
    if (parity32(codeword))
        syn |= 0x400;
    
    return syn;
}

unsigned int bch_pocsag_encode(unsigned int data)
{
    if (!bch_initialized)
        bch_init();
    
    unsigned int d = data & 0x1FFFFF;  /* Mask to 21 bits */
    unsigned int parity = 0;
    
    /* Compute BCH parity using lookup table - XOR contributions for set bits */
    unsigned int tmp = d;
    while (tmp) {
        int bit = ctz32(tmp);
        parity ^= pocsag_parity_tbl[bit];
        tmp &= tmp - 1;  /* Clear lowest set bit */
    }
    
    /* Pack into POCSAG codeword format: data[31:11], parity[10:1], even_parity[0] */
    unsigned int codeword = (d << (BCH_PARITY_BITS + 1)) | (parity << 1);
    codeword |= parity32(codeword);  /* Even parity in bit 0 */
    
    return codeword;
}

int bch_pocsag_correct(unsigned int *codeword)
{
    if (!bch_initialized)
        bch_init();
    
    unsigned int syndrome = pocsag_syndrome(*codeword);
    
    if (syndrome == 0)
        return 0;
    
    unsigned int error = pocsag_err_tbl[syndrome];
    
    if (error == 0)
        return -1;
    
    *codeword ^= error;
    return popcount32(error);
}

/* ========== Initialization ========== */

/*
 * Multiply two elements in GF(2^5).
 * Uses log/exp tables: a * b = exp(log(a) + log(b))
 */
static unsigned char gf_mult(unsigned char a, unsigned char b)
{
    if (a == 0 || b == 0)
        return 0;
    return flex_exp_tbl[(flex_log_tbl[a] + flex_log_tbl[b]) % BCH_CODE_LEN];
}

/* Build GF(2^5) exp/log tables and syndrome tables */
static void build_gf_tables(void)
{
    /* Build exp table: alpha^i -> polynomial representation */
    unsigned int elem = 1;
    for (int i = 0; i < BCH_CODE_LEN; i++) {
        flex_exp_tbl[i] = elem;
        flex_log_tbl[elem] = i;
        elem <<= 1;
        if (elem & 0x20)  /* x^5 term - reduce mod primitive poly */
            elem ^= FLEX_PRIM_POLY;
    }
    flex_exp_tbl[BCH_CODE_LEN] = flex_exp_tbl[0];  /* Wrap around */
    flex_log_tbl[0] = 0;  /* log(0) undefined, set to 0 for safety */
    
    /* Build syndrome tables: S1 = alpha^i, S3 = alpha^(3i) */
    for (int i = 0; i < BCH_CODE_LEN; i++) {
        flex_s1_tbl[i] = flex_exp_tbl[i];
        flex_s3_tbl[i] = flex_exp_tbl[(3 * i) % BCH_CODE_LEN];
    }
}

/*
 * Build the BCH generator polynomial g(x) from its roots.
 * 
 * For BCH(31,21,2), the roots are alpha^1, alpha^2, alpha^3, alpha^4
 * and their conjugates (cyclotomic cosets). This gives a degree-10
 * polynomial with coefficients in GF(2^5).
 * 
 * g(x) = product of (x - alpha^i) for all roots
 */
static void build_generator_poly(void)
{
    int seen[32] = {0};
    int roots[BCH_PARITY_BITS];
    int num_roots = 0;
    
    /* Find roots: alpha^1 through alpha^4 and their conjugates */
    for (int r = 1; r <= 4; r++) {  /* 2t = 4 for t=2 error correction */
        int val = r;
        while (!seen[val]) {
            seen[val] = 1;
            roots[num_roots++] = val;
            val = (val * 2) % BCH_CODE_LEN;  /* Next conjugate */
        }
    }
    
    /* Initialize g(x) = 1 */
    flex_gen_poly[0] = 1;
    for (int i = 1; i <= BCH_PARITY_BITS; i++)
        flex_gen_poly[i] = 0;
    
    /* Multiply g(x) by (x - alpha^root) for each root */
    /* In GF(2^m), subtraction = addition, so (x - a) = (x + a) */
    int degree = 0;
    for (int r = 0; r < num_roots; r++) {
        unsigned char alpha_root = flex_exp_tbl[roots[r]];
        
        /* Multiply: g(x) = g(x) * (x + alpha^root) */
        /* New coefficient[j] = old[j-1] + old[j] * alpha^root */
        for (int j = degree + 1; j > 0; j--) {
            flex_gen_poly[j] = flex_gen_poly[j - 1] ^ 
                               gf_mult(flex_gen_poly[j], alpha_root);
        }
        flex_gen_poly[0] = gf_mult(flex_gen_poly[0], alpha_root);
        degree++;
    }
}

/*
 * Build FLEX parity table using LFSR encoding with generator polynomial.
 * 
 * For each data bit position, simulate encoding a codeword with only
 * that bit set. The resulting parity bits form the table entry.
 * 
 * The LFSR processes data bits MSB first (bit 20 down to bit 0).
 * Feedback XORs with generator polynomial when the MSB of shift register is 1.
 * 
 * Note: The parity table uses a specific bit ordering convention that matches
 * how bch_flex_encode() packs data and parity into the codeword.
 */
static void build_flex_parity_table(void)
{
    for (int databit = 0; databit < BCH_DATA_BITS; databit++) {
        unsigned char bb[BCH_PARITY_BITS] = {0};  /* Shift register */
        
        /* Process data bits from MSB (bit 20) to LSB (bit 0) */
        for (int i = BCH_DATA_BITS - 1; i >= 0; i--) {
            /* Input bit - use reversed index to match expected convention */
            int input = (i == (BCH_DATA_BITS - 1 - databit)) ? 1 : 0;
            int feedback = input ^ bb[BCH_PARITY_BITS - 1];
            
            /* Shift register with conditional XOR based on generator poly */
            if (feedback != 0) {
                for (int j = BCH_PARITY_BITS - 1; j > 0; j--) {
                    if (flex_gen_poly[j] != 0)
                        bb[j] = bb[j - 1] ^ feedback;
                    else
                        bb[j] = bb[j - 1];
                }
                bb[0] = flex_gen_poly[0] && feedback;
            } else {
                for (int j = BCH_PARITY_BITS - 1; j > 0; j--) {
                    bb[j] = bb[j - 1];
                }
                bb[0] = 0;
            }
        }
        
        /* Convert shift register to 10-bit parity value with bit reversal */
        unsigned int parity = 0;
        for (int i = 0; i < BCH_PARITY_BITS; i++) {
            if (bb[i])
                parity |= (1u << (BCH_PARITY_BITS - 1 - i));  /* Reverse bit order */
        }
        flex_parity_tbl[databit] = parity;
    }
}

void bch_init(void)
{
    if (bch_initialized)
        return;
    
    /* ===== Build GF(2^5) field tables ===== */
    build_gf_tables();
    
    /* ===== Build FLEX generator polynomial and parity table ===== */
    build_generator_poly();
    build_flex_parity_table();
    
    /* ===== Build POCSAG parity table ===== */
    /* Compute parity for each single data bit using polynomial division */
    for (int databit = 0; databit < BCH_DATA_BITS; databit++) {
        /* Data bit i maps to bit (i + 11) in the codeword (after shifting) */
        unsigned int shreg = 1u << (databit + BCH_PARITY_BITS);
        for (int i = BCH_DATA_BITS - 1; i >= 0; i--) {
            if (shreg & (1u << (i + BCH_PARITY_BITS)))
                shreg ^= (POCSAG_POLY << i);
        }
        pocsag_parity_tbl[databit] = shreg & 0x3FF;
    }
    
    /* ===== Clear error lookup tables ===== */
    memset(flex_err_tbl, 0, sizeof(flex_err_tbl));
    memset(pocsag_err_tbl, 0, sizeof(pocsag_err_tbl));
    
    /* ===== FLEX: Build error correction table ===== */
    for (int bit = 0; bit < BCH_CODE_LEN; bit++) {
        int recv_idx = 30 - bit;
        unsigned int s1 = flex_s1_tbl[recv_idx];
        unsigned int s3 = flex_s3_tbl[recv_idx];
        unsigned int key = (s1 << 5) | s3;
        flex_bit_key[bit] = key;
        flex_err_tbl[key] = 1u << bit;
    }
    /* Two-bit errors: XOR the single-bit keys */
    for (int i = 0; i < BCH_CODE_LEN; i++) {
        for (int j = i + 1; j < BCH_CODE_LEN; j++) {
            unsigned int key = flex_bit_key[i] ^ flex_bit_key[j];
            if (flex_err_tbl[key] == 0)
                flex_err_tbl[key] = (1u << i) | (1u << j);
        }
    }
    
    /* ===== POCSAG: Build syndrome lookup table ===== */
    /* Compute BCH syndrome for each single bit position using polynomial division */
    for (int bit = 0; bit < 31; bit++) {
        unsigned int shreg = 1u << bit;
        for (int i = BCH_DATA_BITS - 1; i >= 0; i--) {
            if (shreg & (1u << (i + BCH_PARITY_BITS)))
                shreg ^= (POCSAG_POLY << i);
        }
        pocsag_syn_tbl[bit] = shreg & 0x3FF;
    }
    
    /* ===== POCSAG: Build error correction table ===== */
    /* Single-bit errors (bits 1-31, not bit 0 which is parity) */
    for (int i = 1; i < 32; i++) {
        /* Syndrome includes parity bit check: bit i error + parity error = syndrome | 0x400 */
        unsigned int syn = pocsag_syn_tbl[i - 1] | 0x400;  /* Single bit always causes parity error */
        pocsag_err_tbl[syn] = 1u << i;
    }
    /* Two-bit errors: XOR the single-bit syndromes (parity cancels out) */
    for (int i = 1; i < 32; i++) {
        for (int j = i + 1; j < 32; j++) {
            unsigned int syn = pocsag_syn_tbl[i - 1] ^ pocsag_syn_tbl[j - 1];  /* No parity bit */
            if (pocsag_err_tbl[syn] == 0)
                pocsag_err_tbl[syn] = (1u << i) | (1u << j);
        }
    }
    
    bch_initialized = 1;
}
