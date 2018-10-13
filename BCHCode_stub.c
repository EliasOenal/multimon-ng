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
};

struct BCHCode *BCHCode_New(int p[], int m, int n, int k, int t)
{
    /* Ignore unused variables */
    (void) p;
    (void) m;
    (void) n;
    (void) k;
    (void) t;
    return malloc(sizeof(struct BCHCode));
}

void BCHCode_Delete(struct BCHCode *BCHCode_data)
{
    free(BCHCode_data);
}

int BCHCode_Decode(struct BCHCode *BCHCode_data, int recd[])
{
    (void) BCHCode_data;
    (void) recd;
    return 0;
}
