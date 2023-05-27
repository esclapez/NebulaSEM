#include "solve.h"
#include "iteration.h"
#include "properties.h"
#include "turbulence.h"
#include "calc_walldist.h"
#include "wrapper.h"

using namespace std;

/**
  \verbatim
  Navier stokes solver using PISO algorithm
  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  References:
     Hrvoje Jasak, "Error analysis and estimation of FVM with 
     applications to fluid flow".
  Description:
     The PISO algorithm is used to solve NS equations on collocated grids 
     using Rhie-Chow interpolation to avoid wiggles in pressure field.
 
     Prediction
     ~~~~~~~~~~
     Discretize and solve the momenum equation with current values of pressure. 
     The velocities obtained will not satisfy continuity unless exact pressure 
     happened to be specified. 
 
     Correction
     ~~~~~~~~~~
     Step 1) 
       Determine velocity with all terms included except pressure gradient source contribution.
           ap * Up = H(U) - grad(p)
           Up = H(U) / ap - grad(p) / ap
       Droping grad(p) term:
           Ua = H(U) / ap
           Up = Ua - grad(p) / ap
       One jacobi sweep is done to find Ua.
     Step 2)
       Solve poisson pressure equation to satisfy continuity with fluxes calculated 
       from interpolated Ua.
           div(Up) = 0
           div(1/ap * grad(p)) = div(H(U)/ap)
           lap(p,1/ap) = div(Ua)
     Step 3)
       Correct the velocity with gradient of newly found pressure
           U = Ua - grad(p) / ap
     These steps are repeated two or more times for transient solutions.
     For steady state problems once is enough.
     \endverbatim
 */
void piso(std::istream& input) {
    /*Solver specific parameters*/
    Scalar velocity_UR = Scalar(0.8);
    Scalar pressure_UR = Scalar(0.5);
    Scalar t_UR = Scalar(0.8);
    Int n_PISO = 1;
    Int n_ORTHO = 0;
    Int momentum_predictor = 0;
    /*Include buoyancy?*/
    enum BOUYANCY {
        NONE, BOUSSINESQ_T1, BOUSSINESQ_T2, 
        BOUSSINESQ_THETA1, BOUSSINESQ_THETA2,
    };
    BOUYANCY buoyancy = NONE;

    /*fluid properties*/
    {
        Util::ParamList params("general");
        Fluid::enroll(params);
        params.read(input);
    }

    /*piso options*/
    Util::ParamList params("piso");
    Util::Option* op = new Util::Option(&buoyancy,
            {"NONE", "BOUSSINESQ_T1","BOUSSINESQ_T2",
            "BOUSSINESQ_THETA1","BOUSSINESQ_THETA2"});
    params.enroll("buoyancy", op);
    params.enroll("velocity_UR", &velocity_UR);
    params.enroll("pressure_UR", &pressure_UR);
    params.enroll("t_UR", &t_UR);
    params.enroll("n_PISO", &n_PISO);
    params.enroll("n_ORTHO", &n_ORTHO);
    op = new Util::BoolOption(&momentum_predictor);
    params.enroll("momentum_predictor",op);

    Turbulence_Model::RegisterTable(params);
    params.read(input);

    /*AMR iteration*/
    for (AmrIteration ait; !ait.end(); ait.next()) {
        ScalarCellField rho = Fluid::density;
        ScalarCellField mu = rho * Fluid::viscosity;
        VectorCellField U("U", READWRITE);
        ScalarCellField p("p", READWRITE);
        ScalarCellField T(false);

        /*temperature*/
        if(buoyancy != NONE)
            T.construct("T",READWRITE);

        /*turbulence model*/
        VectorCellField Fc;
        ScalarFacetField F;
        Turbulence_Model* turb = Turbulence_Model::Select(U, Fc, F, rho, mu);

        /*read parameters*/
        if(ait.start()) {
            turb->enroll();
            Util::read_params(input,MP::printOn);
        }

        /*wall distance*/
        if (turb->needWallDist())
            Mesh::calc_walldist(ait.get_step(), 2);

        /*Time loop*/
        Iteration it(ait.get_step());
        ScalarCellField po = p;
        VectorCellField gP = -gradf(p);

        Fc = flxc(rho * U);
        F = flx(rho * U);

        for (; !it.end(); it.next()) {
            /*
             * Prediction
             */
            VectorCellMatrix M;
            {
                VectorCellField Sc = turb->getExplicitStresses();
                const ScalarCellField eddy_mu = turb->getTurbVisc();
                /* Add buoyancy in two ways
                 *  1. pm = p - rho*g*h
                 *  2. pm = p - rho_m*g*h
                 */
                if (buoyancy != NONE) {
                    Scalar beta;
                    if(buoyancy <= BOUSSINESQ_T2) 
                        beta = Fluid::beta;
                    else
                        beta = 1 / Fluid::T0;
                    if (buoyancy == BOUSSINESQ_T1 || buoyancy == BOUSSINESQ_THETA1) {  
                        ScalarCellField rhok = rho * (0 - beta * (T - Fluid::T0));
                        Sc += (rhok * VectorCellField(Controls::gravity));
                    } else if(buoyancy == BOUSSINESQ_T2 || buoyancy == BOUSSINESQ_THETA2) {
                        ScalarCellField gz = dot(Mesh::cC,VectorCellField(Controls::gravity));
                        Sc += gz * (rho * beta) * gradi(T);
                    }
                }
                /*momentum prediction*/
                {
                    const ScalarCellField eff_mu = eddy_mu + mu;
                    M = transport(U, Fc, F, eff_mu, velocity_UR, Sc, Scalar(0));
                    if(momentum_predictor) {
                        Solve(M == gP);
                    }
                }
            }

            /*
             * Correction
             */
            const ScalarCellField api = fillBCs<Scalar>(1.0 / M.ap);
            const ScalarCellField rmu = rho * api * Mesh::cV;

            /*PISO loop*/
            for (Int j = 0; j < n_PISO; j++) {
                /* Ua = H(U) / ap*/
                U = getRHS(M,*M.cF) * api;
                applyExplicitBCs(U, true);

                /*solve pressure poisson equation to satisfy continuity*/
                {
                    const ScalarCellField rhs = divf(rho * U);
                    for (Int k = 0; k <= n_ORTHO; k++)
                        Solve(lap(p, rmu, true) += rhs);
                }

                /*explicit velocity correction : add pressure contribution*/
                gP = -gradf(p);
                U -= gP * api;
                applyExplicitBCs(U, true);
            }

            /*update fluctuations*/
            applyExplicitBCs(U, true, true);
            Fc = flxc(rho * U);
            F = flx(rho * U);

            /*solve turbulence transport equations*/
            turb->solve();

            /*solve energy transport*/
            if (buoyancy != NONE) {
                const ScalarCellField eddy_mu = turb->getTurbVisc();
                const ScalarCellField eff_mu = eddy_mu / Fluid::Prt + mu / Fluid::Pr;
                ScalarCellMatrix Mt = transport(T, Fc, F, eff_mu, t_UR);
                Solve(Mt);
                T = max(T, Constants::MachineEpsilon);
            }

            /*explicitly under relax pressure*/
            if (Controls::state == Controls::STEADY) {
                p = po + (p - po) * pressure_UR;
                gP = -gradf(p);
                po = p;
            }
        }

        /*write calculated turbulence fields*/
        if (turb->writeStress) {
            ScalarCellField K("Ksgs", WRITE);
            STensorCellField R("Rsgs", WRITE);
            STensorCellField V("Vsgs", WRITE);
            K = turb->getK();
            R = turb->getReynoldsStress();
            V = turb->getViscousStress();
            Mesh::write_fields(ait.get_step());
        }

        delete turb;
    }
}

/**
  \verbatim
  Main application entry point for piso solver.
  \endverbatim
 */
int main(int argc, char* argv[]) {
   MP mp(argc, argv);
   Solver::Initialize(argc, argv);
   piso(Solver::input);
   Solver::Finalize();
   return 0;
}
