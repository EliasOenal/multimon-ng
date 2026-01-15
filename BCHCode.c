/*
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

#include <stdlib.h>
#include "BCHCode.h"

/*
 * This implements a BCH(n,k,t) decoder over GF(2^m).
 * For FLEX paging: n=31, k=21, t=2, m=5
 *
 * Field elements are represented both as polynomials (for arithmetic)
 * and as powers of the primitive element alpha (for table lookups).
 */

struct BCHCode {
    int field_order;        /* m: defines GF(2^m) */
    int code_len;           /* n: codeword bits */
    int data_len;           /* k: information bits */
    int correct_cap;        /* t: max correctable errors */
    int parity_len;         /* n - k: check bits */
    int *exp_tbl;           /* alpha^i -> polynomial */
    int *log_tbl;           /* polynomial -> i where alpha^i = poly */
    int *gen_poly;          /* generator polynomial */
    int *bb;                /* parity (redundant) bits output from encoder */
    int syn_tbl[4][32];     /* Precomputed syndrome contributions per bit position */
};

/* Build GF(2^m) exp and log tables from primitive polynomial */
static void build_field_tables(struct BCHCode *ctx, const int *prim_poly)
{
    int i, elem;

    elem = 1;

    for (i = 0; i < ctx->code_len; i++) {
        ctx->exp_tbl[i] = elem;
        ctx->log_tbl[elem] = i;
        
        /* Multiply by alpha (i.e., x) in the field */
        elem <<= 1;
        if (elem & (1 << ctx->field_order)) {
            /* Reduce modulo primitive polynomial */
            elem ^= 0;
            for (int j = 0; j <= ctx->field_order; j++) {
                if (prim_poly[j])
                    elem ^= (1 << j);
            }
        }
    }
    
    /* Handle wrap-around and zero */
    ctx->exp_tbl[ctx->code_len] = ctx->exp_tbl[0];
    ctx->log_tbl[0] = -1;
}

/* Compute generator polynomial from its roots */
static void build_generator(struct BCHCode *ctx)
{
    int roots[16];
    int num_roots = 0;
    int i, j, r;
    int seen[32] = {0};
    
    /* Find minimal polynomial roots: alpha^1 through alpha^(2t) and conjugates */
    for (r = 1; r <= 2 * ctx->correct_cap; r++) {
        int val = r % ctx->code_len;
        /* Add this root and all its conjugates (cyclotomic coset) */
        while (!seen[val]) {
            seen[val] = 1;
            roots[num_roots++] = val;
            val = (val * 2) % ctx->code_len;
        }
    }
    
    /* Build g(x) = product of (x - alpha^root) for all roots */
    ctx->gen_poly[0] = 1;
    for (i = 1; i <= ctx->parity_len; i++)
        ctx->gen_poly[i] = 0;
    
    for (i = 0; i < num_roots; i++) {
        /* Multiply current g(x) by (x + alpha^roots[i]) */
        for (j = num_roots; j > 0; j--) {
            int term = ctx->gen_poly[j - 1];
            if (term != 0) {
                /* GF multiply: term * root_val */
                int prod = ctx->exp_tbl[(ctx->log_tbl[term] + roots[i]) % ctx->code_len];
                ctx->gen_poly[j] ^= prod;
            }
        }
    }
}

struct BCHCode *BCHCode_New(int p[], int m, int n, int k, int t)
{
    struct BCHCode *ctx = malloc(sizeof(struct BCHCode));
    if (!ctx) return NULL;
    
    ctx->field_order = m;
    ctx->code_len = n;
    ctx->data_len = k;
    ctx->correct_cap = t;
    ctx->parity_len = n - k;
    
    ctx->exp_tbl = malloc((n + 1) * sizeof(int));
    ctx->log_tbl = malloc((n + 1) * sizeof(int));
    ctx->gen_poly = malloc((n - k + 1) * sizeof(int));
    ctx->bb = malloc((n - k) * sizeof(int));
    
    if (!ctx->exp_tbl || !ctx->log_tbl || !ctx->gen_poly || !ctx->bb) {
        BCHCode_Delete(ctx);
        return NULL;
    }
    
    build_field_tables(ctx, p);
    build_generator(ctx);
    
    /* Precompute syndrome table: syn_tbl[syndrome_idx][bit_pos] = exp[(syndrome * bit) % n] */
    for (int s = 0; s < 4; s++) {
        for (int bit = 0; bit < n; bit++) {
            ctx->syn_tbl[s][bit] = ctx->exp_tbl[((s + 1) * bit) % n];
        }
    }
    
    return ctx;
}

void BCHCode_Delete(struct BCHCode *ctx)
{
    if (!ctx) return;
    free(ctx->exp_tbl);
    free(ctx->log_tbl);
    free(ctx->gen_poly);
    free(ctx->bb);
    free(ctx);
}

void BCHCode_Encode(struct BCHCode *ctx, int bits[])
{
    int i, j, fb;
    
    if (!ctx) return;
    
    /* Initialize parity bits to zero */
    for (i = 0; i < ctx->parity_len; i++) {
        ctx->bb[i] = 0;
    }
    
    /* LFSR-based encoding using generator polynomial */
    for (i = ctx->data_len - 1; i >= 0; i--) {
        fb = bits[i] ^ ctx->bb[ctx->parity_len - 1];
        if (fb != 0) {
            for (j = ctx->parity_len - 1; j > 0; j--) {
                if (ctx->gen_poly[j] != 0)
                    ctx->bb[j] = ctx->bb[j - 1] ^ 1;
                else
                    ctx->bb[j] = ctx->bb[j - 1];
            }
            ctx->bb[0] = ctx->gen_poly[0] && fb;
        } else {
            for (j = ctx->parity_len - 1; j > 0; j--) {
                ctx->bb[j] = ctx->bb[j - 1];
            }
            ctx->bb[0] = 0;
        }
    }
}

/* Return pointer to parity bits array (valid after BCHCode_Encode call) */
int *BCHCode_GetParity(struct BCHCode *ctx)
{
    if (!ctx) return NULL;
    return ctx->bb;
}

/* Return length of parity array (n - k) */
int BCHCode_GetParityLen(struct BCHCode *ctx)
{
    if (!ctx) return 0;
    return ctx->parity_len;
}

/*
 * Decode received word, correcting up to t errors in-place.
 * Returns 0 if successful, 1 if uncorrectable errors detected.
 *
 * Optimized for BCH(31,21,2):
 * - Precomputed syndrome lookup table eliminates multiply/modulo in hot path
 * - Early exit on zero syndromes and after finding 2 roots
 * - Branchless syndrome accumulation where possible
 */
int BCHCode_Decode(struct BCHCode *ctx, int recv[])
{
    if (!ctx) return -1;

    const int n = 31;  /* Hardcoded for performance - compiler can optimize better */
    const int *exp = ctx->exp_tbl;
    const int *log = ctx->log_tbl;

    /* Syndrome computation using precomputed table */
    int S0 = 0, S1 = 0, S2 = 0, S3 = 0;
    for (int bit = 0; bit < n; bit++) {
        if (recv[bit]) {
            S0 ^= ctx->syn_tbl[0][bit];
            S1 ^= ctx->syn_tbl[1][bit];
            S2 ^= ctx->syn_tbl[2][bit];
            S3 ^= ctx->syn_tbl[3][bit];
        }
    }

    /* Early exit: no errors */
    if ((S0 | S1 | S2 | S3) == 0)
        return 0;

    /* Convert to index form (L0-L2 for S1-S3; S4 not needed for t=2) */
    int L0 = log[S0], L1 = log[S1], L2 = log[S2];
    (void)S3;  /* S4 computed but not used in this algorithm */

    /* S1=0 with errors means uncorrectable */
    if (L0 == -1)
        return (L1 != -1) ? 1 : 0;

    /* Single error: S3 == S1^3 (compare in index form) */
    int triple = L0 * 3;
    if (triple >= n) triple -= n;
    if (triple >= n) triple -= n;  /* Faster than modulo for small multiples */
    
    if (L2 == triple) {
        recv[L0] ^= 1;
        return 0;
    }

    /* Two errors: solve error locator polynomial */
    int denom = (L2 != -1) ? (exp[triple] ^ exp[L2]) : exp[triple];
    int denom_log = log[denom];

    /* Error locator coefficients */
    int c1 = L1 - denom_log;
    int c2 = L0 - denom_log;
    if (c1 < 0) c1 += n;
    if (c2 < 0) c2 += n;

    /* Chien search - find roots of 1 + c1*x + c2*x^2 */
    int pos0 = -1, pos1 = -1;
    int a1 = c1, a2 = c2;
    
    for (int i = 1; i <= n; i++) {
        a1++; if (a1 >= n) a1 -= n;
        a2 += 2; if (a2 >= n) a2 -= n; if (a2 >= n) a2 -= n;

        if ((1 ^ exp[a1] ^ exp[a2]) == 0) {
            if (pos0 < 0) {
                pos0 = (i < n) ? i : 0;
            } else {
                pos1 = (i < n) ? i : 0;
                break;  /* Found both roots, exit early */
            }
        }
    }

    if (pos0 >= 0 && pos1 >= 0) {
        recv[pos0] ^= 1;
        recv[pos1] ^= 1;
        return 0;
    }

    return 1;
}
