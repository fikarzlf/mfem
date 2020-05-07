#include "monolithic_convex_limiting.hpp"

MCL_Evolution::MCL_Evolution(FiniteElementSpace *fes_,
                             HyperbolicSystem *hyp_,
                             DofInfo &dofs_)
   : FE_Evolution(fes_, hyp_, dofs_)
{
   Mesh *mesh = fes->GetMesh();
   const FiniteElement *el = fes->GetFE(0);
   Geometry::Type gtype = el->GetGeomType();
   const int order = el->GetOrder();

   H1_FECollection fec(order, dim);
   fesH1 = new FiniteElementSpace(mesh, &fec);
   bounds = new TightBounds(fes, fesH1);

   Vector shape(nd), dx_shape(nd);
   DenseMatrix dshape(nd,dim);
   DenseTensor GradOp(nd,nd,dim);
   GradOp = 0.;

   NodalFluxes.SetSize(hyp->NumEq, dim, nd);
   Adjugates.SetSize(dim,dim,ne);
   PrecGradOp.SetSize(nd,dim,nd);
   GradProd.SetSize(nd,dim,nd);
   CTilde.SetSize(nd,dim,nd);
   CFull.SetSize(nd,dim,nd);
   OuterUnitNormals.SetSize(dim, dofs.NumBdrs, ne);
   DistributionMatrix.SetSize(nd,nd);
   DetJ.SetSize(ne);

   AntiDiffEl.SetSize(nd, nd, hyp->NumEq);
   wij.SetSize(nd, nd, hyp->NumEq);
   mat3.SetSize(nd, hyp->NumEq);
   DGFluxTerms.SetSize(nd, hyp->NumEq);
   GalerkinRhs.SetSize(nd, hyp->NumEq);
   ElFlux.SetSize(nd, hyp->NumEq);
   uDot.SetSize(nd, hyp->NumEq);
   DTilde.SetSize(nd, nd);
   wfi.SetSize(dofs.NumFaceDofs, hyp->NumEq);
   BdrFlux.SetSize(dofs.NumFaceDofs, hyp->NumEq);
   AntiDiffBdr.SetSize(hyp->NumEq*nd);
   sif.SetSize(dofs.NumFaceDofs);
   vec1.SetSize(hyp->NumEq);

   if (gtype == Geometry::TRIANGLE)
   {
      Dof2LocNbr.SetSize(nd, order < 3 ? 2*order : 6);
      MassMatLumpedRef = 0.5 / (double (nd));
   }
   else
   {
      Dof2LocNbr.SetSize(nd, order < 2 ? dim : 2*dim);
      MassMatLumpedRef =  1.0 / (double (nd));
   }

   Dof2LocNbr = -1;

   uFace.SetSize(hyp->NumEq, dofs.NumFaceDofs);
   uNbrFace.SetSize(hyp->NumEq, dofs.NumFaceDofs);

   Array <int> bdrs, orientation;

   for (int k = 0; k < nqe; k++)
   {
      const IntegrationPoint &ip = IntRuleElem->IntPoint(k);
      el->CalcShape(ip, shape);
      el->CalcDShape(ip, dshape);

      for (int l = 0; l < dim; l++)
      {
         dshape.GetColumn(l, dx_shape);
         AddMult_a_VWt(ip.weight, shape, dx_shape, GradOp(l));
      }
   }

   for (int l = 0; l < dim; l++)
   {
      for (int i = 0; i < nd; i++)
      {
         for (int j = 0; j < nd; j++)
         {
            GradProd(i,l,j) = GradOp(i,j,l) + GradOp(j,i,l);
         }
      }
   }

   ComputePrecGradOp();

   FaceMat.SetSize(dofs.NumFaceDofs, dofs.NumFaceDofs);
   FaceMat = 0.;

   for (int k = 0; k < nqf; k++)
   {
      for (int i = 0; i < dofs.NumFaceDofs; i++)
      {
         for (int j = 0; j < dofs.NumFaceDofs; j++)
         {
            FaceMat(i,j) += IntRuleFaceWeights(k)
                            * ShapeEvalFace(0,i,k) * ShapeEvalFace(0,j,k);
         }
      }
   }

   for (int e = 0; e < ne; e++)
   {
      ElementTransformation *eltrans = fes->GetElementTransformation(e);
      const IntegrationPoint &ip = IntRuleElem->IntPoint(0);
      eltrans->SetIntPoint(&ip);
      DenseMatrix mat_aux(dim,dim);
      CalcAdjugate(eltrans->Jacobian(), mat_aux);
      mat_aux.Transpose();
      Adjugates(e) = mat_aux;
      DetJ(e) = eltrans->Weight();

      if (dim==1)      { mesh->GetElementVertices(e, bdrs); }
      else if (dim==2) { mesh->GetElementEdges(e, bdrs, orientation); }
      else if (dim==3) { mesh->GetElementFaces(e, bdrs, orientation); }

      for (int i = 0; i < dofs.NumBdrs; i++)
      {
         Vector nor(dim);
         FaceElementTransformations *facetrans
            = mesh->GetFaceElementTransformations(bdrs[i]);

         const IntegrationPoint &ip = IntRuleFace->IntPoint(0);
         facetrans->Face->SetIntPoint(&ip);

         if (dim == 1)
         {
            IntegrationPoint aux;
            facetrans->Loc1.Transform(ip, aux);
            nor(0) = 2.*aux.x - 1.0;
         }
         else
         {
            CalcOrtho(facetrans->Face->Jacobian(), nor);
         }

         if (facetrans->Elem1No != e)
         {
            nor *= -1.;
         }

         nor /= nor.Norml2();
         for (int l = 0; l < dim; l++)
         {
            OuterUnitNormals(l,i,e) = nor(l);
         }
      }
   }

   // Construct the P1 subcell mass matrix of the reference element.
   MassMatLOR.SetSize(nd,nd);
   MassMatLOR = 0.;
   DenseMatrix RefMat(dofs.numDofsSubcell, dofs.numDofsSubcell);

   ComputeLORMassMatrix(RefMat, gtype, false);

   // TODO should be unnecessary, why isn't it (for very high orders)?
   RefMat *= 1. / ((double) dofs.numSubcells);

   for (int m = 0; m < dofs.numSubcells; m++)
   {
      for (int i = 0; i < dofs.numDofsSubcell; i++)
      {
         int I = dofs.Sub2Ind(m,i);
         for (int j = 0; j < dofs.numDofsSubcell; j++)
         {
            int J = dofs.Sub2Ind(m,j);
            MassMatLOR(I,J) += RefMat(i,j);
         }
      }
   }

   Vector MassMatLORLumped(nd);
   DenseMatrix LowOrderRefinedMat(MassMatLOR);
   LowOrderRefinedMat *= -1;
   LowOrderRefinedMat.GetRowSums(MassMatLORLumped);

   for (int i = 0; i < nd; i++)
   {
      LowOrderRefinedMat(i,i) -= MassMatLORLumped(i);
      LowOrderRefinedMat(0,i) = 1.;
   }

   DenseMatrixInverse InvLOR(&LowOrderRefinedMat);
   InvLOR.Factor();
   InvLOR.GetInverseMatrix(DistributionMatrix);
}

void MCL_Evolution::Mult(const Vector &x, Vector &y) const
{
   if (hyp->TimeDepBC)
   {
      hyp->BdrCond.SetTime(t);
      if (!hyp->ProjType)
      {
         hyp->L2_Projection(hyp->BdrCond, inflow);
      }
      else
      {
         inflow.ProjectCoefficient(hyp->BdrCond);
      }
   }

   z = 0.;
   ComputeTimeDerivative(x, y);
}

void MCL_Evolution::GetNodeVal(const Vector &uElem, Vector &uEval, int j) const
{
   for (int n = 0; n < hyp->NumEq; n++)
   {
      uEval(n) = uElem(n*nd+j);
   }
}

void MCL_Evolution::ComputeTimeDerivative(const Vector &x, Vector &y,
                                          const Vector &xMPI) const
{
   bounds->ComputeBounds(x);

   for (int e = 0; e < ne; e++)
   {
      fes->GetElementVDofs(e, vdofs);
      x.GetSubVector(vdofs, uElem);
      mat2 = mat3 = ElFlux = DGFluxTerms = 0.;
      AntiDiffEl = 0.;
      AntiDiffBdr = 0.;

      for (int k = 0; k < nqe; k++)
      {
         ElemEval(uElem, uEval, k);
         hyp->EvaluateFlux(uEval, Flux, e, k);
         MultABt(ElemInt(e * nqe + k), Flux, mat1);
         AddMult(DShapeEval(k), mat1, mat2);
      }

      for (int i = 0; i < dofs.NumBdrs; i++)
      {
         OuterUnitNormals(e).GetColumn(i, normal);
         BdrFlux = 0.;

         for (int k = 0; k < nqf; k++)
         {
            FaceEval(x, uEval, uNbrEval, xMPI, normal, e, i, k);

            LaxFriedrichs(uEval, uNbrEval, normal, NumFlux, e, k, i);
            NumFlux *= BdrInt(i,k,e) * IntRuleFaceWeights(k);

            for (int n = 0; n < hyp->NumEq; n++)
            {
               for (int j = 0; j < dofs.NumFaceDofs; j++)
               {
                  double tmp = ShapeEvalFace(i,j,k) * NumFlux(n);
                  DGFluxTerms(dofs.BdrDofs(j,i), n) += tmp;
                  BdrFlux(j,n) -= tmp;
               }
            }
         }

         for (int j = 0; j < dofs.NumFaceDofs; j++)
         {
            nbr = dofs.NbrDofs(i,j,e);
            for (int n = 0; n < hyp->NumEq; n++)
            {
               DofInd = n * ne * nd + e * nd + dofs.BdrDofs(j, i);

               if (nbr < 0)
               {
                  uNbr = inflow(DofInd);
               }
               else
               {
                  // nbr in different MPI task?
                  uNbr = (nbr < xSizeMPI) ? x(n * ne * nd + nbr) : xMPI(int((nbr - xSizeMPI) / nd) * nd * hyp->NumEq + n * nd + (nbr - xSizeMPI) % nd);
               }

               uEval(n) = x(DofInd);
               uNbrEval(n) = uNbr;
            }

            if (nbr < 0)
            {
               hyp->SetBdrCond(uEval, uNbrEval, normal, nbr);
            }

            uFace.SetCol(j, uEval);
            uNbrFace.SetCol(j, uNbrEval);
         }

         for (int j = 0; j < dofs.NumFaceDofs; j++)
         {
            double FaceMatLumped = 0.;

            uFace.GetColumn(j, uEval);
            hyp->EvaluateFlux(uEval, Flux, e, dofs.BdrDofs(j, i)); // TODO default argument k wrong

            for (int l = 0; l < dofs.NumFaceDofs; l++)
            {
               FaceMatLumped += FaceMat(j,l);

               if (j == l) { continue; }

               uFace.GetColumn(l, uNbrEval);
               hyp->EvaluateFlux(uNbrEval, FluxNbr, e, dofs.BdrDofs(j, i)); // TODO default argument k wrong
               FluxNbr -= Flux;

               FluxNbr.Mult(normal, vec1);
               vec1 *= BdrInt(i,0,e) * FaceMat(j,l); // TODO BdrInt

               for (int n = 0; n < hyp->NumEq; n++)
               {
                  ElFlux(dofs.BdrDofs(j,i), n) += vec1(n);
               }
            }

            uNbrFace.GetColumn(j, uNbrEval);

            double ws = max( hyp->GetWaveSpeed(uEval, normal, e, 0, i),
                             hyp->GetWaveSpeed(uNbrEval, normal, e, 0, i) );

            FaceMatLumped *= BdrInt(i,0,e);
            sif(j) = ws * FaceMatLumped;

            subtract(sif(j), uNbrEval, uEval, NumFlux);

            hyp->EvaluateFlux(uEval, Flux, e, dofs.BdrDofs(j, i));
            hyp->EvaluateFlux(uNbrEval, FluxNbr, e, dofs.BdrDofs(j, i));

            for (int n = 0; n < hyp->NumEq; n++)
            {
               wfi(j,n) = 0.5 * sif(j) * (uEval(n) + uNbrEval(n));
               for (int l = 0; l < dim; l++)
               {
                  wfi(j,n) -= 0.5 * (FluxNbr(n,l) - Flux(n,l)) * normal(l) * FaceMatLumped;
               }
            }

            Flux.Mult(normal, vec1);
            vec1 *= FaceMatLumped;

            Flux -= FluxNbr;
            Flux.AddMult_a(FaceMatLumped, normal, NumFlux);

            NumFlux *= 0.5;

            for (int n = 0; n < hyp->NumEq; n++)
            {
               z(vdofs[n*nd + dofs.BdrDofs(j,i)]) += NumFlux(n);
               BdrFlux(j,n) += vec1(n) - NumFlux(n);
            }
         }

         for (int j = 0; j < dofs.NumFaceDofs; j++)
         {
            for (int n = 0; n < hyp->NumEq; n++)
            {
               double gif = BdrFlux(j,n);

               int J = dofs.BdrDofs(j,i);
               if (gif > 0.)
               {
                  gif = min( gif, min( sif(j) * bounds->xi_max(n*ne*nd + e*nd+J) - wfi(j,n),
                                       wfi(j,n) - sif(j) * bounds->xi_min(n*ne*nd + e*nd+J) ) );
               }
               else
               {
                  gif = max( gif, max( sif(j) * bounds->xi_min(n*ne*nd + e*nd+J) - wfi(j,n),
                                       wfi(j,n) - sif(j) * bounds->xi_max(n*ne*nd + e*nd+J) ) );
               }

               AntiDiffBdr(n*nd+dofs.BdrDofs(j,i)) += gif;
            }
         }
      }

      z.AddElementVector(vdofs, AntiDiffBdr);

      Add(mat2, DGFluxTerms, -1., GalerkinRhs);
      mfem::Mult(MassMatRefInv, GalerkinRhs, uDot);

      mat2 = 0.;

      // Dense and sparsified group FE terms, and some antidiffusive fluxes.
      for (int j = 0; j < nd; j++)
      {
         GetNodeVal(uElem, uEval, j);
         hyp->EvaluateFlux(uEval, Flux, e, j);
         NodalFluxes(j) = Flux;
         MultABt(PrecGradOp(j), Adjugates(e), CTilde(j));
         AddMult_a_ABt(-1., CTilde(j), Flux, mat2);

         MultABt(GradProd(j), Adjugates(e), CFull(j));
         AddMultABt(CFull(j), Flux, mat3);

         /* M_C uDot = M_C (M_C)^{-1} GalerkinRhs
                     = int_K \nabla \phi f(u_h) dx - \int_S \phi LF(u_h^-, u_h^+; n) ds,
            ElFlux + = int_K \nabla \phi f(u_h) dx + (M_L - M_C) uDot
                     = M_L uDot + \int_S \phi LF(u_h^-, u_h^+; n) ds. */
         for (int n = 0; n < hyp->NumEq; n++)
         {
            ElFlux(j,n) += LumpedMassMat(n*ne*nd + e*nd + j) * uDot(j,n) / DetJ(e) + DGFluxTerms(j,n);
         }
      }

      // Sparsified Rusanov artificial diffusion for volume terms.
      for (int I = 0; I < nd; I++)
      {
         GetNodeVal(uElem, uEval, I);

         for (int j = 0; j < Dof2LocNbr.Width(); j++)
         {
            int J = Dof2LocNbr(I,j);
            if (J == -1) { break; }

            GetNodeVal(uElem, uNbrEval, J);

            double CTildeNorm1 = 0.;
            double CTildeNorm2 = 0.;

            for (int l = 0; l < dim; l++)
            {
               CTildeNorm1 += CTilde(I,l,J) * CTilde(I,l,J);
               CTildeNorm2 += CTilde(J,l,I) * CTilde(J,l,I);
            }

            CTildeNorm1 = sqrt(CTildeNorm1);
            CTildeNorm2 = sqrt(CTildeNorm2);

            // double dij = max( 0., max( -CTilde(I,0,J), -CTilde(J,0,I) ) );

            CTilde(J).GetRow(I, normal);
            normal /= CTildeNorm1;

            double ws1 = max(hyp->GetWaveSpeed(uEval, normal, e, I),
                             hyp->GetWaveSpeed(uNbrEval, normal, e, J));

            CTilde(I).GetRow(J, normal);
            normal /= CTildeNorm2;

            double ws2 = max(hyp->GetWaveSpeed(uEval, normal, e, I),
                             hyp->GetWaveSpeed(uNbrEval, normal, e, J));
            double dij = max(CTildeNorm1 * ws1, CTildeNorm2 * ws2);
            DTilde(I,J) = dij;

            for (int n = 0; n < hyp->NumEq; n++)
            {
               double rus = dij * (x(vdofs[n * nd + J]) - x(vdofs[n * nd + I]));
               z(vdofs[n * nd + I]) += rus;
               AntiDiffEl(I,J,n) -= rus;
               wij(I,J,n) = dij * (x(vdofs[n*nd + J]) + x(vdofs[n*nd + I]));

               for (int l = 0; l < dim; l++)
               {
                  wij(I,J,n) -= CTilde(I,l,J) * (NodalFluxes(n,l,J) - NodalFluxes(n,l,I));
               }
            }
         }
      }

      ElFlux -= mat2; // ElFlux_i += sum_j f_j cTilde_ij
      ElFlux -= mat3; // ElFlux_i -= sum_j f_j (c_ij + c_ji)
      z.AddElementVector(vdofs, mat2.GetData());

      mfem::Mult(DistributionMatrix, ElFlux, mat2);

      ElFlux = 0.;
      for (int I = 0; I < nd; I++)
      {
         for (int j = 0; j < Dof2LocNbr.Width(); j++)
         {
            int J = Dof2LocNbr(I,j);
            if (J == -1) { break; }

            for (int n = 0; n < hyp->NumEq; n++)
            {
               double fij = AntiDiffEl(I,J,n) + MassMatLOR(I,J) * (mat2(I,n) - mat2(J,n));

               if (fij > 0.)
               {
                  fij = min( fij, min( 2.*DTilde(I,J) * bounds->xi_max(n*ne*nd + e*nd+I) - wij(I,J,n),
                                       wij(J,I,n) - 2.*DTilde(I,J) * bounds->xi_min(n*ne*nd + e*nd+J) ) );
               }
               else
               {
                  fij = max( fij, max( 2.*DTilde(I,J) * bounds->xi_min(n*ne*nd + e*nd+I) - wij(I,J,n),
                                       wij(J,I,n) - 2.*DTilde(I,J) * bounds->xi_max(n*ne*nd + e*nd+J) ) );
               }

               ElFlux(I,n) += fij;
            }
         }
      }

      z.AddElementVector(vdofs, ElFlux.GetData());

      for (int n = 0; n < hyp->NumEq; n++)
      {
         for (int j = 0; j < nd; j++)
         {
            DofInd = n*ne*nd + e*nd + j;
            y(DofInd) = z(DofInd) / (MassMatLumpedRef * DetJ(e));
         }
      }
   }
}

void MCL_Evolution::ComputePrecGradOp()// TODO maybe SparseMatrix - check multiplication first
{
   const FiniteElement *el = fes->GetFE(0);
   int p = el->GetOrder();
   Geometry::Type gtype = el->GetGeomType();

   MassMatRefInv.SetSize(nd,nd);

   if (gtype == Geometry::TRIANGLE)
   {
      IntegrationRule ir = IntRules.Get(gtype, 2*p);
      Vector shape(nd);
      DenseMatrix MassMat(nd,nd);

      for (int k = 0; k < ir.GetNPoints(); k++)
      {
         IntegrationPoint ip = ir.IntPoint(k);
         el->CalcShape(ip, shape);
         AddMult_a_VVt(ip.weight, shape, MassMat);
      }

      DenseMatrixInverse inv(&MassMat);
      inv.Factor();
      inv.GetInverseMatrix(MassMatRefInv);

      Vector a(3), b(3);
      Array<int> aux(2);
      aux[0] = 0;
      PrecGradOp = 0.;

      for (int l = 0; l < dim; l++)
      {
         aux[1] = l+1;

         for (int j = 0; j < nd; j++)
         {
            dofs.Loc2Multiindex.GetRow(j, a);

            for (int m = 0; m < 2; m++)
            {
               for (int k = 0; k < dim+1; k++)
               {
                  b = a;
                  b(aux[m])--;
                  b(k)++;

                  if (b(aux[m]) < 0 || b(k) > p) { continue; }

                  int i = dofs.GetLocFromMultiindex(p, b);
                  PrecGradOp(i,l,j) += pow(-1., m+1) * (a(k) - (k==aux[m]) + 1) / (2.*nd);
               }
            }
         }
      }

      for (int i = 0; i < nd; i++)
      {
         int ctr = 0;
         for (int j = 0; j < nd; j++)
         {
            if (i==j) { continue; }

            for (int l = 0; l < dim; l++)
            {
               if (PrecGradOp(i,l,j) != 0.)
               {
                  Dof2LocNbr(i,ctr) = j;
                  ctr++;
                  break;
               }
            }
         }
      }
   }
   else
   {
      L2Pos_SegmentElement el1D(p);
      IntegrationRule ir = IntRules.Get(Geometry::SEGMENT, 2*p);
      Vector shape(p+1);
      DenseMatrix MassMat1D(p+1,p+1), InvMassMat1D(p+1,p+1),
                  PrecGradOp1D(p+1,p+1);
      DenseTensor PrecGradOpAux(nd,nd,dim);

      MassMat1D = PrecGradOp1D = 0.;

      PrecGradOp1D(0,0) = p;
      for (int j = 1; j < p+1; j++)
      {
         PrecGradOp1D(j,j) = p-2*j;
         PrecGradOp1D(j-1,j) = j-1-p;
         PrecGradOp1D(j,j-1) = j;
      }
      PrecGradOp1D *= -1./pow(p+1, dim);

      for (int k = 0; k < ir.GetNPoints(); k++)
      {
         IntegrationPoint ip = ir.IntPoint(k);
         el1D.CalcShape(ip, shape);
         AddMult_a_VVt(ip.weight, shape, MassMat1D);
      }

      DenseMatrixInverse inv(&MassMat1D);
      inv.Factor();
      inv.GetInverseMatrix(InvMassMat1D);

      switch (dim)
      {
         case 1:
         {
            MassMatRefInv = InvMassMat1D;
            PrecGradOpAux(0) = PrecGradOp1D;
            break;
         }
         case 2:
         {
            DenseMatrix eye(p+1,p+1);
            eye = 0.;
            for (int i = 0; i < p+1; i++) { eye(i,i) = 1.; }

            MassMatRefInv = *OuterProduct(InvMassMat1D, InvMassMat1D);
            PrecGradOpAux(0) = *OuterProduct(eye, PrecGradOp1D);
            PrecGradOpAux(1) = *OuterProduct(PrecGradOp1D, eye);
            break;
         }
         case 3:
         {
            DenseMatrix eye(p+1,p+1);
            eye = 0.;
            for (int i = 0; i < p+1; i++) { eye(i,i) = 1.; }

            MassMatRefInv = *OuterProduct( *OuterProduct(InvMassMat1D, InvMassMat1D), InvMassMat1D );
            PrecGradOpAux(0) = *OuterProduct( eye, *OuterProduct(eye, PrecGradOp1D) );
            PrecGradOpAux(1) = *OuterProduct( eye, *OuterProduct(PrecGradOp1D, eye) );
            PrecGradOpAux(2) = *OuterProduct( PrecGradOp1D, *OuterProduct(eye, eye) );
            break;
         }
      }

      // Reshape and fill Dof2LocNbr.
      for (int i = 0; i < nd; i++)
      {
         int ctr = 0;
         for (int j = 0; j < nd; j++)
         {
            for (int l = 0; l < dim; l++)
            {
               PrecGradOp(i,l,j) = PrecGradOpAux(i,j,l);
            }

            if (i==j) { continue; }

            for (int l = 0; l < dim; l++)
            {
               if (PrecGradOp(i,l,j) != 0.)
               {
                  Dof2LocNbr(i,ctr) = j;
                  ctr++;
                  break;
               }
            }
         }
      }
   }
}

void MCL_Evolution::ComputeLORMassMatrix(DenseMatrix &RefMat, Geometry::Type gtype, bool UseDiagonalNbrs)
{
   switch (gtype)
   {
      case Geometry::SEGMENT:
      {
         RefMat = 1.;
         RefMat(0,0) = RefMat(1,1) = 2.;
         RefMat *= 1. / 6.;
         break;
      }
      case Geometry::TRIANGLE:
      {
         RefMat = 1.;
         RefMat(0,0) = RefMat(1,1) = RefMat(2,2) = 2.;
         RefMat *= 1. / 24.;
         break;
      }
      case Geometry::SQUARE:
      {
         RefMat = 2.;
         if (UseDiagonalNbrs)
         {
            RefMat(0,0) = RefMat(1,1) = RefMat(2,2) = RefMat(3,3) = 4.;
            RefMat(0,3) = RefMat(1,2) = RefMat(2,1) = RefMat(3,0) = 1.;
         }
         else
         {
            RefMat(0,0) = RefMat(1,1) = RefMat(2,2) = RefMat(3,3) = 5.;
            RefMat(0,3) = RefMat(1,2) = RefMat(2,1) = RefMat(3,0) = 0.;
         }
         RefMat *= 1. / 36.;
         break;
      }
      case Geometry::CUBE:
      {
         if (UseDiagonalNbrs)
         {
            RefMat = 2.;
            RefMat(0,0) = RefMat(1,1) = RefMat(2,2) = RefMat(3,3) = 8.;
            RefMat(4,4) = RefMat(5,5) = RefMat(6,6) = RefMat(7,7) = 8.;
            RefMat(0,7) = RefMat(1,6) = RefMat(2,5) = RefMat(3,4) = 1.;
            RefMat(4,3) = RefMat(5,2) = RefMat(6,1) = RefMat(7,0) = 1.;
         }
         else
         {
            RefMat = 0.;
            RefMat(0,0) = RefMat(1,1) = RefMat(2,2) = RefMat(3,3) = 15.;
            RefMat(4,4) = RefMat(5,5) = RefMat(6,6) = RefMat(7,7) = 15.;
         }

         RefMat(0,1) = RefMat(0,2) = RefMat(1,0) = RefMat(1,3) = 4.;
         RefMat(2,0) = RefMat(2,3) = RefMat(3,1) = RefMat(3,2) = 4.;
         RefMat(4,5) = RefMat(4,6) = RefMat(5,4) = RefMat(5,7) = 4.;
         RefMat(6,4) = RefMat(6,7) = RefMat(7,5) = RefMat(7,6) = 4.;
         RefMat(0,4) = RefMat(1,5) = RefMat(2,6) = RefMat(3,7) = 4.;
         RefMat(4,0) = RefMat(5,1) = RefMat(6,2) = RefMat(7,3) = 4.;

         RefMat *= 1. / 216.;
         break;
      }
      default:
         MFEM_ABORT("Unsupported Geometry.");
   }
}