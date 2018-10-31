// Copyright (c) 2017, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-734707. All Rights
// reserved. See files LICENSE and NOTICE for details.
//
// This file is part of CEED, a collection of benchmarks, miniapps, software
// libraries and APIs for efficient high-order finite element and spectral
// element discretizations for exascale applications. For more information and
// source code availability see http://github.com/ceed.
//
// The CEED research is supported by the Exascale Computing Project 17-SC-20-SC,
// a collaborative effort of two U.S. Department of Energy organizations (Office
// of Science and the National Nuclear Security Administration) responsible for
// the planning and preparation of a capable exascale ecosystem, including
// software, applications, hardware, advanced system engineering and early
// testbed platforms, in support of the nation's exascale computing imperative.

#include "../../general/okina.hpp"

namespace mfem
{

// *****************************************************************************
void kGlobalToLocal(const int NUM_VDIM,
                    const bool VDIM_ORDERING,
                    const int globalEntries,
                    const int localEntries,
                    const int* __restrict offsets,
                    const int* __restrict indices,
                    const double* __restrict globalX,
                    double* __restrict localX)
{

   GET_CONST_ADRS_T(offsets,int);
   GET_CONST_ADRS_T(indices,int);
   GET_CONST_ADRS(globalX);
   GET_ADRS(localX);
   MFEM_FORALL(i, globalEntries,
   {
      const int offset = d_offsets[i];
      const int nextOffset = d_offsets[i+1];
      for (int v = 0; v < NUM_VDIM; ++v)
      {
         const int g_offset = ijNMt(v,i,NUM_VDIM,globalEntries,VDIM_ORDERING);
         const double dofValue = d_globalX[g_offset];
         for (int j = offset; j < nextOffset; ++j)
         {
            const int l_offset = ijNMt(v,d_indices[j],NUM_VDIM,localEntries,VDIM_ORDERING);
            d_localX[l_offset] = dofValue;
         }
      }
   });
}

}
