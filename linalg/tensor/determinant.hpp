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

#ifndef MFEM_TENSOR_DET
#define MFEM_TENSOR_DET

#include "../../general/backends.hpp"
#include "tensor.hpp"

namespace mfem
{

// Determinant
template <typename Tensor,
          std::enable_if_t<
             is_dynamic_matrix<Tensor>,
          bool> = true>
MFEM_HOST_DEVICE inline
auto Determinant(const Tensor &J)
{
   if (J.Size<0>()==3 && J.Size<1>()==3)
   {
      return J(0,0)*J(1,1)*J(2,2)-J(0,2)*J(1,1)*J(2,0)
            +J(0,1)*J(1,2)*J(2,0)-J(0,1)*J(1,0)*J(2,2)
            +J(0,2)*J(1,0)*J(2,1)-J(0,0)*J(1,2)*J(2,1);
   }
   else if (J.Size<0>()==2 && J.Size<1>()==2)
   {
      return J(0,0)*J(1,1)-J(0,1)*J(1,0);
   }
   else if (J.Size<0>()==1 && J.Size<1>()==1)
   {
      return J(0,0);
   }
   else
   {
      // TODO abort
      return 0;
   }
}

template <typename Tensor,
          std::enable_if_t<
             is_static_matrix<3,3,Tensor>,
          bool> = true>
MFEM_HOST_DEVICE inline
auto Determinant(const Tensor &J)
{
   return J(0,0)*J(1,1)*J(2,2)-J(0,2)*J(1,1)*J(2,0)
         +J(0,1)*J(1,2)*J(2,0)-J(0,1)*J(1,0)*J(2,2)
         +J(0,2)*J(1,0)*J(2,1)-J(0,0)*J(1,2)*J(2,1);
}

template <typename Tensor,
          std::enable_if_t<
             is_static_matrix<2,2,Tensor>,
          bool> = true>
MFEM_HOST_DEVICE inline
auto Determinant(const Tensor &J)
{
   return J(0,0)*J(1,1)-J(0,1)*J(1,0);
}

template <typename Tensor,
          std::enable_if_t<
             is_static_matrix<1,1,Tensor>,
          bool> = true>
MFEM_HOST_DEVICE inline
auto Determinant(const Tensor &J)
{
   return J(0,0);
}

// Computes determinant for all quadrature points
template<int Q,int Dim> MFEM_HOST_DEVICE inline
dTensor<Q> Determinant(const StaticTensor<dTensor<Dim,Dim>,Q> &J)
{
   dTensor<Q> det;
   MFEM_FOREACH_THREAD(q,x,Q)
   {
      det(q) = Determinant(J(q));
   }
   return det;
}

template<int Q1d> MFEM_HOST_DEVICE inline
dTensor<Q1d,Q1d,Q1d> Determinant(const StaticTensor<dTensor<3,3>,Q1d,Q1d,Q1d> &J)
{
   dTensor<Q1d,Q1d,Q1d> det;
   for (int qz = 0; qz < Q1d; qz++)
   {
      MFEM_FOREACH_THREAD(qy,y,Q1d)
      {
         MFEM_FOREACH_THREAD(qx,x,Q1d)
         {
            det(qx,qy,qz) = Determinant(J(qx,qy,qz));
         }
      }
   }
   return det;
}

template<int Q1d> MFEM_HOST_DEVICE inline
dTensor<Q1d,Q1d> Determinant(const StaticTensor<dTensor<2,2>,Q1d,Q1d> &J)
{
   dTensor<Q1d,Q1d> det;
   MFEM_FOREACH_THREAD(qy,y,Q1d)
   {
      MFEM_FOREACH_THREAD(qx,x,Q1d)
      {
         det(qx,qy) = Determinant(J(qx,qy));
      }
   }
   return det;
}

} // namespace mfem

#endif // MFEM_TENSOR_DET