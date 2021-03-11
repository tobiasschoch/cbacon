/* Implementation of a weighted variant of the BACON algorithm for robust
   linear regression of Billor et al. (2000)

   Copyright (C) 2020 Tobias Schoch (e-mail: tobias.schoch@gmail.com)

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not, a copy is available at
   https://www.gnu.org/licenses/

   Billor N, Hadi AS, Vellemann PF (2000). BACON: Blocked Adaptative
      Computationally efficient Outlier Nominators. Computational Statistics
      and Data Analysis 34, pp. 279-298.
*/

#include "wbacon_reg.h"

#define _POWER2(_x) ((_x) * (_x))
#define _debug_mode 1               // 0: default; 1: debug mode

#if _debug_mode
#include "utils.h"
#endif

// structure of working arrays
typedef struct workarray_struct {
    int lwork;
    int *iarray;
    double *work_p;
    double *work_n;
    double *work_np;
    double *work_pp;
    double *dgels_work;
} workarray;

// declarations of local function
static wbacon_error_type initial_reg(regdata*, workarray*, estimate*,
    int* restrict, int*, int*);
static wbacon_error_type algorithm_4(regdata*, workarray*, estimate*,
    int* restrict, int* restrict, int*, int*, int*);
static wbacon_error_type algorithm_5(regdata*, workarray*, estimate*,
    int* restrict, int* restrict, double*, int*, int*, int*);
static wbacon_error_type compute_ti(regdata*, workarray*, estimate*,
    int* restrict, int*, double* restrict);
static wbacon_error_type update_chol_xty(regdata*, workarray*, estimate*,
    int* restrict, int* restrict, int*);
static inline wbacon_error_type chol_downdate(double* restrict,
    double* restrict, int);
static inline wbacon_error_type hat_matrix(regdata*, workarray*,
    double* restrict, double* restrict);
static void select_subset(double* restrict, int* restrict, int*, int*);
static inline void cholesky_reg(double*, double*, double*, double*, int*, int*);
static inline void chol_update(double* restrict, double* restrict, int);

/******************************************************************************\
|* BACON regression estimator                                                 *|
|*  x        design matrix, array[n, p]                                       *|
|*  y        response variable, array[n]                                      *|
|*  w        weights, array[n]                                                *|
|*  resid    on return: residuals, array[n]                                   *|
|*  beta     on return: estimated coefficients, array[p]                      *|
|*  subset0  on entry: subset generated by Algorithm 3; on return, subset of  *|
|*           outlier-free observations                                        *|
|*  dist     on entry: Mahalanobis distances generated by Algorithm 3; on     *|
|*           return: discrepancies/ distances t[i]                            *|
|*  n        dimension                                                        *|
|*  p        dimension                                                        *|
|*  m        on entry: size of subset; on return: size of final subset        *|
|*  verbose  toggle: 1: additional information is printed to the console;     *|
|*           0: quiet                                                         *|
|*  success  on return, 1 if sucessfull; 0 if failed to converge              *|
|*  collect  on entry: parameter to specify the size of the intial subset     *|
|*  alpha    level of significance that determines the (1-alpha) quantile of  *|
|*           the Student t-distr. used as a cutoff value for the t[i]'s       *|
|*  maxiter  maximum number of iterations                                     *|
\******************************************************************************/
void wbacon_reg(double *x, double *y, double *w, double *resid, double *beta,
    int *subset0, double *dist, int *n, int *p, int *m, int *verbose,
    int *success, int *collect, double *alpha, int *maxiter)
{
wbacon_error_type err;
*success = 1;

int *subset1 = (int*) Calloc(*n, int);

    // initialize and populate 'data' which is a regdata struct
    regdata data;
    regdata *dat = &data;
    dat->n = *n; dat->p = *p;
    dat->x = x;
    dat->y = y;
    dat->w = w;
    double *wy = (double*) Calloc(*n, double);
    dat->wy = wy;
    double *wx = (double*) Calloc(*n * *p, double);
    dat->wx = wx;
    // sqrt(w) is computed once and then shared
    double *w_sqrt = (double*) Calloc(*n, double);

    for (int i = 0; i < *n; i++)
        w_sqrt[i] = sqrt(w[i]);

    dat->w_sqrt = w_sqrt;

    // initialize and populate 'est' which is a estimate struct
    estimate the_estimate;
    estimate *est = &the_estimate;
    est->resid = resid;
    est->beta = beta;
    est->dist = dist;
    double *L = (double*) Calloc(*p * *p, double);
    est->L = L;
    double *xty = (double*) Calloc(*p, double);
    est->xty = xty;

    // initialize and populate 'data' which is a workarray struct
    workarray warray;
    workarray *work = &warray;
    double *work_p = (double*) Calloc(*p, double);
    work->work_p = work_p;
    double *work_pp = (double*) Calloc(*p * *p, double);
    work->work_pp = work_pp;
    double *work_np = (double*) Calloc(*n * *p, double);
    work->work_np = work_np;
    double *work_n = (double*) Calloc(*n, double);
    work->work_n = work_n;
    int *iarray = (int*) Calloc(*n, int);
    work->iarray = iarray;
    // determine size of work array for LAPACK:degels
    work->lwork = fitwls(dat, est, subset0, work_np, -1);
    double *dgels_work = (double*) Calloc(work->lwork, double);
    work->dgels_work = dgels_work;

    // STEP 0 (initialization)
    err = initial_reg(dat, work, est, subset0, m, verbose);
    if (err != WBACON_ERROR_OK) {
        *success = 0;
        PRINT_OUT("Error: design %s (step 0)\n", wbacon_error(err));
        goto clean_up;
    }

#if _debug_mode
PRINT_OUT("initialization\n");
print_magic_number(subset0, *m);
PRINT_OUT("\n");
#endif

    // select the p+1 obs. with the smallest ti's (initial basic subset)
    *m = *p + 1;
    select_subset(est->dist, subset1, m, n);

    // STEP 1 (Algorithm 4)
    err = algorithm_4(dat, work, est, subset0, subset1, m, verbose, collect);
    if (err != WBACON_ERROR_OK) {
        PRINT_OUT("Error: %s (Cholesky update, step 1)\n", wbacon_error(err));
        *success = 0;
        goto clean_up;
    }

#if _debug_mode
PRINT_OUT("Alg. 4 (end of)\n");
print_magic_number(subset1, *m);
PRINT_OUT("\n");
#endif

    // STEP 2 (Algorithm 5)
    err = algorithm_5(dat, work, est, subset1, subset0, alpha, m, maxiter,
        verbose);
    if (err != WBACON_ERROR_OK) {
        PRINT_OUT("Error: %s (step 2)\n", wbacon_error(err));
        *success = 0;
    }

    // copy the QR factorization to x (as returned by fitwls -> dgels -> dgeqrf)
    Memcpy(x, wx, *n * *p);

clean_up:
    Free(work_pp); Free(work_p); Free(work_np); Free(work_n); Free(dgels_work);
    Free(iarray); Free(subset1);
    Free(wx); Free(wy); Free(w_sqrt); Free(L);  Free(xty);
}

/******************************************************************************\
|* Initial basic subset, adapted for weighting                                *|
|*  dat      typedef struct regdata                                           *|
|*  work     typedef struct workarray                                         *|
|*  est      typedef struct estimate                                          *|
|*  subset   subset, 1: obs. is in the subset, 0 otherwise, array[n]          *|
|*  m        number of obs. in subset                                         *|
|*  verbose  toggle: 1: additional information is printed to the console;     *|
|*           0: quiet                                                         *|
|* On return, dat->wx is overwritten with the R matrix of the QR factorization*|
\******************************************************************************/
static wbacon_error_type initial_reg(regdata *dat, workarray *work,
    estimate *est, int* restrict subset, int *m, int *verbose)
{
    int info, n = dat->n, p = dat->p;
    int* restrict iarray = work->iarray;
    double* restrict w = dat->w;
    double* restrict x = dat->x;
    double* restrict y = dat->y;
    double* restrict wx = dat->wx;
    double* restrict L = est->L;
    double* restrict xty = est->xty;
    wbacon_error_type status = WBACON_ERROR_OK;

    // 4) compute regression estimate (on return, dat->wx is overwritten by the
    // R matrix of the QR factorization (R will be used by the caller of
    // initial_reg)
    info = fitwls(dat, est, subset, work->dgels_work, work->lwork);

    // if the design matrix is rank deficient, we enlarge the subset
    if (info) {
        status = WBACON_ERROR_RANK_DEFICIENT;
        // sort the dist[i]'s in ascending order
        psort_array(est->dist, iarray, n, n);

        // add obs. to the subset until x has full rank
        while (*m < n) {
            (*m)++;
            // select obs. with smallest dist (among those obs. not in the
            // subset)
            subset[iarray[*m - 1]] = 1;

            // re-do regression and check rank
            info = fitwls(dat, est, subset, work->dgels_work, work->lwork);
            if (info == 0) {
                status = WBACON_ERROR_OK;
                break;
            }
        }
    }
    if (*verbose)
    PRINT_OUT("Step 0: initial subset, m = %d\n", *m);

    // extract R matrix (as a lower triangular matrix: L) from dat->wx
    for (int i = 0; i < p; i++)
        for (int j = i; j < p; j++)
            L[j + i * p] = wx[i + j * n];

    // compute xty (weighted)
    #pragma omp parallel for if(p * n > REG_OMP_MIN_SIZE)
    for (int i = 0; i < p; i++)
        for (int j = 0; j < n; j++)
            if (subset[j])
                xty[i] += w[j] * x[j + i * n] * y[j];

    // compute t[i]'s
    status = compute_ti(dat, work, est, subset, m, est->dist);

    return status;
}

/******************************************************************************\
|* Algorithm 4 of Billor et al. (2000), adapted for weighting                 *|
|*  dat      typedef struct regdata                                           *|
|*  work     typedef struct workarray                                         *|
|*  est      typedef struct estimate                                          *|
|*  subset0  subset, 1: obs. is in the subset, 0 otherwise, array[n]          *|
|*  subset1  subset, 1: obs. is in the subset, 0 otherwise, array[n]          *|
|*  m        number of obs. in subset1                                        *|
|*  verbose  toggle: 1: additional information is printed to the console;     *|
|*           0: quiet                                                         *|
|*  collect  defines size of initial subset: m = collect * p                  *|
\******************************************************************************/
static wbacon_error_type algorithm_4(regdata *dat, workarray *work,
    estimate *est, int* restrict subset0, int* restrict subset1, int *m,
    int *verbose, int *collect)
{
    int n = dat->n, p = dat->p;
    int* restrict iarray = work->iarray;
    wbacon_error_type err;

    if (*verbose)
        PRINT_OUT("Step 1 (Algorithm 4):\n");

    // STEP 1 (Algorithm 4)
    for (;;) {
        if (*verbose)
            PRINT_OUT("  m = %d", *m);

        // update cholesky factor and xty matrix (subset0 => subset1)
        err = update_chol_xty(dat, work, est, subset0, subset1, verbose);

        // check whether L is well defined; if not, keep adding obs. to the
        // subset until it has full rank
        if (err != WBACON_ERROR_OK) {
            for (;;) {
                (*m)++;
                subset1[iarray[*m - 1]] = 1;

                if (*verbose)
                    PRINT_OUT("  m = %d", *m);

                // re-do the updating
                err = update_chol_xty(dat, work, est, subset0, subset1,
                verbose);
                if (err == WBACON_ERROR_OK)
                    break;
                if (*m == p * *collect)
                    return err;
            }
        }

        // prepare next while iteration; then update subset1
        Memcpy(subset0, subset1, n);

        // regression estimate: beta (updated Cholesky factor)
        cholesky_reg(est->L, dat->x, est->xty, est->beta, &n, &p);

        // compute residuals
        const int int_1 = 1;
        const double double_minus1 = -1.0, double_1 = 1.0;
        Memcpy(est->resid, dat->y, n);
        F77_CALL(dgemv)("N", &n, &p, &double_minus1, dat->x, &n, est->beta,
            &int_1, &double_1, est->resid, &int_1);

        // compute t[i]'s
        err = compute_ti(dat, work, est, subset1, m, est->dist);
        if (err != WBACON_ERROR_OK)
            return err;

        // select the m obs. with the smallest t[i]'s
        (*m)++;

        if (*m == p * *collect + 1)
            break;

        select_subset(est->dist, subset1, m, &n);
    }

    return WBACON_ERROR_OK;
}

/******************************************************************************\
|* Algorithm 5 of Billor et al. (2000), adapted for weighting                 *|
|*  dat      typedef struct regdata                                           *|
|*  work     typedef struct workarray                                         *|
|*  est      typedef struct estimate                                          *|
|*  subset0  subset, 1: obs. is in the subset, 0 otherwise, array[n]          *|
|*  subset1  subset, 1: obs. is in the subset, 0 otherwise, array[n]          *|
|*  alpha   level of significance that determines the (1-alpha) quantile of   *|
|*           the Student t-distr. used as a cutoff value for the t[i]'s       *|
|*  m        number of obs. in subset1                                        *|
|*  maxiter  maximum number of iterations                                     *|
|*  verbose  toggle: 1: additional information is printed to the console;     *|
|*           0: quiet                                                         *|
\******************************************************************************/
static wbacon_error_type algorithm_5(regdata *dat, workarray *work,
    estimate *est, int* restrict subset0, int* restrict subset1, double *alpha,
    int *m, int *maxiter, int *verbose)
{
    int n = dat->n, p = dat->p, iter = 1, info, i;
    double cutoff;
    double* restrict L = est->L;
    double* restrict wx = dat->wx;
    double* restrict dist = est->dist;
    wbacon_error_type err;

    if (*verbose)
        PRINT_OUT("Step 2 (Algorithm 5):\n");

    while (iter <= *maxiter) {

#if _debug_mode
print_magic_number(subset1, *m);
#endif

        // weighted least squares (on return, wx is overwritten by the
        // QR factorization)
        info = fitwls(dat, est, subset0, work->dgels_work, work->lwork);
        if (info)
            return WBACON_ERROR_RANK_DEFICIENT;

        // extract L
        for (int i = 0; i < p; i++)
            for (int j = i; j < p; j++)
                L[j + i * p] = wx[i + j * n];

        // compute t[i]'s (dist)
        err = compute_ti(dat, work, est, subset0, m, dist);
        if (err != WBACON_ERROR_OK)
            return err;

        // t-distr. cutoff value (quantile)
        cutoff = qt(*alpha / (double)(2 * (*m + 1)), *m - p, 0, 0);

        // generate new subset that includes all obs. with t[i] < cutoff
        *m = 0;
        for (int i = 0; i < n; i++) {
            if (dist[i] < cutoff) {
                subset1[i] = 1;
                (*m)++;
            } else {
                subset1[i] = 0;
            }
        }

        // check whether the subsets differ
        for (i = 0; i < n; i++)
            if (subset0[i] ^ subset1[i])
                break;

        // if the subsets are identical, we return
        if (i == n) {
            *maxiter = iter;
            return WBACON_ERROR_OK;
        }

        if (*verbose)
            PRINT_OUT("  m = %d\n", *m);

        Memcpy(subset0, subset1, n);
        iter++;
    }

    return WBACON_ERROR_CONVERGENCE_FAILURE;
}

/******************************************************************************\
|* select the smallest m observations of array x[n] into the subset           *|
|*  x       array[n]                                                          *|
|*  subset  on return: array[n], 1: element is in the subset, 0: otherwise    *|
|*  m       size of the subset                                                *|
|*  n       dimension                                                         *|
\******************************************************************************/
static void select_subset(double* restrict x, int* restrict subset, int *m,
    int *n)
{
    // select the m-th smallest element (threshold)
    select_k(x, 0, *n - 1, *m - 1);
    double threshold = x[*m - 1];

    // select the elements smaller than the threshold into the subset
    for (int i = 0; i < *n; i++)
        if (x[i] <= threshold)
            subset[i] = 1;
        else
            subset[i] = 0;
}

/******************************************************************************\
|* Update the cholesky factor and the 'xty' matrix to account for the changes *|
|* between subset0 and subset1                                                *|
|*  dat      typedef struct regdata                                           *|
|*  work     typedef struct workarray                                         *|
|*  est      typedef struct estimate                                          *|
|*  subset0  subset, 1: obs. is in the subset, 0 otherwise, array[n]          *|
|*  subset1  subset, 1: obs. is in the subset, 0 otherwise, array[n]          *|
|*  verbose  toggle: 1: additional information is printed to the console;     *|
|*           0: quiet                                                         *|
\******************************************************************************/
static wbacon_error_type update_chol_xty(regdata *dat, workarray *work,
    estimate *est, int* restrict subset0, int* restrict subset1, int *verbose)
{
    int n = dat->n, p = dat->p;
    int* restrict stack = work->iarray;
    double* restrict xtx_update = work->work_p;
    double* restrict xty = est->xty;
    double* restrict x = dat->x;
    double* restrict y = dat->y;
    double* restrict weight = dat->w;
    wbacon_error_type err;

    // make copies of L and xty (to restore the arrays if the updating fails)
    Memcpy(work->work_pp, est->L, p * p);
    Memcpy(work->work_np, xty, p);

    // first pass: make updates to L and xty (if required) and put the
    // downdates to the 'stack'
    int n_update = 0, n_downdate = 0;
    for (int i = 0; i < n; i++) {
        if (subset1[i] > subset0[i]) {
            // updates
            for (int j = 0; j < p; j++) {
                xtx_update[j] = x[i + j * n] * sqrt(weight[i]);
                xty[j] += x[i + j * n] * y[i] * weight[i];
            }
            chol_update(est->L, xtx_update, p);
            n_update++;
        } else {
            // downdates are put onto the stack
            if (subset1[i] < subset0[i]) {
                stack[n_downdate] = i;
                n_downdate++;
            }
        }
    }

    // in the second pass, we consider the downdates (which may turn L into a
    // rank deficient matrix)
    int at;
    for (int i = 0; i < n_downdate; i++) {
        at = stack[i];
        for (int j = 0; j < p; j++) {
            xtx_update[j] = x[at + j * n] * sqrt(weight[at]);
            xty[j] -= x[at + j * n] * y[at] * weight[at];
        }
        err = chol_downdate(est->L, xtx_update, p);
        if (err != WBACON_ERROR_OK) {
            // updating failed: restore the original arrays
            Memcpy(est->L, work->work_pp, p * p);
            Memcpy(xty, work->work_np, p);
            if (*verbose)
                PRINT_OUT(" (downdate failed, subset is increased)\n");
            return err;
        }
    }
    if (*verbose)
        PRINT_OUT(" (%d up- and %d downdates)\n", n_update, n_downdate);

    return WBACON_ERROR_OK;
}

/******************************************************************************\
|* Rank-one update of the (lower triangular) Cholesky factor                  *|
|*  L   Cholesky factor (lower triangular), array[p, p]                       *|
|*  u   rank-one update, array[p]                                             *|
|*  p   dimension                                                             *|
|*                                                                            *|
|* Golub, G.H, and Van Loan, C.F. (1996). Matrix Computations, 3rd. ed.,      *|
|* Baltimore: The Johns Hopkins University Press, ch. 12.5                    *|
\******************************************************************************/
static inline void chol_update(double* restrict L, double* restrict u, int p)
{
    double a, b, c, tmp;
    for (int i = 0; i < p - 1; i++) {
        tmp = L[i * (p + 1)];               // element L[i,i]
        a = hypot(tmp, u[i]);
        b = a / tmp;
        c = u[i] / tmp;
        L[i * (p + 1)] = a;                 // element L[i,i]

        for (int j = i + 1; j < p; j++) {   // off-diagonal elements
            L[p * i + j] += c * u[j];
            L[p * i + j] /= b;
            u[j] = b * u[j] - c * L[p * i + j];
        }
    }
    L[p * p - 1] = sqrt(_POWER2(L[p * p - 1]) + _POWER2(u[p - 1]));
}

/******************************************************************************\
|* Rank-one downdate of the (lower triangular) Cholesky factor                *|
|*  L   Cholesky factor (lower triangular), array[p, p]                       *|
|*  u   rank-one update, array[p]                                             *|
|*  p   dimension                                                             *|
|*                                                                            *|
|* NOTE: downdating can turn a full rank matrix into a rank deficient matrix  *|
|* Golub, G.H, and Van Loan, C.F. (1996). Matrix Computations, 3rd. ed.,      *|
|* Baltimore: The Johns Hopkins University Press, ch. 12.5                    *|
\******************************************************************************/
static inline wbacon_error_type chol_downdate(double* restrict L,
    double* restrict u, int p)
{
    double a, b, c, tmp;
    for (int i = 0; i < p - 1; i++) {
        tmp = L[i * (p + 1)];                   // element L[i,i]
        a = _POWER2(tmp) - _POWER2(u[i]);
        if (a < 0.0)
            return WBACON_ERROR_RANK_DEFICIENT;
        a = sqrt(a);
        b = a / tmp;
        c = u[i] / tmp;
        L[i * (p + 1)] = a;                     // element L[i,i]

        for (int j = i + 1; j < p; j++) {       // off-diagonal elements
            L[p * i + j] -= c * u[j];
            L[p * i + j] /= b;
            u[j] = b * u[j] - c * L[p * i + j];
        }
    }

    a = _POWER2(L[p * p - 1]) - _POWER2(u[p - 1]);
    if (a < 0.0)
        return WBACON_ERROR_RANK_DEFICIENT;

    L[p * p - 1] = sqrt(a);
    return WBACON_ERROR_OK;
}

/******************************************************************************\
|* Distance measure t[i] of Billor et al. (2000, Eq. 6)                       *|
|*  dat      typedef struct regdata                                           *|
|*  work     typedef struct workarray                                         *|
|*  est      typedef struct estimate                                          *|
|*  subset   subset, 1: obs. is in the subset, 0 otherwise, array[n]          *|
|*  m        number of obs. in subset                                         *|
|*  tis      on entry: work array[n]; on return: t[i]'s                       *|
\******************************************************************************/
static wbacon_error_type compute_ti(regdata *dat, workarray *work,
    estimate *est, int* restrict subset, int *m, double* restrict tis)
{
    int n = dat->n;
    double* restrict resid = est->resid;
    double* restrict hat = work->work_n;

    // compute the diag. of the 'hat' matrix
    wbacon_error_type err = hat_matrix(dat, work, est->L, hat);
    if (err != WBACON_ERROR_OK)
        return err;

    // compute t[i]'s (branchless)
    double tmp;
    for (int i = 0; i < n; i++) {
        tmp = 1 + (double)(1 - 2 * subset[i]) * hat[i];
        tis[i] = fabs(resid[i]) / (est->sigma * sqrt(tmp));
    }

    return WBACON_ERROR_OK;
}

/******************************************************************************\
|* Least squares estimate (Cholesky factorization)                            *|
|*  L     Cholesky factor (lower triangular), array[p, p]                     *|
|*  x     design matrix, array[n]                                             *|
|*  xty   X^T * y, array[p]                                                   *|
|*  beta  on return: regression coefficients, array[p]                        *|
|*  n, p  dimension                                                           *|
\******************************************************************************/
static inline void cholesky_reg(double *L, double *x, double *xty, double *beta,
    int *n, int *p)
{
    const int int_one = 1;
    double const d_one = 1.0;

    // solve for 'a' (return in beta) in the triangular system L^T * a = xty
    Memcpy(beta, xty, *p);
    F77_CALL(dtrsm)("L", "L", "N", "N", p, &int_one, &d_one, L, p, beta, p);

    // solve for 'beta' in the triangular system L * beta  = a
    F77_CALL(dtrsm)("L", "L", "T", "N", p, &int_one, &d_one, L, p, beta, p);
}

/******************************************************************************\
|* Diagonal elements of the 'hat' matrix                                      *|
|*  dat      typedef struct regdata                                           *|
|*  work     typedef struct workarray                                         *|
|*  L        Cholesky factor (lower triangular), array[p, p]                  *|
|*  hat      diagonal elements of the 'hat' matrix                            *|
\******************************************************************************/
static inline wbacon_error_type hat_matrix(regdata *dat, workarray *work,
    double* restrict L, double* restrict hat)
{
    int n = dat->n, p = dat->p;
    double* restrict weight = dat->w;
    double* restrict work_np = work->work_np;

    // invert the cholesky factor L
    int info;
    Memcpy(work->work_pp, L, p * p);
    F77_CALL(dtrtri)("L", "N", &p, work->work_pp, &p, &info);
    if (info != 0)
        return WBACON_ERROR_TRIANG_MAT_SINGULAR;

    // triangular matrix multiplication x * L^{-T}
    const double d_one = 1.0;
    Memcpy(work_np, dat->x, n * p);
    F77_CALL(dtrmm)("R", "L", "T", "N", &n, &p, &d_one, work->work_pp, &p,
        work_np, &n);

    // diagonal elements of hat matrix = row sums of (x * L^{-T})^2
    for (int i = 0; i < n; i++)
        hat[i] = _POWER2(work_np[i]);

    #pragma omp parallel for if(p * n > REG_OMP_MIN_SIZE)
    for (int j = 1; j < p; j++) {
        #pragma omp simd
        for (int i = 0; i < n; i++) {
            hat[i] += _POWER2(work_np[i + j * n]);
        }
    }

    for (int i = 0; i < n; i++)
        hat[i] *= weight[i];

    return WBACON_ERROR_OK;
}
#undef _POWER2
