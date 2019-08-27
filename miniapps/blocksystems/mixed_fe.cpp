//                       MFEM Example 5 - Parallel Version
//
// Compile with: make ex5p
//
// Sample runs:  mpirun -np 4 ex5p -m ../data/square-disc.mesh
//               mpirun -np 4 ex5p -m ../data/star.mesh
//               mpirun -np 4 ex5p -m ../data/beam-tet.mesh
//               mpirun -np 4 ex5p -m ../data/beam-hex.mesh
//               mpirun -np 4 ex5p -m ../data/escher.mesh
//               mpirun -np 4 ex5p -m ../data/fichera.mesh
//
// Description:  This example code solves a simple 2D/3D mixed Darcy problem
//               corresponding to the saddle point system
//                                 k*u + grad p = f
//                                 - div u      = g
//               with natural boundary condition -p = <given pressure>.
//               Here, we use a given exact solution (u,p) and compute the
//               corresponding r.h.s. (f,g).  We discretize with Raviart-Thomas
//               finite elements (velocity u) and piecewise discontinuous
//               polynomials (pressure p).
//
//               The example demonstrates the use of the BlockMatrix class, as
//               well as the collective saving of several grid functions in a
//               VisIt (visit.llnl.gov) visualization format.
//
//               We recommend viewing examples 1-4 before viewing this example.

#include "mfem.hpp"
#include "mixed_fe_solvers.hpp"
#include <fstream>
#include <iostream>
#include <assert.h>
#include <memory>

using namespace std;
using namespace mfem;

// Define the analytical solution and forcing terms / boundary conditions
void uFun_ex(const Vector & x, Vector & u);
double pFun_ex(const Vector & x);
void fFun(const Vector & x, Vector & f);
double gFun(const Vector & x);
double f_natural(const Vector & x);

int main(int argc, char *argv[])
{
    StopWatch chrono;

    // 1. Initialize MPI.
    int num_procs, myid;
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
    MPI_Comm_rank(MPI_COMM_WORLD, &myid);
    bool verbose = (myid == 0);

    // 2. Parse command-line options.
    int order = 0;
    bool visualization = true;
    bool divfree = false;
    bool GMG = 0;
    int par_ref_levels = 2;
    bool ML_particular = false;

    OptionsParser args(argc, argv);
    args.AddOption(&order, "-o", "--order",
                   "Finite element order (polynomial degree).");
    args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                   "--no-visualization",
                   "Enable or disable GLVis visualization.");
    args.AddOption(&divfree, "-df", "--divfree", "-no-df",
                   "--no-divfree",
                   "whether to use the divergence free solver or not.");
    args.AddOption(&ML_particular, "-ml-part", "--multilevel-particular", "-no-ml-part",
                   "--no-multilevel-particular",
                   "whether to use the divergence free solver or not.");
    args.AddOption(&GMG, "-GMG", "--GeometricMG", "-AMG",
                   "--AlgebraicMG",
                   "whether to use goemetric or algebraic multigrid solver.");
    args.AddOption(&par_ref_levels, "-r", "--ref",
                   "Number of parallel refinement steps.");
    args.Parse();
    if (!args.Good())
    {
        if (verbose)
        {
            args.PrintUsage(cout);
        }
        MPI_Finalize();
        return 1;
    }
    if (verbose)
    {
        args.PrintOptions(cout);
    }

    // 3. Read the (serial) mesh from the given mesh file on all processors.  We
    //    can handle triangular, quadrilateral, tetrahedral, hexahedral, surface
    //    and volume meshes with the same code.
    Mesh *mesh = new Mesh(2, 2, 2, mfem::Element::TETRAHEDRON, true);

    int dim = mesh->Dimension();

    // 4. Refine the serial mesh on all processors to increase the resolution. In
    //    this example we do 'ref_levels' of uniform refinement. We choose
    //    'ref_levels' to be the largest number that gives a final mesh with no
    //    more than 10,000 elements.

    // 5. Define a parallel mesh by a partitioning of the serial mesh. Refine
    //    this mesh further in parallel to increase the resolution. Once the
    //    parallel mesh is defined, the serial mesh can be deleted.
    ParMesh *pmesh = new ParMesh(MPI_COMM_WORLD, *mesh);
    delete mesh;

    Array<int> ess_bdr(pmesh->bdr_attributes.Max());
    ess_bdr = 0;

    // 6. Define a parallel finite element space on the parallel mesh. Here we
    //    use the Raviart-Thomas finite elements of the specified order.
    ND_FECollection hcurl_coll(order+1, dim);
    RT_FECollection hdiv_coll(order, dim);
    L2_FECollection l2_coll(order, dim);
    L2_FECollection l2_coll_0(0, dim);

    ParFiniteElementSpace* N_space;
    ParFiniteElementSpace* E_space;;
    ParFiniteElementSpace* R_space = new ParFiniteElementSpace(pmesh, &hdiv_coll);
    ParFiniteElementSpace* W_space = new ParFiniteElementSpace(pmesh, &l2_coll);

    ParFiniteElementSpace* coarse_R_space;
    ParFiniteElementSpace* coarse_W_space;
    HypreParMatrix* d_td_coarse_R;
    HypreParMatrix* d_td_coarse_W;

    // Constructing multigrid hierarchy while refining the mesh (if GMG is true)
    InterpolationCollector* P_N;
    vector<SparseMatrix> P_W(par_ref_levels);
    vector<SparseMatrix> P_R(par_ref_levels);
    vector<SparseMatrix> elem_Rdofs(par_ref_levels);
    vector<SparseMatrix> elem_Wdofs(par_ref_levels);
    vector<SparseMatrix> agg_elem(par_ref_levels);
    Array<int> coarsest_ess_dofs;

    chrono.Clear();
    chrono.Start();

    if (divfree && ML_particular)
    {
        coarse_R_space = new ParFiniteElementSpace(pmesh, &hdiv_coll);
        coarse_W_space = new ParFiniteElementSpace(pmesh, &l2_coll);
        E_space = new ParFiniteElementSpace(pmesh, &l2_coll_0);

        d_td_coarse_R = coarse_R_space->Dof_TrueDof_Matrix();
        d_td_coarse_W = coarse_W_space->Dof_TrueDof_Matrix();
        coarse_R_space->GetEssentialVDofs(ess_bdr, coarsest_ess_dofs);

        E_space->SetUpdateOperatorType(Operator::MFEM_SPARSEMAT);
        R_space->SetUpdateOperatorType(Operator::MFEM_SPARSEMAT);
        W_space->SetUpdateOperatorType(Operator::MFEM_SPARSEMAT);
    }

    unique_ptr<ParFiniteElementSpace> coarse_N_space;
    if (divfree)
    {
        N_space = new ParFiniteElementSpace(pmesh, &hcurl_coll);
        P_N = new InterpolationCollector(*N_space, par_ref_levels);
    }

    for (int l = 1; l < par_ref_levels+1; l++)
    {
        pmesh->UniformRefinement();

//        if (l == par_ref_levels)
//        {
//            pmesh->ReorientTetMesh();
//        }

        R_space->Update();
        W_space->Update();

        if (divfree && ML_particular)
        {
            P_R[par_ref_levels-l] = (const SparseMatrix&)*R_space->GetUpdateOperator();
            P_W[par_ref_levels-l] = (const SparseMatrix&)*W_space->GetUpdateOperator();
            auto& elem_agg_l = (const SparseMatrix&)*E_space->GetUpdateOperator();
            unique_ptr<SparseMatrix> agg_elem_l(Transpose(elem_agg_l));
            agg_elem[par_ref_levels-l].Swap(*agg_elem_l);

            P_R[par_ref_levels-l].Threshold(1e-16);
            P_W[par_ref_levels-l].Threshold(1e-16);

            elem_Rdofs[par_ref_levels-l] = ElemToDofs(*R_space);
            elem_Wdofs[par_ref_levels-l] = ElemToDofs(*W_space);
        }

        if (divfree)
        {
            N_space->Update();
            if (GMG)
            {
                P_N->Collect();
            }
        }
    }
    if (verbose)
        cout << "Divergence free hierarchy constructed in " << chrono.RealTime() << "\n";

    HYPRE_Int dimR = R_space->GlobalTrueVSize();
    HYPRE_Int dimW = W_space->GlobalTrueVSize();
    HYPRE_Int dimN = divfree ? N_space->GlobalTrueVSize() : 0;

    if (verbose)
    {
        std::cout << "***********************************************************\n";
        std::cout << "dim(R) = " << dimR << "\n";
        std::cout << "dim(W) = " << dimW << "\n";
        std::cout << "dim(R+W) = " << dimR + dimW << "\n";
        if (divfree)
            std::cout << "dim(N) = " << dimN << "\n";
        std::cout << "***********************************************************\n";
    }

    // 7. Define the two BlockStructure of the problem.  block_offsets is used
    //    for Vector based on dof (like ParGridFunction or ParLinearForm),
    //    block_trueOffstes is used for Vector based on trueDof (HypreParVector
    //    for the rhs and solution of the linear system).  The offsets computed
    //    here are local to the processor.
    Array<int> block_offsets(3); // number of variables + 1
    block_offsets[0] = 0;
    block_offsets[1] = R_space->GetVSize();
    block_offsets[2] = W_space->GetVSize();
    block_offsets.PartialSum();

    Array<int> block_trueOffsets(3); // number of variables + 1
    block_trueOffsets[0] = 0;
    block_trueOffsets[1] = R_space->TrueVSize();
    block_trueOffsets[2] = W_space->TrueVSize();
    block_trueOffsets.PartialSum();

    // 8. Define the coefficients, analytical solution, and rhs of the PDE.
    ConstantCoefficient k(1.0);

    VectorFunctionCoefficient fcoeff(dim, fFun);
    FunctionCoefficient fnatcoeff(f_natural);
    FunctionCoefficient gcoeff(gFun);

    VectorFunctionCoefficient ucoeff(dim, uFun_ex);
    FunctionCoefficient pcoeff(pFun_ex);

    // 9. Define the parallel grid function and parallel linear forms, solution
    //    vector and rhs.
    BlockVector x(block_offsets), rhs(block_offsets);
    BlockVector trueX(block_trueOffsets), trueRhs(block_trueOffsets);

    ParLinearForm *fform(new ParLinearForm);
    fform->Update(R_space, rhs.GetBlock(0), 0);
    fform->AddDomainIntegrator(new VectorFEDomainLFIntegrator(fcoeff));
    fform->AddBoundaryIntegrator(new VectorFEBoundaryFluxLFIntegrator(fnatcoeff));
    fform->Assemble();
    fform->ParallelAssemble(trueRhs.GetBlock(0));

    ParLinearForm *gform(new ParLinearForm);
    gform->Update(W_space, rhs.GetBlock(1), 0);
    gform->AddDomainIntegrator(new DomainLFIntegrator(gcoeff));
    gform->Assemble();
    gform->ParallelAssemble(trueRhs.GetBlock(1));

    // 10. Assemble the finite element matrices for the Darcy operator
    //
    //                            D = [ M  B^T ]
    //                                [ B   0  ]
    //     where:
    //
    //     M = \int_\Omega k u_h \cdot v_h d\Omega   u_h, v_h \in R_h
    //     B   = -\int_\Omega \div u_h q_h d\Omega   u_h \in R_h, q_h \in W_h
    ParBilinearForm *mVarf(new ParBilinearForm(R_space));
    ParMixedBilinearForm *bVarf(new ParMixedBilinearForm(R_space, W_space));

    HypreParMatrix *M, *B;

    mVarf->AddDomainIntegrator(new VectorFEMassIntegrator(k));
    mVarf->Assemble();
    mVarf->Finalize();
    M = mVarf->ParallelAssemble();

    bVarf->AddDomainIntegrator(new VectorFEDivergenceIntegrator);
    bVarf->Assemble();
    bVarf->Finalize();
    bVarf->SpMat() *= -1.0;
    B = bVarf->ParallelAssemble();

    HypreParMatrix *BT = B->Transpose();

    ParDiscreteLinearOperator *DiscreteCurl;
    if (divfree)
    {
        DiscreteCurl = new ParDiscreteLinearOperator(N_space, R_space);
        DiscreteCurl->AddDomainInterpolator(new CurlInterpolator);
        DiscreteCurl->Assemble();
        DiscreteCurl->Finalize();
    }

    int maxIter(500);
    double rtol(1.e-9);
    double atol(1.e-12);

    Operator *darcyOp;
    Solver *darcyPr;
    if (divfree)
    {
        chrono.Clear();
        chrono.Start();

        StopWatch chrono_local;
        chrono_local.Clear();
        chrono_local.Start();

        unique_ptr<HypreParMatrix> BBT(ParMult(B, BT));
        HypreBoomerAMG prec_particular(*BBT);
        prec_particular.SetPrintLevel(0);

        CGSolver solver_particular(M->GetComm());
        solver_particular.SetAbsTol(atol);
        solver_particular.SetRelTol(rtol);
        solver_particular.SetMaxIter(maxIter);
        solver_particular.SetOperator(*BBT);
        solver_particular.SetPreconditioner(prec_particular);
        solver_particular.SetPrintLevel(0);

        // Find a particular solution for div sigma = f
        Vector sol_particular(BT->GetNumRows());
        if (ML_particular)
        {
            auto sol_part = div_part(par_ref_levels+1, SparseMatrix(0), bVarf->SpMat(),
                                     *gform, agg_elem, elem_Rdofs, elem_Wdofs, P_R, P_W,
                                     *d_td_coarse_R, *d_td_coarse_W, coarsest_ess_dofs);

            SparseMatrix true_Rdof_restrict;
            R_space->Dof_TrueDof_Matrix()->GetDiag(true_Rdof_restrict);
            true_Rdof_restrict.MultTranspose(sol_part, sol_particular);
        }
        else
        {
            trueX.GetBlock(1) = 0.0;
            solver_particular.Mult(trueRhs.GetBlock(1), trueX.GetBlock(1));
            BT->Mult(trueX.GetBlock(1), sol_particular);
            chrono_local.Stop();

            if (verbose)
            {
                if (solver_particular.GetConverged())
                    cout << "CG converged in " << solver_particular.GetNumIterations()
                         << " iterations with a residual norm of " << solver_particular.GetFinalNorm() << ".\n";
                else
                    cout << "CG did not converge in " << solver_particular.GetNumIterations()
                         << " iterations. Residual norm is " << solver_particular.GetFinalNorm() << ".\n";
            }
        }
        if (verbose) cout << "Particular solution found in " << chrono_local.RealTime() << "s. \n";

        chrono_local.Clear();
        chrono_local.Start();
        unique_ptr<HypreParMatrix> C(DiscreteCurl->ParallelAssemble());
        unique_ptr<HypreParMatrix> MC(ParMult(M, C.get()));
        unique_ptr<HypreParMatrix> CT(C->Transpose());
        darcyOp = ParMult(CT.get(), MC.get());
        if (GMG)
        {
            darcyPr = new Multigrid(((HypreParMatrix&)*darcyOp), P_N->GetP());
        }
        else
        {
            darcyPr = new HypreAMS(((HypreParMatrix&)*darcyOp), N_space);
            ((HypreAMS*)darcyPr)->SetSingularProblem();
        }

        // Compute the right hand side for the divergence free solver problem
        Vector rhs_divfree(MC->GetNumCols());
        M->Mult(-1.0, sol_particular, 1.0, trueRhs.GetBlock(0));
        CT->Mult(trueRhs.GetBlock(0), rhs_divfree);

        // Solve the divergence free solution
        CGSolver solver(M->GetComm());
        solver.SetAbsTol(atol);
        solver.SetRelTol(rtol);
        solver.SetMaxIter(maxIter);
        solver.SetOperator(*darcyOp);
        solver.SetPreconditioner(*darcyPr);
        solver.SetPrintLevel(1);

        Vector sol_potential(darcyOp->Width());
        sol_potential = 0.0;
        solver.Mult(rhs_divfree, sol_potential);

        Vector sol_divfree(C->GetNumRows());
        C->Mult(sol_potential, sol_divfree);

        // Combining the particular solution and the divergence free solution
        trueX.GetBlock(0) = sol_particular;
        trueX.GetBlock(0) += sol_divfree;

        chrono_local.Stop();
        if (verbose)
        {
            if (solver.GetConverged())
                cout << "CG converged in " << solver.GetNumIterations()
                     << " iterations with a residual norm of " << solver.GetFinalNorm() << ".\n";
            else
                cout << "CG did not converge in " << solver.GetNumIterations()
                     << " iterations. Residual norm is " << solver.GetFinalNorm() << ".\n";
            cout << "Divergence free solution found in " << chrono_local.RealTime() << "s. \n";
        }

        // Compute the right hand side for the pressure problem BB^T p = rhs_p
        chrono_local.Clear();
        chrono_local.Start();

        M->Mult(-1.0, sol_divfree, 1.0, trueRhs.GetBlock(0));
        Vector rhs_p(B->GetNumRows());
        B->Mult(trueRhs.GetBlock(0), rhs_p);
        trueX.GetBlock(1) = 0.0;
        solver_particular.Mult(rhs_p, trueX.GetBlock(1));

        chrono_local.Stop();
        if (verbose)
        {
            if (solver_particular.GetConverged())
                cout << "CG converged in " << solver_particular.GetNumIterations()
                     << " iterations with a residual norm of " << solver_particular.GetFinalNorm() << ".\n";
            else
                cout << "CG did not converge in " << solver_particular.GetNumIterations()
                     << " iterations. Residual norm is " << solver_particular.GetFinalNorm() << ".\n";
            cout << "Pressure solution found in " << chrono_local.RealTime() << "s. \n";
        }
        chrono.Stop();
        if (verbose)
            cout << "Divergence free solver overall took " << chrono.RealTime() << "s. \n";
    }
    else
    {
        chrono.Clear();
        chrono.Start();

        darcyOp = new BlockOperator(block_trueOffsets);
        ((BlockOperator*)darcyOp)->SetBlock(0,0, M);
        ((BlockOperator*)darcyOp)->SetBlock(0,1, BT);
        ((BlockOperator*)darcyOp)->SetBlock(1,0, B);

        // 11. Construct the operators for preconditioner
        //
        //                 P = [ diag(M)         0         ]
        //                     [  0       B diag(M)^-1 B^T ]
        //
        //     Here we use Symmetric Gauss-Seidel to approximate the inverse of the
        //     pressure Schur Complement.
        HypreParMatrix *MinvBt = B->Transpose();
        Vector Md(M->GetNumRows());
        M->GetDiag(Md);
        MinvBt->InvScaleRows(Md);
        HypreParMatrix *S = ParMult(B, MinvBt);

        HypreSolver *invM, *invS;
        invM = new HypreDiagScale(*M);
        invS = new HypreBoomerAMG(*S);
        static_cast<HypreBoomerAMG*>(invS)->SetPrintLevel(0);

        invM->iterative_mode = false;
        invS->iterative_mode = false;

        darcyPr = new BlockDiagonalPreconditioner(
                    block_trueOffsets);
        ((BlockDiagonalPreconditioner*)darcyPr)->SetDiagonalBlock(0, invM);
        ((BlockDiagonalPreconditioner*)darcyPr)->SetDiagonalBlock(1, invS);

        // 12. Solve the linear system with MINRES.
        //     Check the norm of the unpreconditioned residual.

        MINRESSolver solver(MPI_COMM_WORLD);
        solver.SetAbsTol(atol);
        solver.SetRelTol(rtol);
        solver.SetMaxIter(maxIter);
        solver.SetOperator(*darcyOp);
        solver.SetPreconditioner(*darcyPr);
        solver.SetPrintLevel(0);
        trueX = 0.0;
        solver.Mult(trueRhs, trueX);
        chrono.Stop();

        if (verbose)
        {
            if (solver.GetConverged())
                std::cout << "MINRES converged in " << solver.GetNumIterations()
                          << " iterations with a residual norm of " << solver.GetFinalNorm() << ".\n";
            else
                std::cout << "MINRES did not converge in " << solver.GetNumIterations()
                          << " iterations. Residual norm is " << solver.GetFinalNorm() << ".\n";
            std::cout << "MINRES solver took " << chrono.RealTime() << "s. \n";
        }
        delete invM;
        delete invS;
        delete S;
        delete MinvBt;
    }

    // 13. Extract the parallel grid function corresponding to the finite element
    //     approximation X. This is the local solution on each processor. Compute
    //     L2 error norms.
    ParGridFunction *u(new ParGridFunction);
    ParGridFunction *p(new ParGridFunction);
    u->MakeRef(R_space, x.GetBlock(0), 0);
    p->MakeRef(W_space, x.GetBlock(1), 0);
    u->Distribute(&(trueX.GetBlock(0)));
    p->Distribute(&(trueX.GetBlock(1)));

    int order_quad = max(2, 2*order+1);
    const IntegrationRule *irs[Geometry::NumGeom];
    for (int i=0; i < Geometry::NumGeom; ++i)
    {
        irs[i] = &(IntRules.Get(i, order_quad));
    }

    double err_u  = u->ComputeL2Error(ucoeff, irs);
    double norm_u = ComputeGlobalLpNorm(2, ucoeff, *pmesh, irs);
    double err_p  = p->ComputeL2Error(pcoeff, irs);
    double norm_p = ComputeGlobalLpNorm(2, pcoeff, *pmesh, irs);

    if (verbose)
    {
        std::cout << "|| u_h - u_ex || / || u_ex || = " << err_u / norm_u << "\n";
        std::cout << "|| p_h - p_ex || / || p_ex || = " << err_p / norm_p << "\n";
    }

    // 14. Save the refined mesh and the solution in parallel. This output can be
    //     viewed later using GLVis: "glvis -np <np> -m mesh -g sol_*".
    {
        ostringstream mesh_name, u_name, p_name;
        mesh_name << "mesh." << setfill('0') << setw(6) << myid;
        u_name << "sol_u." << setfill('0') << setw(6) << myid;
        p_name << "sol_p." << setfill('0') << setw(6) << myid;

        ofstream mesh_ofs(mesh_name.str().c_str());
        mesh_ofs.precision(8);
        pmesh->Print(mesh_ofs);

        ofstream u_ofs(u_name.str().c_str());
        u_ofs.precision(8);
        u->Save(u_ofs);

        ofstream p_ofs(p_name.str().c_str());
        p_ofs.precision(8);
        p->Save(p_ofs);
    }

    // 15. Save data in the VisIt format
    VisItDataCollection visit_dc("Example5-Parallel", pmesh);
    visit_dc.RegisterField("velocity", u);
    visit_dc.RegisterField("pressure", p);
    visit_dc.Save();

    // 16. Send the solution by socket to a GLVis server.
    if (visualization)
    {
        char vishost[] = "localhost";
        int  visport   = 19916;
        socketstream u_sock(vishost, visport);
        u_sock << "parallel " << num_procs << " " << myid << "\n";
        u_sock.precision(8);
        u_sock << "solution\n" << *pmesh << *u << "window_title 'Velocity'"
               << endl;
        // Make sure all ranks have sent their 'u' solution before initiating
        // another set of GLVis connections (one from each rank):
        MPI_Barrier(pmesh->GetComm());
        socketstream p_sock(vishost, visport);
        p_sock << "parallel " << num_procs << " " << myid << "\n";
        p_sock.precision(8);
        p_sock << "solution\n" << *pmesh << *p << "window_title 'Pressure'"
               << endl;
    }

    // 17. Free the used memory.
    delete fform;
    delete gform;
    delete u;
    delete p;
    delete darcyOp;
    delete darcyPr;
    delete BT;
    delete B;
    delete M;
    delete mVarf;
    delete bVarf;
    delete W_space;
    delete R_space;
    if (divfree)
    {
        delete DiscreteCurl;
        delete N_space;
        delete P_N;
        if (ML_particular)
        {
            delete coarse_R_space;
            delete coarse_W_space;
        }
    }

    delete pmesh;

    MPI_Finalize();

    return 0;
}


void uFun_ex(const Vector & x, Vector & u)
{
    double xi(x(0));
    double yi(x(1));
    double zi(0.0);
    if (x.Size() == 3)
    {
        zi = x(2);
    }

    u(0) = - exp(xi)*sin(yi)*cos(zi);
    u(1) = - exp(xi)*cos(yi)*cos(zi);

    if (x.Size() == 3)
    {
        u(2) = exp(xi)*sin(yi)*sin(zi);
    }
}

// Change if needed
double pFun_ex(const Vector & x)
{
    double xi(x(0));
    double yi(x(1));
    double zi(0.0);

    if (x.Size() == 3)
    {
        zi = x(2);
    }

    return exp(xi)*sin(yi)*cos(zi);
}

void fFun(const Vector & x, Vector & f)
{
    f = 0.0;
}

double gFun(const Vector & x)
{
    if (x.Size() == 3)
    {
        return -pFun_ex(x);
    }
    else
    {
        return 0;
    }
}

double f_natural(const Vector & x)
{
    return (-pFun_ex(x));
}
