/*
 *   R package for Singular Spectrum Analysis
 *   Copyright (c) 2009 Anton Korobeynikov <asl@math.spbu.ru>
 *
 *   This program is free software; you can redistribute it
 *   and/or modify it under the terms of the GNU General Public
 *   License as published by the Free Software Foundation;
 *   either version 2 of the License, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied
 *   warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *   PURPOSE.  See the GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public
 *   License along with this program; if not, write to the
 *   Free Software Foundation, Inc., 675 Mass Ave, Cambridge,
 *   MA 02139, USA.
 */

#include <R.h>
#include <Rinternals.h>

#include <complex.h>
#include <fftw3.h>

typedef struct {
  fftw_complex * circ_freq;
  fftw_plan r2c_plan;
  fftw_plan c2r_plan;
  R_len_t window;
  R_len_t length;
} toeplitz_circulant;

typedef struct {
  toeplitz_circulant normal;
  toeplitz_circulant transposed;
} hankel_matrix;

void _free_circulant(toeplitz_circulant *C) {
  fftw_free(C->circ_freq);
  fftw_destroy_plan(C->r2c_plan);
  fftw_destroy_plan(C->c2r_plan);
}

void _initialize_circulant(toeplitz_circulant *C,
                           const double *F, R_len_t N, R_len_t L) {
  R_len_t K = N - L + 1, i;
  fftw_complex *ocirc;
  fftw_plan p1, p2;
  double *circ;

  /* Allocate needed memory */
  circ = (double*) fftw_malloc(N * sizeof(double));
  ocirc = (fftw_complex*) fftw_malloc((N/2 + 1) * sizeof(fftw_complex));

  /* Estimate the best plans for given input length */
  p1 = fftw_plan_dft_r2c_1d(N, circ, ocirc, FFTW_ESTIMATE);
  p2 = fftw_plan_dft_c2r_1d(N, ocirc, circ, FFTW_ESTIMATE);

  /* Fill input buffer */
  for (i = K-1; i < N; ++i)
    circ[i - K + 1] = F[i];

  for (i = 0; i < K-1; ++i) {
    circ[L + i] = F[i];
  }

  /* Run the plan on input data */
  fftw_execute(p1);

  /* Cleanup and return */
  fftw_free(circ);

  C->circ_freq = ocirc;
  C->r2c_plan = p1;
  C->c2r_plan = p2;
  C->window = L;
  C->length = N;
}

static void hmatFinalizer(SEXP ptr) {
  hankel_matrix *h;

  if (TYPEOF(ptr) != EXTPTRSXP)
    return;

  h = R_ExternalPtrAddr(ptr);
  if (!h)
    return;

  _free_circulant(&h->normal);
  _free_circulant(&h->transposed);
  Free(h);

  R_ClearExternalPtr(ptr);
}

SEXP initialize_hmat(SEXP F, SEXP window) {
  R_len_t N, L;
  hankel_matrix *h;
  SEXP hmat;

  N = length(F);
  L = INTEGER(window)[0];

  /* Build toeplitz circulants for normal and transposed matrices */
  h = Calloc(1, hankel_matrix);
  _initialize_circulant(&h->normal, REAL(F), N, L);
  _initialize_circulant(&h->transposed, REAL(F), N, N - L + 1);

  /* Make an external pointer envolope */
  hmat = R_MakeExternalPtr(h, install("hankel_matrix"), R_NilValue);
  R_RegisterCFinalizer(hmat, hmatFinalizer);

  return hmat;
}

void _hmatmul(double* out,
              const double* v, R_len_t K,
              const toeplitz_circulant *C) {
  R_len_t L = C->window;
  R_len_t N = K + L - 1, i;
  double *circ;
  fftw_complex *ocirc;

  /* Allocate needed memory */
  circ = (double*) fftw_malloc(N * sizeof(double));
  ocirc = (fftw_complex*) fftw_malloc((N/2 + 1) * sizeof(fftw_complex));

  /* Fill the arrays */
  for (i = 0; i < K; ++i)
    circ[i] = v[K - i - 1];
  memset(circ + K, 0, (L - 1)*sizeof(double));

  /* Compute the FFT of the reversed vector v */
  fftw_execute_dft_r2c(C->r2c_plan, circ, ocirc);

  /* Dot-multiply with pre-computed FFT of toeplitz circulant */
  for (i = 0; i < (N/2 + 1); ++i)
    ocirc[i] = ocirc[i] * C->circ_freq[i];

  /* Compute the reverse transform to obtain result */
  fftw_execute_dft_c2r(C->c2r_plan, ocirc, circ);

  /* Cleanup and return */
  for (i = 0; i < L; ++i)
    out[i] = circ[i] / N;

  fftw_free(circ);
  fftw_free(ocirc);
}


SEXP hmatmul(SEXP hmat, SEXP v, SEXP transposed) {
  R_len_t K = length(v);
  double *rV;
  hankel_matrix *h;
  toeplitz_circulant *C;
  SEXP Y;

  /* Grab needed data */
  h = R_ExternalPtrAddr(hmat);
  C = (LOGICAL(transposed)[0] ? &h->transposed : &h->normal);

  /* Check agains absurd values of inputs */
  K = length(v);
  if (K + C->window - 1 != C->length)
    error("invalid length of input vector 'v'");

  /* Allocate output buffer */
  PROTECT(Y = allocVector(REALSXP, C->window));

  /* Calculate the product */
  _hmatmul(REAL(Y), rV, K, C);

  UNPROTECT(1);

  return Y;
}
