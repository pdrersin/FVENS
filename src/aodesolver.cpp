/** @file aodesolver.cpp
 * @brief Implements driver class(es) for solution of ODEs arising from PDE discretizations
 * @author Aditya Kashi
 * @date Feb 24, 2016
 *
 * This file is part of FVENS.
 *   FVENS is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   FVENS is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with FVENS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "aodesolver.hpp"
#include <algorithm>

#ifdef _OPENMP
#include <omp.h>
#endif

#include <petscksp.h>

namespace acfd {

/// Returns an array containing TVD Runge-Kutta coefficients for high-order accuracy
static Matrix<a_real,Dynamic,Dynamic> initialize_TVDRK_Coeffs(const int _order) 
{
	Matrix<a_real,Dynamic,Dynamic> tvdrk(_order,3);
	if(_order == 1) {
		tvdrk(0,0) = 1.0;	tvdrk(0,1) = 0.0; tvdrk(0,2) = 1.0;
	}
	else if(_order == 2) {
		tvdrk(0,0) = 1.0;	tvdrk(0,1) = 0.0; tvdrk(0,2) = 1.0;
		tvdrk(1,0) = 0.5;	tvdrk(1,1) = 0.5;	tvdrk(1,2) = 0.5;
	}
	else if(_order == 3)
	{
		tvdrk(0,0) = 1.0;      tvdrk(0,1) = 0.0;  tvdrk(0,2) = 1.0;
		tvdrk(1,0) = 0.75;	    tvdrk(1,1) = 0.25; tvdrk(1,2) = 0.25;
		tvdrk(2,0) = 0.3333333333333333; 
		tvdrk(2,1) = 0.6666666666666667; 
		tvdrk(2,2) = 0.6666666666666667;
	}
	else {
		std::cout << "! Temporal order " << _order  << " not available!\n";
	}
	return tvdrk;
}

template<short nvars>
SteadyForwardEulerSolver<nvars>::SteadyForwardEulerSolver(
		const Spatial<nvars> *const spatial,
		const SteadySolverConfig& conf,
		const bool use_implicitSmoothing, AbstractMatrix<a_real,a_int> *const A)

	: SteadySolver<nvars>(spatial, conf), useImplicitSmoothing{use_implicitSmoothing}, M{A},
	linsolv{nullptr}, prec{nullptr}
{
	const UMesh2dh *const m = space->mesh();

	residual.resize(m->gnelem(),nvars);
	dtm.setup(m->gnelem(), 1);
}

template<short nvars>
SteadyForwardEulerSolver<nvars>::~SteadyForwardEulerSolver()
{
	delete linsolv;
	delete prec;
}

template<short nvars>
void SteadyForwardEulerSolver<nvars>::solve(MVector& u)
{
	const UMesh2dh *const m = space->mesh();

	if(config.maxiter <= 0) {
		std::cout << " SteadyForwardEulerSolver: solve(): No iterations to be done.\n";
		return;
	}

	int step = 0;
	a_real resi = 1.0;
	a_real initres = 1.0;

	std::ofstream convout;
	if(config.lognres)
		convout.open(config.logfile+".conv", std::ofstream::app);
	
	struct timeval time1, time2;
	gettimeofday(&time1, NULL);
	double initialwtime = (double)time1.tv_sec + (double)time1.tv_usec * 1.0e-6;
	double initialctime = (double)clock() / (double)CLOCKS_PER_SEC;

	while(resi/initres > config.tol && step < config.maxiter)
	{
#pragma omp parallel for simd default(shared)
		for(a_int iel = 0; iel < m->gnelem(); iel++) {
			for(short i = 0; i < nvars; i++)
				residual(iel,i) = 0;
		}

		// update residual
		space->compute_residual(u, residual, true, dtm);

		a_real errmass = 0;

#pragma omp parallel default(shared)
		{
#pragma omp for simd
			for(a_int iel = 0; iel < m->gnelem(); iel++)
			{
				for(short i = 0; i < nvars; i++)
				{
					u(iel,i) -= config.cflinit*dtm(iel) * 1.0/m->garea(iel)*residual(iel,i);
				}
			}

#pragma omp for simd reduction(+:errmass)
			for(a_int iel = 0; iel < m->gnelem(); iel++)
			{
				errmass += residual(iel,nvars-1)*residual(iel,nvars-1)*m->garea(iel);
			}
		} // end parallel region

		resi = sqrt(errmass);

		if(step == 0)
			initres = resi;

		if(step % 50 == 0)
			std::cout << "  SteadyForwardEulerSolver: solve(): Step " << step 
				<< ", rel residual " << resi/initres << std::endl;

		step++;
		if(config.lognres)
			convout << step << " " << std::setw(10) << resi/initres << '\n';
	}

	if(config.lognres)
		convout.close();
	
	gettimeofday(&time2, NULL);
	double finalwtime = (double)time2.tv_sec + (double)time2.tv_usec * 1.0e-6;
	double finalctime = (double)clock() / (double)CLOCKS_PER_SEC;
	walltime += (finalwtime-initialwtime); cputime += (finalctime-initialctime);

	if(step == config.maxiter)
		std::cout << "! SteadyForwardEulerSolver: solve(): Exceeded max iterations!\n";
	std::cout << " SteadyForwardEulerSolver: solve(): Done, steps = " << step << "\n\n";
	std::cout << " SteadyForwardEulerSolver: solve(): Time taken by ODE solver:\n";
	std::cout << "                                   CPU time = " << cputime 
		<< ", wall time = " << walltime << std::endl << std::endl;

	// append data to log file
	int numthreads = 0;
#ifdef _OPENMP
	numthreads = omp_get_max_threads();
#endif
	std::ofstream outf; outf.open(config.logfile, std::ofstream::app);
	outf << "\t" << numthreads << "\t" << walltime << "\t" << cputime << "\n";
	outf.close();
}

/** By default, the Jacobian is stored in a block sparse row format.
 */
template <short nvars>
SteadyBackwardEulerSolver<nvars>::SteadyBackwardEulerSolver(
		const Spatial<nvars> *const spatial, 
		const SteadySolverConfig& conf,	
		AbstractMatrix<a_real,a_int> *const pmat)

	: SteadySolver<nvars>(spatial, conf), M{pmat}
{
	const UMesh2dh *const m = space->mesh();

	// NOTE: the number of columns here MUST match the static number of columns, which is nvars.
	residual.resize(m->gnelem(),nvars);
	dtm.setup(m->gnelem(), 1);

	// select preconditioner
	if(config.preconditioner == "J") {
		prec = new Jacobi<nvars>(M);
		std::cout << " SteadyBackwardEulerSolver: Selected ";
		std::cout << "Jacobi preconditioner.\n";
	}
	else if(config.preconditioner == "SGS") {
		prec = new SGS<nvars>(M);
		std::cout << " SteadyBackwardEulerSolver: Selected ";
		std::cout << "SGS preconditioner.\n";
	}
	else if(config.preconditioner == "ILU0") {
		prec = new ILU0<nvars>(M);
		std::cout << " SteadyBackwardEulerSolver: Selected ";
		std::cout << " ILU0 preconditioner.\n";
	}
	else {
		prec = new NoPrec<nvars>(M);
		std::cout << " SteadyBackwardEulerSolver: No preconditioning will be applied.\n";
	}

	if(config.linearsolver == "BCGSTB") {
		linsolv = new BiCGSTAB<nvars>(m, M, prec);
		std::cout << " SteadyBackwardEulerSolver: BiCGStab solver selected.\n";
	}
	else if(config.linearsolver == "GMRES") {
		linsolv = new GMRES<nvars>(m, M, prec, config.restart_vecs);
		std::cout << " SteadyBackwardEulerSolver: GMRES solver selected, restart after " 
			<< config.restart_vecs << " iterations\n";
	}
	else {
		linsolv = new RichardsonSolver<nvars>(m, M, prec);
		std::cout << " SteadyBackwardEulerSolver: Richardson iteration selected, no acceleration.\n";
	}
}

template <short nvars>
SteadyBackwardEulerSolver<nvars>::~SteadyBackwardEulerSolver()
{
	delete linsolv;
	delete prec;
}

template <short nvars>
StatusCode SteadyBackwardEulerSolver<nvars>::solve(MVector& u)
{
	StatusCode ierr = 0;
	const UMesh2dh *const m = space->mesh();

	if(config.maxiter <= 0) {
		std::cout << " SteadyBackwardEulerSolver: solve(): No iterations to be done.\n";
		return;
	}

	a_real curCFL; int curlinmaxiter;
	int step = 0;
	a_real resi = 1.0;
	a_real initres = 1.0;
	MVector du = MVector::Zero(m->gnelem(), nvars);

	std::ofstream convout;
	if(config.lognres)
		convout.open(config.logfile+".conv", std::ofstream::app);
	
	struct timeval time1, time2;
	gettimeofday(&time1, NULL);
	double initialwtime = (double)time1.tv_sec + (double)time1.tv_usec * 1.0e-6;
	double initialctime = (double)clock() / (double)CLOCKS_PER_SEC;
	
	int avglinsteps = 0;

	walltime = cputime = 0;
	linsolv->resetRunTimes();

	while(resi/initres > config.tol && step < config.maxiter)
	{
#pragma omp parallel for default(shared)
		for(a_int iel = 0; iel < m->gnelem(); iel++) {
#pragma omp simd
			for(short i = 0; i < nvars; i++) {
				residual(iel,i) = 0;
			}
		}

		M->setAllZero();
		
		// update residual and local time steps
		space->compute_residual(u, residual, true, dtm);

		space->compute_jacobian(u, M);
		
		// compute ramped quantities
		if(step < config.rampstart) {
			curCFL = config.cflinit;
			curlinmaxiter = config.linmaxiterstart;
		}
		else if(step < config.rampend) {
			if(config.rampend-config.rampstart <= 0) {
				curCFL = config.cflfin;
				curlinmaxiter = config.linmaxiterend;
			}
			else {
				const a_real slopec = (config.cflfin-config.cflinit)
					                    /(config.rampend-config.rampstart);
				curCFL = config.cflinit + slopec*(step-config.rampstart);
				
				const a_real slopei = a_real(config.linmaxiterend-config.linmaxiterstart)
					                    /(config.rampend-config.rampstart);
				curlinmaxiter = int(config.linmaxiterstart + slopei*(step-config.rampstart));
			}
		}
		else {
			curCFL = config.cflfin;
			curlinmaxiter = config.linmaxiterend;
		}

		// add pseudo-time terms to diagonal blocks

#pragma omp parallel for default(shared)
		for(a_int iel = 0; iel < m->gnelem(); iel++)
		{
			Matrix<a_real,nvars,nvars,RowMajor> db 
				= Matrix<a_real,nvars,nvars,RowMajor>::Zero();

			for(short i = 0; i < nvars; i++)
				db(i,i) = m->garea(iel) / (curCFL*dtm(iel));
			
			M->updateDiagBlock(iel*nvars, db.data(), nvars);
		}

		MatAssemblyBegin(M, MAT_FINAL_ASSEMBLY);
		MatAssemblyEnd(M, MAT_FINAL_ASSEMBLY);
	
		/// Freezes the non-zero structure for efficiency in subsequent time steps.
		ierr = MatSetOption(M, MAT_NEW_NONZERO_LOCATIONS, PETSC_FALSE); CHKERRQ(ierr);

		// setup and solve linear system for the update du
		
		linsolv->setupPreconditioner();
		linsolv->setParams(config.lintol, curlinmaxiter);
		int linstepsneeded = linsolv->solve(residual, du);
		avglinsteps += linstepsneeded;
		
		a_real resnorm2 = 0;

#pragma omp parallel default(shared)
		{
#pragma omp for
			for(a_int iel = 0; iel < m->gnelem(); iel++) {
				u.row(iel) += du.row(iel);
			}
#pragma omp for simd reduction(+:resnorm2)
			for(a_int iel = 0; iel < m->gnelem(); iel++)
			{
				resnorm2 += residual(iel,nvars-1)*residual(iel,nvars-1)*m->garea(iel);
			}
		}

		resi = sqrt(resnorm2);

		if(step == 0)
			initres = resi;

		if(step % 10 == 0) {
			std::cout << "  SteadyBackwardEulerSolver: solve(): Step " << step 
				<< ", rel residual " << resi/initres << std::endl;
			std::cout << "      CFL = " << curCFL << ", Lin max iters = " << curlinmaxiter 
				<< ", iters used = " << linstepsneeded << std::endl;
		}

		step++;
			
		if(config.lognres)
			convout << step << " " << std::setw(10)  << resi/initres << '\n';
	}

	gettimeofday(&time2, NULL);
	double finalwtime = (double)time2.tv_sec + (double)time2.tv_usec * 1.0e-6;
	double finalctime = (double)clock() / (double)CLOCKS_PER_SEC;
	walltime += (finalwtime-initialwtime); cputime += (finalctime-initialctime);
	avglinsteps /= step;

	if(config.lognres)
		convout.close();

	if(step == config.maxiter)
		std::cout << "! SteadyBackwardEulerSolver: solve(): Exceeded max iterations!\n";
	std::cout << " SteadyBackwardEulerSolver: solve(): Done, steps = " << step << ", rel residual " 
		<< resi/initres << std::endl;

	// print timing data
	double linwtime, linctime;
	linsolv->getRunTimes(linwtime, linctime);
	std::cout << "\n SteadyBackwardEulerSolver: solve(): Time taken by linear solver:\n";
	std::cout << " \t\tWall time = " << linwtime << ", CPU time = " << linctime << std::endl;
	std::cout << "\t\tAverage number of linear solver iterations = " << avglinsteps << std::endl;
	std::cout << "\n SteadyBackwardEulerSolver: solve(): Time taken by ODE solver:" << std::endl;
	std::cout << " \t\tWall time = " << walltime << ", CPU time = " << cputime << "\n\n";

	// append data to log file
	int numthreads = 0;
#ifdef _OPENMP
	numthreads = omp_get_max_threads();
#endif
	std::ofstream outf; outf.open(config.logfile, std::ofstream::app);
	outf << std::setw(10) << m->gnelem() << " "
		<< std::setw(6) << numthreads << " " << std::setw(10) << linwtime << " " 
		<< std::setw(10) << linctime << " " << std::setw(10) << avglinsteps << " "
		<< std::setw(10) << step
		<< "\n";
	outf.close();
}

template <short nvars>
TVDRKSolver<nvars>::TVDRKSolver(const Spatial<nvars> *const spatial, 
		MVector& soln, const int temporal_order, const std::string log_file, const double cfl_num)
	: UnsteadySolver<nvars>(spatial, soln, temporal_order, log_file), cfl{cfl_num},
	tvdcoeffs(initialize_TVDRK_Coeffs(temporal_order))
{
	dtm.setup(space->mesh()->gnelem(), 1);
}

template<short nvars>
void TVDRKSolver<nvars>::solve(const a_real finaltime)
{
	const UMesh2dh *const m = space->mesh();

	int step = 0;
	a_real time = 0;   //< Physical time elapsed
	a_real dtmin;      //< Time step

	// Stage solution vector
	MVector ustage(m->gnelem(),nvars);
	for(a_int iel = 0; iel < m->gnelem(); iel++)
		for(int ivar = 0; ivar < nvars; ivar++)
			ustage(iel,ivar) = u(iel,ivar);
	
	struct timeval time1, time2;
	gettimeofday(&time1, NULL);
	double initialwtime = (double)time1.tv_sec + (double)time1.tv_usec * 1.0e-6;
	double initialctime = (double)clock() / (double)CLOCKS_PER_SEC;

	while(time <= finaltime - A_SMALL_NUMBER)
	{
		for(int istage = 0; istage < order; istage++)
		{
#pragma omp parallel for simd default(shared)
			for(a_int iel = 0; iel < m->gnelem(); iel++) {
				for(short i = 0; i < nvars; i++)
					residual(iel,i) = 0;
			}

			// update residual
			space->compute_residual(u, residual, true, dtm);

			// update time step for the first stage of each time step
			if(istage == 0)
				dtmin = dtm.min();

#pragma omp parallel for simd default(shared)
			for(a_int iel = 0; iel < m->gnelem(); iel++)
			{
				for(short i = 0; i < nvars; i++)
				{
					//u(iel,i) -= cfl*dtmin * 1.0/m->garea(iel)*residual(iel,i);
					ustage(iel,i) = tvdcoeffs(istage,0)*u(iel,i)
						          + tvdcoeffs(istage,1)*ustage(iel,i)
								  - tvdcoeffs(istage,2) * dtmin*cfl/m->garea(iel)*residual(iel,i);
				}
			}
		}

#pragma omp parallel for simd default(shared)
		for(a_int iel = 0; iel < m->gnelem(); iel++)
			for(int ivar = 0; ivar < nvars; ivar++)
				u(iel,ivar) = ustage(iel,ivar);


		if(step % 50 == 0)
			std::cout << "  TVDRKSolver: solve(): Step " << step 
				<< ", time " << time << std::endl;

		step++;
		time += dtmin;
	}
	
	gettimeofday(&time2, NULL);
	double finalwtime = (double)time2.tv_sec + (double)time2.tv_usec * 1.0e-6;
	double finalctime = (double)clock() / (double)CLOCKS_PER_SEC;
	walltime += (finalwtime-initialwtime); cputime += (finalctime-initialctime);

	std::cout << " TVDRKSolver: solve(): Done, steps = " << step << "\n\n";
	std::cout << " TVDRKSolver: solve(): Time taken by ODE solver:\n";
	std::cout << "                                   CPU time = " << cputime 
		<< ", wall time = " << walltime << std::endl << std::endl;

	// append data to log file
	int numthreads = 0;
#ifdef _OPENMP
	numthreads = omp_get_max_threads();
#endif
	std::ofstream outf; outf.open(logfile, std::ofstream::app);
	outf << "\t" << numthreads << "\t" << walltime << "\t" << cputime << "\n";
	outf.close();
}

template class SteadyForwardEulerSolver<NVARS>;
template class SteadyBackwardEulerSolver<NVARS>;
template class SteadyForwardEulerSolver<1>;
template class SteadyBackwardEulerSolver<1>;

}	// end namespace
