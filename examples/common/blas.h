/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009, 2010-2011  Université de Bordeaux
 * Copyright (C) 2010  Centre National de la Recherche Scientifique
 *
 * StarPU is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * StarPU is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU Lesser General Public License in COPYING.LGPL for more details.
 */

#ifndef __BLAS_H__
#define __BLAS_H__

#include <starpu.h>

#ifdef STARPU_ATLAS
#include <cblas.h>
#endif

void STARPU_SGEMM(char *transa, char *transb, int M, int N, int K, float alpha, const float *A, int lda, 
		const float *B, int ldb, float beta, float *C, int ldc);
void STARPU_DGEMM(char *transa, char *transb, int M, int N, int K, double alpha, double *A, int lda, 
		double *B, int ldb, double beta, double *C, int ldc);
void STARPU_SGEMV(char *transa, int M, int N, float alpha, float *A, int lda,
		float *X, int incX, float beta, float *Y, int incY);
void STARPU_DGEMV(char *transa, int M, int N, double alpha, double *A, int lda,
		double *X, int incX, double beta, double *Y, int incY);
float STARPU_SASUM(int N, float *X, int incX);
double STARPU_DASUM(int N, double *X, int incX);
void STARPU_SSCAL(int N, float alpha, float *X, int incX);
void STARPU_DSCAL(int N, double alpha, double *X, int incX);
void STARPU_STRSM (const char *side, const char *uplo, const char *transa,
                   const char *diag, const int m, const int n,
                   const float alpha, const float *A, const int lda,
                   float *B, const int ldb);
void STARPU_DTRSM (const char *side, const char *uplo, const char *transa,
                   const char *diag, const int m, const int n,
                   const double alpha, const double *A, const int lda,
                   double *B, const int ldb);
void STARPU_DGEMM(char *transa, char *transb, int M, int N, int K, 
			double alpha, double *A, int lda, double *B, int ldb, 
			double beta, double *C, int ldc);
void STARPU_SSYR (const char *uplo, const int n, const float alpha,
                  const float *x, const int incx, float *A, const int lda);
void STARPU_SSYRK (const char *uplo, const char *trans, const int n,
                   const int k, const float alpha, const float *A,
                   const int lda, const float beta, float *C,
                   const int ldc);
void STARPU_SGER (const int m, const int n, const float alpha,
                  const float *x, const int incx, const float *y,
                  const int incy, float *A, const int lda);
void STARPU_DGER(const int m, const int n, const double alpha,
                  const double *x, const int incx, const double *y,
                  const int incy, double *A, const int lda);
void STARPU_STRSV (const char *uplo, const char *trans, const char *diag, 
                   const int n, const float *A, const int lda, float *x, 
                   const int incx);
void STARPU_STRMM(const char *side, const char *uplo, const char *transA,
                 const char *diag, const int m, const int n,
                 const float alpha, const float *A, const int lda,
                 float *B, const int ldb);
void STARPU_DTRMM(const char *side, const char *uplo, const char *transA,
                 const char *diag, const int m, const int n,
                 const double alpha, const double *A, const int lda,
                 double *B, const int ldb);
void STARPU_STRMV(const char *uplo, const char *transA, const char *diag,
                 const int n, const float *A, const int lda, float *X,
                 const int incX);
void STARPU_SAXPY(const int n, const float alpha, float *X, const int incX, float *Y, const int incy);
void STARPU_DAXPY(const int n, const double alpha, double *X, const int incX, double *Y, const int incY);
int STARPU_ISAMAX (const int n, float *X, const int incX);
int STARPU_IDAMAX (const int n, double *X, const int incX);
float STARPU_SDOT(const int n, const float *x, const int incx, const float *y, const int incy);
double STARPU_DDOT(const int n, const double *x, const int incx, const double *y, const int incy);
void STARPU_SSWAP(const int n, float *x, const int incx, float *y, const int incy);
void STARPU_DSWAP(const int n, double *x, const int incx, double *y, const int incy);

#ifdef STARPU_MKL
void STARPU_SPOTRF(const char*uplo, const int n, float *a, const int lda);
void STARPU_DPOTRF(const char*uplo, const int n, double *a, const int lda);
#endif

#if defined(STARPU_GOTO) || defined(STARPU_SYSTEM_BLAS) || defined(STARPU_MKL)

extern void sgemm_ (const char *transa, const char *transb, const int *m,
                   const int *n, const int *k, const float *alpha, 
                   const float *A, const int *lda, const float *B, 
                   const int *ldb, const float *beta, float *C, 
                   const int *ldc);
extern void dgemm_ (const char *transa, const char *transb, const int *m,
                   const int *n, const int *k, const double *alpha, 
                   const double *A, const int *lda, const double *B, 
                   const int *ldb, const double *beta, double *C, 
                   const int *ldc);
extern void sgemv_(const char *trans, const int *m, const int *n, const float *alpha,
                   const float *a, const int *lda, const float *x, const int *incx, 
                   const float *beta, float *y, const int *incy);
extern void dgemv_(const char *trans, const int *m, const int *n, const double *alpha,
                   const double *a, const int *lda, const double *x, const int *incx,
                   const double *beta, double *y, const int *incy);
extern void ssyr_ (const char *uplo, const int *n, const float *alpha,
                  const float *x, const int *incx, float *A, const int *lda);
extern void ssyrk_ (const char *uplo, const char *trans, const int *n,
                   const int *k, const float *alpha, const float *A,
                   const int *lda, const float *beta, float *C,
                   const int *ldc);
extern void strsm_ (const char *side, const char *uplo, const char *transa, 
                   const char *diag, const int *m, const int *n,
                   const float *alpha, const float *A, const int *lda,
                   float *B, const int *ldb);
extern void dtrsm_ (const char *side, const char *uplo, const char *transa, 
                   const char *diag, const int *m, const int *n,
                   const double *alpha, const double *A, const int *lda,
                   double *B, const int *ldb);
extern double sasum_ (const int *n, const float *x, const int *incx);
extern double dasum_ (const int *n, const double *x, const int *incx);
extern void sscal_ (const int *n, const float *alpha, float *x,
                   const int *incx);
extern void dscal_ (const int *n, const double *alpha, double *x,
                   const int *incx);
extern void sger_(const int *m, const int *n, const float *alpha,
                  const float *x, const int *incx, const float *y,
                  const int *incy, float *A, const int *lda);
extern void dger_(const int *m, const int *n, const double *alpha,
                  const double *x, const int *incx, const double *y,
                  const int *incy, double *A, const int *lda);
extern void strsv_ (const char *uplo, const char *trans, const char *diag, 
                   const int *n, const float *A, const int *lda, float *x, 
                   const int *incx);
extern void strmm_(const char *side, const char *uplo, const char *transA,
                 const char *diag, const int *m, const int *n,
                 const float *alpha, const float *A, const int *lda,
                 float *B, const int *ldb);
extern void dtrmm_(const char *side, const char *uplo, const char *transA,
                 const char *diag, const int *m, const int *n,
                 const double *alpha, const double *A, const int *lda,
                 double *B, const int *ldb);
extern void strmv_(const char *uplo, const char *transA, const char *diag,
                 const int *n, const float *A, const int *lda, float *X,
                 const int *incX);
extern void saxpy_(const int *n, const float *alpha, const float *X, const int *incX,
		float *Y, const int *incy);
extern void daxpy_(const int *n, const double *alpha, const double *X, const int *incX,
		double *Y, const int *incy);
extern int isamax_(const int *n, const float *X, const int *incX);
extern int idamax_(const int *n, const double *X, const int *incX);
/* for some reason, FLOATRET is not a float but a double in GOTOBLAS */
extern double sdot_(const int *n, const float *x, const int *incx, const float *y, const int *incy);
extern double ddot_(const int *n, const double *x, const int *incx, const double *y, const int *incy);
extern void sswap_(const int *n, float *x, const int *incx, float *y, const int *incy);
extern void dswap_(const int *n, double *x, const int *incx, double *y, const int *incy);

#ifdef STARPU_MKL
extern void spotrf_(const char*uplo, const int *n, float *a, const int *lda, int *info);
extern void dpotrf_(const char*uplo, const int *n, double *a, const int *lda, int *info);
#endif

#endif

#endif /* __BLAS_H__ */
