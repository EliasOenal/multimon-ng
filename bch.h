/*
 * bch.h - BCH(31,21,2) Error Correction for FLEX and POCSAG
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

#ifndef BCH_H
#define BCH_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BCH(31,21,2) Error Correction Library
 * 
 * Supports two protocols:
 * - FLEX: 31-bit codeword, data in bits 0-20, parity in bits 21-30
 * - POCSAG: 32-bit codeword, data in bits 31-11, parity in bits 10-1, overall parity in bit 0
 *
 * Both use BCH(31,21,2) but with different generator polynomials and bit layouts.
 */

/* Initialize lookup tables (called automatically on first use) */
void bch_init(void);

/* ========== FLEX Functions ========== */

/*
 * Encode 21-bit data into 31-bit FLEX codeword.
 * Input:  data in bits 0-20
 * Output: 31-bit codeword (data in bits 0-20, parity in bits 21-30)
 */
unsigned int bch_flex_encode(unsigned int data);

/*
 * Correct errors in a FLEX codeword.
 * Input/Output: pointer to 31-bit codeword
 * Returns: 0 = no errors, 1-2 = corrected bit count, -1 = uncorrectable
 */
int bch_flex_correct(unsigned int *codeword);

/* ========== POCSAG Functions ========== */

/*
 * Encode 21-bit data into 32-bit POCSAG codeword.
 * Input:  data in bits 0-20
 * Output: 32-bit codeword (data in bits 31-11, parity in bits 10-1, even parity in bit 0)
 */
unsigned int bch_pocsag_encode(unsigned int data);

/*
 * Correct errors in a POCSAG codeword.
 * Input/Output: pointer to 32-bit codeword
 * Returns: 0 = no errors, 1-2 = corrected bit count, -1 = uncorrectable
 */
int bch_pocsag_correct(unsigned int *codeword);

#ifdef __cplusplus
}
#endif

#endif /* BCH_H */
