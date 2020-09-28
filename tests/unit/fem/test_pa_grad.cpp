// Copyright (c) 2010-2020, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

#include "catch.hpp"
#include "mfem.hpp"

using namespace mfem;

double compare_pa_assembly(int dim, int num_elements, int order, bool transpose)
{
   Mesh * mesh;
   if (num_elements == 0)
   {
      if (dim == 2)
      {
         mesh = new Mesh("../../data/star.mesh", order);
      }
      else
      {
         mesh = new Mesh("../../data/beam-hex.mesh", order);
      }
   }
   else
   {
      if (dim == 2)
      {
         mesh = new Mesh(num_elements, num_elements, Element::QUADRILATERAL, true);
      }
      else
      {
         mesh = new Mesh(num_elements, num_elements, num_elements,
                         Element::HEXAHEDRON, true);
      }
   }
   FiniteElementCollection *h1_fec = new H1_FECollection(order, dim);
   FiniteElementCollection *nd_fec = new ND_FECollection(order, dim);
   FiniteElementSpace h1_fespace(mesh, h1_fec);
   FiniteElementSpace nd_fespace(mesh, nd_fec);

   DiscreteLinearOperator assembled_grad(&h1_fespace, &nd_fespace);
   assembled_grad.AddDomainInterpolator(new GradientInterpolator);
   const int skip_zeros = 1;
   assembled_grad.Assemble(skip_zeros);
   assembled_grad.Finalize(skip_zeros);
   const SparseMatrix& assembled_grad_mat = assembled_grad.SpMat();

   DiscreteLinearOperator pa_grad(&h1_fespace, &nd_fespace);
   pa_grad.SetAssemblyLevel(AssemblyLevel::PARTIAL);
   pa_grad.AddDomainInterpolator(new GradientInterpolator);
   pa_grad.Assemble();
   pa_grad.Finalize();

   int insize, outsize;
   if (transpose)
   {
      insize = nd_fespace.GetVSize();
      outsize = h1_fespace.GetVSize();
   }
   else
   {
      insize = h1_fespace.GetVSize();
      outsize = nd_fespace.GetVSize();
   }
   Vector xv(insize);
   Vector assembled_y(outsize);
   Vector pa_y(outsize);

   xv.Randomize();
   if (transpose)
   {
      assembled_grad_mat.BuildTranspose();
      assembled_grad_mat.MultTranspose(xv, assembled_y);
      pa_grad.MultTranspose(xv, pa_y);
   }
   else
   {
      assembled_grad_mat.Mult(xv, assembled_y);
      pa_grad.Mult(xv, pa_y);
   }

   if (false)
   {
      std::cout << "true   \tpa\n";
      for (int i = 0; i < assembled_y.Size(); ++i)
      {
         std::cout << i << " : " << assembled_y(i) << "\t" << pa_y(i) << std::endl;
      }
   }

   pa_y -= assembled_y;
   double error = pa_y.Norml2() / assembled_y.Norml2();
   std::cout << "dim " << dim << " ne " << num_elements << " order "
             << order;
   if (transpose)
   {
      std::cout << " T";
   }
   std::cout << ": error in PA gradient: " << error << std::endl;

   delete h1_fec;
   delete nd_fec;
   delete mesh;

   return error;
}

TEST_CASE("PAGradient", "[CUDA]")
{
   for (bool transpose : {false, true})
   {
      for (int dim = 2; dim < 4; ++dim)
      {
         for (int num_elements = 0; num_elements < 5; ++num_elements)
         {
            for (int order = 1; order < 5; ++order)
            {
               double error = compare_pa_assembly(dim, num_elements, order, transpose);
               REQUIRE(error < 1.e-14);
            }
         }
      }
   }
}

#ifdef MFEM_USE_MPI

double par_compare_pa_assembly(int dim, int num_elements, int order,
                               bool transpose)
{
   int rank;
   MPI_Comm_rank(MPI_COMM_WORLD, &rank);
   int size;
   MPI_Comm_size(MPI_COMM_WORLD, &size);

   Mesh * smesh;
   if (dim == 2)
   {
      smesh = new Mesh(num_elements, num_elements, Element::QUADRILATERAL, true);
   }
   else
   {
      smesh = new Mesh(num_elements, num_elements, num_elements,
                       Element::HEXAHEDRON, true);
   }
   ParMesh * mesh = new ParMesh(MPI_COMM_WORLD, *smesh);
   delete smesh;
   FiniteElementCollection *h1_fec = new H1_FECollection(order, dim);
   FiniteElementCollection *nd_fec = new ND_FECollection(order, dim);
   ParFiniteElementSpace h1_fespace(mesh, h1_fec);
   ParFiniteElementSpace nd_fespace(mesh, nd_fec);

   ParDiscreteLinearOperator assembled_grad(&h1_fespace, &nd_fespace);
   assembled_grad.AddDomainInterpolator(new GradientInterpolator);
   const int skip_zeros = 1;
   assembled_grad.Assemble(skip_zeros);
   assembled_grad.Finalize(skip_zeros);
   HypreParMatrix * assembled_grad_mat = assembled_grad.ParallelAssemble();

   ParDiscreteLinearOperator pa_grad(&h1_fespace, &nd_fespace);
   pa_grad.SetAssemblyLevel(AssemblyLevel::PARTIAL);
   pa_grad.AddDomainInterpolator(new GradientInterpolator);
   pa_grad.Assemble();
   OperatorPtr pa_grad_oper;
   pa_grad.FormRectangularSystemMatrix(pa_grad_oper);

   int insize, outsize;
   if (transpose)
   {
      insize = assembled_grad_mat->Height();
      outsize = assembled_grad_mat->Width();
   }
   else
   {
      insize = assembled_grad_mat->Width();
      outsize = assembled_grad_mat->Height();
   }
   Vector xv(insize);
   Vector assembled_y(outsize);
   Vector pa_y(outsize);
   assembled_y = 0.0;
   pa_y = 0.0;

   xv.Randomize();
   if (transpose)
   {
      assembled_grad_mat->MultTranspose(xv, assembled_y);
      pa_grad_oper->MultTranspose(xv, pa_y);
   }
   else
   {
      assembled_grad_mat->Mult(xv, assembled_y);
      pa_grad_oper->Mult(xv, pa_y);
   }

   Vector error_vec(pa_y);
   error_vec -= assembled_y;
   // serial norms and serial error; we are enforcing equality on each processor
   // in the test
   double error = error_vec.Norml2() / assembled_y.Norml2();

   for (int p = 0; p < size; ++p)
   {
      if (rank == p)
      {
         std::cout << "[" << rank << "]";
         // std::cout << "pa_y.Norml2() = " << pa_y.Norml2() << std::endl;
         // std::cout << "assembled_y.Norml2() = " << assembled_y.Norml2() << std::endl;
         std::cout << "[par] dim " << dim << " ne " << num_elements << " order "
                   << order;
         if (transpose)
         {
            std::cout << " T";
         }
         std::cout << ": error in PA gradient: " << error << std::endl;
         std::cout.flush();
      }
      MPI_Barrier(MPI_COMM_WORLD);
   }

   delete h1_fec;
   delete nd_fec;
   delete assembled_grad_mat;
   delete mesh;

   return error;
}

TEST_CASE("ParallelPAGradient", "[Parallel], [ParallelPAGradient]")
{
   for (bool transpose : {false, true})
   {
      for (int dim = 2; dim < 4; ++dim)
      {
         for (int num_elements = 4; num_elements < 6; ++num_elements)
         {
            for (int order = 1; order < 5; ++order)
            {
               double error = par_compare_pa_assembly(dim, num_elements, order, transpose);
               REQUIRE(error < 1.e-14);
            }
         }
      }
   }
}

#endif
