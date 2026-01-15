/*
 *      BCHCode_stub.c
 *
 *      Copyright (C) 2018 Göran Weinholt <weinholt@debian.org>
 *
 *      Stub replacement for BCHCode.c, whose license is GPL incompatible
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License along
 *      with this program; if not, write to the Free Software Foundation, Inc.,
 *      51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdlib.h>
#include "BCHCode.h"

struct BCHCode {
    int parity_len;
    int *bb;
};

struct BCHCode *BCHCode_New(int p[], int m, int n, int k, int t)
{
    /* Ignore unused variables */
    (void) p;
    (void) m;
    (void) t;
    
    struct BCHCode *ctx = malloc(sizeof(struct BCHCode));
    if (!ctx) return NULL;
    
    ctx->parity_len = n - k;
    ctx->bb = calloc(ctx->parity_len, sizeof(int));
    if (!ctx->bb) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

void BCHCode_Delete(struct BCHCode *BCHCode_data)
{
    if (!BCHCode_data) return;
    free(BCHCode_data->bb);
    free(BCHCode_data);
}

void BCHCode_Encode(struct BCHCode *BCHCode_data, int data[])
{
    /* Stub: does nothing - no actual encoding */
    (void) BCHCode_data;
    (void) data;
}

int BCHCode_Decode(struct BCHCode *BCHCode_data, int recd[])
{
    (void) BCHCode_data;
    (void) recd;
    return 0;
}

int *BCHCode_GetParity(struct BCHCode *BCHCode_data)
{
    if (!BCHCode_data) return NULL;
    return BCHCode_data->bb;
}

int BCHCode_GetParityLen(struct BCHCode *BCHCode_data)
{
    if (!BCHCode_data) return 0;
    return BCHCode_data->parity_len;
}
