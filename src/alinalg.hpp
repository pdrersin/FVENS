/** @file alinalg.hpp
 * @brief Dense linear algebra subroutines
 * @author Aditya Kashi
 */

#ifndef __ALINALG_H

#ifndef __AMATRIX_H
#include <amatrix.hpp>
#endif

#define __ALINALG_H

namespace amat {

/// Solves Ax=b for dense A by Gaussian elimination
void gausselim(Matrix<acfd_real>& A, Matrix<acfd_real>& b, Matrix<acfd_real>& x);

/// Base class for AF (approximate factorization)-type iterative linear solver
class IterativeSolver
{
protected:
	const int nvars;						///< Number of conserved variables
	const UMesh2dh* const m;				///< mesh
	const Matrix<acfd_real>* const diag;	///< Diagonal blocks
	const Matrix<acfd_real>* const res;		///< Vector of residuals at previous iteration
	Matrix<acfd_real>* const u;				///< Vector of unknowns
public:
	IterativeSolver(const int num_vars, const UMesh2dh* const mesh, const Matrix<acfd_real>* const diagonal_blocks, const Matrix<acfd_real>* const residual, Matrix<acfd_real>* const unk) 
		: nvars(num_vars), m(mesh), diag(diagonal_blocks), res(residual), u(unk)
	{ }

	/// Supposed to carry a single step of the corresponding AF solver
	virtual void update() = 0;

	virtual ~IterativeSolver()
	{ }
};

/// Base class for matrix-free iterative solvers of AF-type
/** A FluxFunction object allows us to compute inviscid flux corresponding to a given state vector.
 */
class MatrixFreeIterativeSolver : public IterativeSolver
{
protected:
	const FluxFunction* const invf;
public:
	MatrixFreeIterativeSolver(const int num_vars, const UMesh2dh* const mesh, const FluxFunction* const inviscid_flux,
			const Matrix<acfd_real>* const diagonal_blocks, const Matrix<acfd_real>* const residual, Matrix<acfd_real>* const delta_u) 
		: IterativeSolver(num_vars, mesh, diagonal_blocks, residual, delta_u), invf(inviscid_flux)
	{ }
	virtual void update() = 0;
};

/// Matrix-free LU-SGS solver
/** Reference: 
 * H. Luo, D. Sharov, J.D. Baum and R. Loehner. "On the Computation of Compressible Turbulent Flows on Unstructured Grids". Internation Journal of Computational Fluid Dynamics Vol 14, No 4, pp 253-270. 2001.
 */
class LUSGS_Solver : public MatrixFreeIterativeSolver
{
	Matrix<acfd_real>* du;
	Matrix<acfd_real> f1;
	Matrix<acfd_real> f2;
	Matrix<acfd_real> uel;
	Matrix<acfd_real> uelpdu;
	acfd_int ielem, jelem, iface;
	acfd_real s, sum;
	acfd_real n[NDIM];
	int jfa;
	int ivar;
	int ip1[NDIM], ip2[NDIM];
public:

	LUSGS_Solver(const int num_vars, const UMesh2dh* const mesh, const FluxFunction* const inviscid_flux,
			const Matrix<acfd_real>* const diagonal_blocks, const Matrix<acfd_real>* const residual, Matrix<acfd_real>* const u);

	~LUSGS_Solver();
	
	/// Carries out a single step (one forward followed by one backward sweep) of SGS
	/** The equation being solved is assumed to be
	 * \f$ \mathbf{M} \frac{d\mathbf{u}}{dt} = \mathbf{R} \f$
	 */
	void update();
};
	
}
#endif
