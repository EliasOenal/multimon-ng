/*
 *      BCHCode.c
 *
 *      Copyright (C) 2015 Craig Shelley (craig@microtron.org.uk)
 *
 *      BCH Encoder/Decoder - Adapted from GNURadio for use with Multimon
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <math.h>
#include <stdlib.h>
#include "BCHCode.h"

struct BCHCode {
	int * p;         // coefficients of primitive polynomial used to generate GF(2**5)
	int m;           // order of the field GF(2**5) = 5
	int n;           // 2**5 - 1 = 31
	int k;           // n - deg(g(x)) = 21 = dimension
	int t;           // 2 = error correcting capability
	int * alpha_to;  // log table of GF(2**5)
	int * index_of;  // antilog table of GF(2**5)
	int * g;         // coefficients of generator polynomial, g(x) [n - k + 1]=[11]
	int * bb;        // coefficients of redundancy polynomial ( x**(10) i(x) ) modulo g(x)
};






static void generate_gf(struct BCHCode * BCHCode_data) {
	if (BCHCode_data==NULL)  return;
	/*
	 * generate GF(2**m) from the irreducible polynomial p(X) in p[0]..p[m]
	 * lookup tables:  index->polynomial form   alpha_to[] contains j=alpha**i;
	 * polynomial form -> index form  index_of[j=alpha**i] = i alpha=2 is the
	 * primitive element of GF(2**m)
	 */

	register int    i, mask;
	mask = 1;
	BCHCode_data->alpha_to[BCHCode_data->m] = 0;
	for (i = 0; i < BCHCode_data->m; i++) {
		BCHCode_data->alpha_to[i] = mask;
		BCHCode_data->index_of[BCHCode_data->alpha_to[i]] = i;
		if (BCHCode_data->p[i] != 0)
			BCHCode_data->alpha_to[BCHCode_data->m] ^= mask;
		mask <<= 1;
	}
	BCHCode_data->index_of[BCHCode_data->alpha_to[BCHCode_data->m]] = BCHCode_data->m;
	mask >>= 1;
	for (i = BCHCode_data->m + 1; i < BCHCode_data->n; i++) {
		if (BCHCode_data->alpha_to[i - 1] >= mask)
			BCHCode_data->alpha_to[i] = BCHCode_data->alpha_to[BCHCode_data->m] ^ ((BCHCode_data->alpha_to[i - 1] ^ mask) << 1);
		else
			BCHCode_data->alpha_to[i] = BCHCode_data->alpha_to[i - 1] << 1;
		BCHCode_data->index_of[BCHCode_data->alpha_to[i]] = i;
	}
	BCHCode_data->index_of[0] = -1;
}


static void gen_poly(struct BCHCode * BCHCode_data) {
	if (BCHCode_data==NULL)  return;
	/*
	 * Compute generator polynomial of BCH code of length = 31, redundancy = 10
	 * (OK, this is not very efficient, but we only do it once, right? :)
	 */

	register int    ii, jj, ll, kaux;
	int             test, aux, nocycles, root, noterms, rdncy;
	int             cycle[15][6], size[15], min[11], zeros[11];
	/* Generate cycle sets modulo 31 */
	cycle[0][0] = 0; size[0] = 1;
	cycle[1][0] = 1; size[1] = 1;
	jj = 1;			/* cycle set index */
	do {
		/* Generate the jj-th cycle set */
		ii = 0;
		do {
			ii++;
			cycle[jj][ii] = (cycle[jj][ii - 1] * 2) % BCHCode_data->n;
			size[jj]++;
			aux = (cycle[jj][ii] * 2) % BCHCode_data->n;
		} while (aux != cycle[jj][0]);
		/* Next cycle set representative */
		ll = 0;
		do {
			ll++;
			test = 0;
			for (ii = 1; ((ii <= jj) && (!test)); ii++) {
				/* Examine previous cycle sets */
				for (kaux = 0; ((kaux < size[ii]) && (!test)); kaux++) {
					if (ll == cycle[ii][kaux]) {
						test = 1;
					}
				}
			}
		} while ((test) && (ll < (BCHCode_data->n - 1)));
		if (!(test)) {
			jj++;	/* next cycle set index */
			cycle[jj][0] = ll;
			size[jj] = 1;
		}
	} while (ll < (BCHCode_data->n - 1));
	nocycles = jj;		/* number of cycle sets modulo BCHCode_data->n */
	/* Search for roots 1, 2, ..., BCHCode_data->d-1 in cycle sets */
	kaux = 0;
	rdncy = 0;
	for (ii = 1; ii <= nocycles; ii++) {
		min[kaux] = 0;
		for (jj = 0; jj < size[ii]; jj++) {
			for (root = 1; root < (2*BCHCode_data->t + 1); root++) {
				if (root == cycle[ii][jj]) {
					min[kaux] = ii;
				}
			}
		}
		if (min[kaux]) {
			rdncy += size[min[kaux]];
			kaux++;
		}
	}
	noterms = kaux;
	kaux = 1;
	for (ii = 0; ii < noterms; ii++) {
		for (jj = 0; jj < size[min[ii]]; jj++) {
			zeros[kaux] = cycle[min[ii]][jj];
			kaux++;
		}
	}
	//printf("This is a (%d, %d, %d) binary BCH code\n", BCHCode_data->n, BCHCode_data->k, BCHCode_data->d);
	/* Compute generator polynomial */
	BCHCode_data->g[0] = BCHCode_data->alpha_to[zeros[1]];
	BCHCode_data->g[1] = 1;		/* g(x) = (X + zeros[1]) initially */
	for (ii = 2; ii <= rdncy; ii++) {
		BCHCode_data->g[ii] = 1;
		for (jj = ii - 1; jj > 0; jj--) {
			if (BCHCode_data->g[jj] != 0)
				BCHCode_data->g[jj] = BCHCode_data->g[jj - 1] ^ BCHCode_data->alpha_to[(BCHCode_data->index_of[BCHCode_data->g[jj]] + zeros[ii]) % BCHCode_data->n];
			else
				BCHCode_data->g[jj] = BCHCode_data->g[jj - 1];
		}
		BCHCode_data->g[0] = BCHCode_data->alpha_to[(BCHCode_data->index_of[BCHCode_data->g[0]] + zeros[ii]) % BCHCode_data->n];
	}
	//printf("g(x) = ");
	//for (ii = 0; ii <= rdncy; ii++) {
	//	printf("%d", BCHCode_data->g[ii]);
	//	if (ii && ((ii % 70) == 0)) {
	//		printf("\n");
	//	}
	//}
	//printf("\n");
}


void BCHCode_Encode(struct BCHCode * BCHCode_data, int data[]) {
	if (BCHCode_data==NULL)  return;
	/*
	 * Calculate redundant bits bb[], codeword is c(X) = data(X)*X**(n-k)+ bb(X)
	 */

	register int    i, j;
	register int    feedback;
	for (i = 0; i < BCHCode_data->n - BCHCode_data->k; i++) {
		BCHCode_data->bb[i] = 0;
	}
	for (i = BCHCode_data->k - 1; i >= 0; i--) {
		feedback = data[i] ^ BCHCode_data->bb[BCHCode_data->n - BCHCode_data->k - 1];
		if (feedback != 0) {
			for (j = BCHCode_data->n - BCHCode_data->k - 1; j > 0; j--) {
				if (BCHCode_data->g[j] != 0) {
					BCHCode_data->bb[j] = BCHCode_data->bb[j - 1] ^ feedback;
				} else {
					BCHCode_data->bb[j] = BCHCode_data->bb[j - 1];
				}
			}
			BCHCode_data->bb[0] = BCHCode_data->g[0] && feedback;
		} else {
			for (j = BCHCode_data->n - BCHCode_data->k - 1; j > 0; j--) {
				BCHCode_data->bb[j] = BCHCode_data->bb[j - 1];
			}
			BCHCode_data->bb[0] = 0;
		};
	};
};


int BCHCode_Decode(struct BCHCode * BCHCode_data, int recd[]) {
	if (BCHCode_data==NULL)  return -1;
	/*
	 * We do not need the Berlekamp algorithm to decode.
	 * We solve before hand two equations in two variables.
	 */

	register int    i, j, q;
	int             elp[3], s[5], s3;
	int             count = 0, syn_error = 0;
	int             loc[3], reg[3];
	int				aux;
	int retval=0;
	/* first form the syndromes */
	//	printf("s[] = (");
	for (i = 1; i <= 4; i++) {
		s[i] = 0;
		for (j = 0; j < BCHCode_data->n; j++) {
			if (recd[j] != 0) {
				s[i] ^= BCHCode_data->alpha_to[(i * j) % BCHCode_data->n];
			}
		}
		if (s[i] != 0) {
			syn_error = 1;	/* set flag if non-zero syndrome */
		}
		/* NOTE: If only error detection is needed,
		 * then exit the program here...
		 */
		/* convert syndrome from polynomial form to index form  */
		s[i] = BCHCode_data->index_of[s[i]];
		//printf("%3d ", s[i]);
	};
	//printf(")\n");
	if (syn_error) {	/* If there are errors, try to correct them */
		if (s[1] != -1) {
			s3 = (s[1] * 3) % BCHCode_data->n;
			if ( s[3] == s3 ) { /* Was it a single error ? */
				//printf("One error at %d\n", s[1]);
				recd[s[1]] ^= 1; /* Yes: Correct it */
			} else {
				/* Assume two errors occurred and solve
				 * for the coefficients of sigma(x), the
				 * error locator polynomail
				 */
				if (s[3] != -1) {
					aux = BCHCode_data->alpha_to[s3] ^ BCHCode_data->alpha_to[s[3]];
				} else {
					aux = BCHCode_data->alpha_to[s3];
				}
				elp[0] = 0;
				elp[1] = (s[2] - BCHCode_data->index_of[aux] + BCHCode_data->n) % BCHCode_data->n;
				elp[2] = (s[1] - BCHCode_data->index_of[aux] + BCHCode_data->n) % BCHCode_data->n;
				//printf("sigma(x) = ");
				//for (i = 0; i <= 2; i++) {
				//	printf("%3d ", elp[i]);
				//}
				//printf("\n");
				//printf("Roots: ");
				/* find roots of the error location polynomial */
				for (i = 1; i <= 2; i++) {
					reg[i] = elp[i];
				}
				count = 0;
				for (i = 1; i <= BCHCode_data->n; i++) { /* Chien search */
					q = 1;
					for (j = 1; j <= 2; j++) {
						if (reg[j] != -1) {
							reg[j] = (reg[j] + j) % BCHCode_data->n;
							q ^= BCHCode_data->alpha_to[reg[j]];
						}
					}
					if (!q) {	/* store error location number indices */
						loc[count] = i % BCHCode_data->n;
						count++;
						//printf("%3d ", (i%n));
					}
				}
				//printf("\n");
				if (count == 2)	{
					/* no. roots = degree of elp hence 2 errors */
					for (i = 0; i < 2; i++)
						recd[loc[i]] ^= 1;
				} else {	/* Cannot solve: Error detection */
					retval=1;
					//for (i = 0; i < 31; i++) {
					//	recd[i] = 0;
					//}
					//printf("incomplete decoding\n");
				}
			}
		} else if (s[2] != -1) {/* Error detection */
			retval=1;
			//for (i = 0; i < 31; i++) recd[i] = 0;
			//printf("incomplete decoding\n");
		}
	}

	return retval;
}

/*
 * Example usage BCH(31,21,5)
 *
 * p[] = coefficients of primitive polynomial used to generate GF(2**5)
 * m = order of the field GF(2**5) = 5
 * n = 2**5 - 1 = 31
 * t = 2 = error correcting capability
 * d = 2*BCHCode_data->t + 1 = 5 = designed minimum distance
 * k = n - deg(g(x)) = 21 = dimension
 * g[] = coefficients of generator polynomial, g(x) [n - k + 1]=[11]
 * alpha_to [] = log table of GF(2**5)
 * index_of[] = antilog table of GF(2**5)
 * data[] = coefficients of data polynomial, i(x)
 * bb[] = coefficients of redundancy polynomial ( x**(10) i(x) ) modulo g(x)
 */
struct BCHCode * BCHCode_New(int p[], int m, int n, int k, int t) {
	struct BCHCode * BCHCode_data=NULL;

	BCHCode_data=(struct BCHCode *) malloc(sizeof (struct BCHCode));

	if (BCHCode_data!=NULL) {
		BCHCode_data->alpha_to=(int *) malloc(sizeof(int) * (n+1));
		BCHCode_data->index_of=(int *) malloc(sizeof(int) * (n+1));
		BCHCode_data->p=(int *) malloc(sizeof(int) * (m+1));
		BCHCode_data->g=(int *) malloc(sizeof(int) * (n-k+1));
		BCHCode_data->bb=(int *) malloc(sizeof(int) * (n-k+1));

		if (
				BCHCode_data->alpha_to == NULL ||
				BCHCode_data->index_of == NULL ||
				BCHCode_data->p        == NULL ||
				BCHCode_data->g        == NULL ||
				BCHCode_data->bb       == NULL
				) {
			BCHCode_Delete(BCHCode_data);
			BCHCode_data=NULL;
		}
	}

	if (BCHCode_data!=NULL) {
		int i;
		for (i=0; i<(m+1); i++) {
			BCHCode_data->p[i]=p[i];
		}
		BCHCode_data->m=m;
		BCHCode_data->n=n;
		BCHCode_data->k=k;
		BCHCode_data->t=t;

		generate_gf(BCHCode_data);			/* generate the Galois Field GF(2**m) */
		gen_poly(BCHCode_data);				/* Compute the generator polynomial of BCH code */
	}

	return BCHCode_data;
}

void BCHCode_Delete(struct BCHCode * BCHCode_data) {
	if (BCHCode_data==NULL)  return;

	if (BCHCode_data->alpha_to != NULL) free(BCHCode_data->alpha_to);
	if (BCHCode_data->index_of != NULL) free(BCHCode_data->index_of);
	if (BCHCode_data->p        != NULL) free(BCHCode_data->p);
	if (BCHCode_data->g        != NULL) free(BCHCode_data->g);
	if (BCHCode_data->bb       != NULL) free(BCHCode_data->bb);

	free(BCHCode_data);
}
