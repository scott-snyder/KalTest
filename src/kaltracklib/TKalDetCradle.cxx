//*************************************************************************
//* =====================
//*  TKalDetCradle Class
//* =====================
//*
//* (Description)
//*   A singleton class to hold information of detector system
//*   used in Kalman filter classes.
//* (Requires)
//*     TObjArray
//*     TVKalDetector
//* (Provides)
//*     class TKalDetCradle
//* (Update Recored)
//*   2005/02/23  A.Yamaguchi  	Original version.
//*   2005/08/14  K.Fujii        Removed CalcTable(), GetMeasLayerTable(),
//*                              GetPhiTable(), and GetDir() and added
//*                              Transport() to do their functions.
//*   2010/04/06  K.Fujii        Modified Transport() to allow a 1-dim hit,
//*                              for which pivot is at the expected hit.
//*   2012/11/29  K.Fujii        Moved GetEnergyLoss and CalcQms from
//*                              TKalDetCradle.
//*
//*************************************************************************

#include "TKalDetCradle.h"   // from KalTrackLib
#include "TVMeasLayer.h"     // from KalTrackLib
#include "TVKalDetector.h"   // from KalTrackLib
#include "TKalTrackSite.h"   // from KalTrackLib
#include "TKalTrackState.h"  // from KalTrackLib
#include "TKalTrack.h"       // from KalTrackLib
#include "TVSurface.h"       // from GeomLib
#include "TVTrack.h"         // from GeomLib
#include "TBField.h"         // from Bfield
#include "TRungeKuttaTrack.h"

#include <iostream>          // from STL

ClassImp(TKalDetCradle)

Bool_t   TKalDetCradle::fUseRKTrack= kFALSE;

//_________________________________________________________________________
//  ----------------------------------
//   Ctors and Dtor
//  ----------------------------------

TKalDetCradle::TKalDetCradle(Int_t n)
             : TObjArray(n), fIsMSON(kTRUE), fIsDEDXON(kTRUE),
               fDone(kFALSE), fIsClosed(kFALSE)
{
}

TKalDetCradle::~TKalDetCradle()
{
}

//_________________________________________________________________________
//  ----------------------------------
//   Utility Methods
//  ----------------------------------
//_________________________________________________________________________
// -----------------
//  Install
// -----------------
//    installs a sub-detector into this cradle.
//
void TKalDetCradle::Install(TVKalDetector &det)
{
    if (IsClosed()) {
        std::cerr << ">>>> Error!! >>>> TKalDetCradle::Install" << std::endl
        << "      Cradle already closed. Abort!!"     << std::endl;
        abort();
    }
    TIter next(&det);
    TObject *mlp = 0;  // measment layer pointer
    while ((mlp = next())) {
        Add(mlp);
        dynamic_cast<TAttElement &>(*mlp).SetParentPtr(&det);
        det.SetParentPtr(this);
    }
    fDone = kFALSE;
}

void TKalDetCradle::Transport(const TKalTrackSite  &from,   // site from
                                    TKalTrackSite  &to,     // site to
                                    TKalMatrix     &sv,     // state vector
                                    TKalMatrix     &F,      // propagator matrix
                                    TKalMatrix     &Q)      // process noise matrix
{
    // ---------------------------------------------------------------------
    //  Sort measurement layers in this cradle if not
    // ---------------------------------------------------------------------

    if (!fDone) Update();

    // ---------------------------------------------------------------------
    //  Move to site "to"
    // ---------------------------------------------------------------------

    std::unique_ptr<TVTrack> help(&static_cast<TKalTrackState &>
                                (from.GetCurState()).CreateTrack()); // tmp track
    
    const TVMeasLayer& ml_to = to.GetHit().GetMeasLayer() ;
    
    TVector3  x0; // local pivot at the "to" site

	if(fUseRKTrack) {
		this->Transport2(from, ml_to, x0, sv, F, Q, help);
	} 
	else {
		this->Transport(from, ml_to, x0, sv, F, Q, help);
	}

    TVTrack &hel = *help;
    
    // ---------------------------------------------------------------------
    //  Move pivot from last expected hit to actural hit at site "to"
    // ---------------------------------------------------------------------
    
    if (to.GetDimension() > 1) {
        
        double fid = 0.;
        Int_t sdim = sv.GetNrows();              // number of track parameters
	if(to.GetHit().GetBfield()!=0.) {
        TKalMatrix DF(sdim, sdim);               // propagator matrix segment
        
        hel.MoveTo(to.GetGlobalPivot(), fid, &DF, 0, kFALSE);     // move pivot to actual hit (to)

        F = DF * F;                              // update F accordingly
        hel.PutInto(sv);                         // save updated hel to sv
   }
   else {
	   TKalMatrix DF(sdim, sdim);

  	   TStraightTrack strtrk(sv, x0);
  	   strtrk.MoveTo(to.GetPivot(), fid, &DF);     // move pivot to actual hit (to)
  	   F = DF * F;                                 // update F accordingly
  	   strtrk.PutInto(sv);                         // save updated hel to sv
   }
    
  } else {
	    TVector3 x0g = x0;

        if(!TBField::IsUsingUniformBfield()) {
		   	x0g = (hel.GetFrame()).Transform(x0, TTrackFrame::kLocalToGlobal);
        }

        to.SetPivot(x0g);                         // if it is a 1-dim hit
    }

    if(!TBField::IsUsingUniformBfield()) {
        to.SetFrame(hel.GetFrame());     // set frame
        to.SetBfield(hel.GetMagField()); // set B field
    }
}

int TKalDetCradle::Transport(const TKalTrackSite  &from,  // site from
                             const TVMeasLayer    &ml_to, // layer to reach
                             TVector3       &x0,    // pivot for sv
                             TKalMatrix     &sv,    // state vector
                             TKalMatrix     &F,     // propagator matrix
                             TKalMatrix     &Q)     // process noise matrix
{
    if (!fDone) Update();
    
    std::unique_ptr<TVTrack> help(&static_cast<TKalTrackState &>
                                (from.GetCurState()).CreateTrack()); // tmp track
    return Transport(from, ml_to, x0, sv, F, Q, help);
}

//
//
//
//_________________________________________________________________________
// -----------------
//  Transport
// -----------------
//    transports state (sv) from site (from) to layer (ml_to), taking into
//    account multiple scattering and energy loss and updates state (sv),
//    fills pivot in x0, propagator matrix (F), and process noise matrix (Q).
//

int TKalDetCradle::Transport(const TKalTrackSite  &from,  // site from
                             const TVMeasLayer    &ml_to, // layer to reach
                                   TVector3       &x0,    // pivot for sv
                                   TKalMatrix     &sv,    // state vector
                                   TKalMatrix     &F,     // propagator matrix
                                   TKalMatrix     &Q,     // process noise matrix
                           std::unique_ptr<TVTrack> &help)  // pointer to update track object
{
  // ---------------------------------------------------------------------
  //  Sort measurement layers in this cradle if not
  // ---------------------------------------------------------------------
  if (!fDone) Update();
	
  // ---------------------------------------------------------------------
  //  Locate sites from and to in this cradle
  // ---------------------------------------------------------------------
  Int_t  fridx = from.GetHit().GetMeasLayer().GetIndex(); // index of site from
  Int_t  toidx = ml_to.GetIndex();                        // index of layer to
  Int_t  di    = fridx > toidx ? -1 : 1;                  // layer increment
    TVTrack &hel = *help;
    
    //=====================
    // FIXME
    //=====================
    TVector3 xfrom = from.GetGlobalPivot();         // get the referenece point
    TVector3 xto;                                   // reference point at destination to be returned by CalcXingPointWith
    Double_t fito = 0;                              // deflection angle to destination to be returned by CalcXingPointWith
    
    const TVSurface *sfp = dynamic_cast<const TVSurface *>(&ml_to);// surface at destination
    
	double eps = 1.e-8;

	if(!TBField::IsUsingUniformBfield()) {
		eps = 1.e-5;
	}

    sfp->CalcXingPointWith(hel, xto, fito, 0, eps);

    // as mode is 0 here the closest point crossing point is taken
    // this means that if we are at the top of a looping track
    // and the point to which we want to move is on the other side of
    // the loop but has a lower radius the transport will move down
    // through all layers and segfault on reaching index -1
    
    //   if( does_cross < 1 ) return does_cross ;
    
    TMatrixD dxdphi = hel.CalcDxDphi(fito);                       // tangent vector at destination surface
    TVector3 dxdphiv(dxdphi(0,0),dxdphi(1,0),dxdphi(2,0));        // convert matirix diagonal to vector
    //  Double_t cpa = hel.GetKappa();                                // get pt
    
    Bool_t isout = -fito*dxdphiv.Dot(sfp->GetOutwardNormal(xto)) < 0 ? kTRUE : kFALSE;  // out-going or in-coming at the destination surface
    //=====================
    // ENDFIXME
    //=====================
    
    TVector3 xx;                               // expected hit position vector
    Double_t fid     = 0.;                     // deflection angle from the last hit
    
    Int_t sdim = sv.GetNrows();                // number of track parameters
    F.UnitMatrix();                            // set the propagator matrix to the unit matrix
    Q.Zero();                                  // zero the noise matrix
    
    TKalMatrix DF(sdim, sdim);                 // propagator matrix segment
    
    // ---------------------------------------------------------------------
    //  Loop over layers and transport sv, F, and Q step by step
    // ---------------------------------------------------------------------
    Int_t ifr = fridx; // set index to the index of the intitial starting layer
    
    // here we make first make sure that the helix is at the crossing point of the current surface.
    // this is necessary to ensure that the material is only accounted for between fridx and toidx
    // otherwise it is possible to have inconsistencies with material treatment.
    // loop until we reach the index toidx, which is the surface we need to reach
    for (Int_t ito=fridx; (di>0 && ito<=toidx)||(di<0 && ito>=toidx); ito += di) {
        
        Double_t fid_temp = fid; // deflection angle from the last layer crossing
        
        int mode = ito!=fridx ? di : 0; // need to move to the from site as the helix may not be on the crossing point yet, meaning that the eloss and ms will be incorrectely attributed ...
        
        if (static_cast<TVSurface *>(At(ito))->CalcXingPointWith(hel, xx, fid, mode, eps)) { // if we have a crossing point at this surface, note di specifies if we are moving forwards or backwards

            //=====================
            // FIXME
            //=====================
            static const Double_t kMergin = 1.0;
            // if the distance from the current crossing point to the starting point - kMergin(1mm) is greater than the distance from the destination to the starting point
            // this is needed to skip crossing points which come from the far side of the IP, for a cylinder this would not be a problem
            // but for the bounded planes it is perfectly posible due to the sorting in R
            // reset the deflection angle and skip this layer
            // this would at stop layers being added which are too far away but I am not sure how this will work with the problem described above.
            if( (xx-xfrom).Mag() - kMergin > (xto-xfrom).Mag() ){
                fid = fid_temp;
                continue ;
            }
            //=====================
            // ENDFIXME
            //=====================

            const TVMeasLayer   &ml  = *dynamic_cast<TVMeasLayer *>(At(ifr)); // get the last layer
      
            
            TKalMatrix Qms(sdim, sdim);
            if (IsMSOn()&& ito!=fridx ){
                
                ml.CalcQms(isout, hel, fid, Qms); // Qms for this step, using the fact that the material was found to be outgoing or incomming above, and the distance from the last layer
            }
            
            hel.MoveTo(xx, fid, &DF);         // move the helix to the present crossing point, DF will simply have its values overwritten so it could be explicitly set to unity here
            if (sdim == 6) DF(5, 5) = 1.;     // t0 stays the same
            F = DF * F;                       // update F
            TKalMatrix DFt  = TKalMatrix(TMatrixD::kTransposed, DF);
            
            Q = DF * (Q + Qms) * DFt;         // transport Q to the present crossing point
            
            if (IsDEDXOn() && ito!=fridx) {
                hel.PutInto(sv);              // copy hel to sv
                // whether the helix is moving forwards or backwards is calculated using the sign of the charge and the sign of the deflection angle
                // Bool_t isfwd = ((cpa > 0 && df < 0) || (cpa <= 0 && df > 0)) ? kForward : kBackward;  // taken from TVMeasurmentLayer::GetEnergyLoss  not df = fid
                sv(2,0) += ml.GetEnergyLoss(isout, hel, fid); // correct for dE/dx, returns delta kappa i.e. the change in pt
                hel.SetTo(sv, hel.GetPivot());                // save sv back to hel
            }
            ifr = ito; // for the next iteration set the "previous" layer to the current layer moved to

            //fg: need to set the deflection angle to 0. as we moved the helix to a new position
            //    as this fid is used in the next call to TVSurface::CalcXingPointWith(), i.e. to
            //    compute the start position of the newtonian solver (which should be the reference point)
            fid = 0. ; 

        } else {
            fid = fid_temp;
        }
    } // end of loop over surfaces
    
    //   // ---------------------------------------------------------------------
    //   //  Move pivot to crossing point with layer to move to
    //   // ---------------------------------------------------------------------
    //   dynamic_cast<const TVSurface *>(&ml_to)->CalcXingPointWith(hel, xx, fid);
    //   hel.MoveTo(xx, fid, &DF); // move pivot to expected hit, DF will simply have its values overwritten so it could be explicitly set to unity here
    //   F = DF * F;                          // update F accordingly
    
    x0 = hel.GetPivot();                 // local pivot corresponding to sv
    hel.PutInto(sv);                     // save updated hel to sv
    
    return 0;
    
}


int TKalDetCradle::Transport2(const TKalTrackSite  &from,  // site from
                              const TVMeasLayer    &ml_to, // layer to reach
                                    TVector3       &x0,    // pivot for sv
                                    TKalMatrix     &sv,    // state vector
                                    TKalMatrix     &F,     // propagator matrix
                                    TKalMatrix     &Q,     // process noise matrix
                           std::unique_ptr<TVTrack> &help)  // pointer to update track object
{
  // ---------------------------------------------------------------------
  //  Sort measurement layers in this cradle if not
  // ---------------------------------------------------------------------
  if (!fDone) Update();
    
  // ---------------------------------------------------------------------
  //  Locate sites from and to in this cradle
  // ---------------------------------------------------------------------
  Int_t  fridx = from.GetHit().GetMeasLayer().GetIndex(); // index of site from
  Int_t  toidx = ml_to.GetIndex();                        // index of layer to
  Int_t  di    = fridx > toidx ? -1 : 1;                  // layer increment

  // FIXME
  THelicalTrack& hel = dynamic_cast<THelicalTrack&>(*help);

  TVector3 xx;                               // expected hit position vector
  Double_t fid     = 0.;                     // deflection angle from the last hit
  
  Int_t sdim = sv.GetNrows();                // number of track parameters
  F.UnitMatrix();                            // set the propagator matrix to the unit matrix
  Q.Zero();                                  // zero the noise matrix
  
  TKalMatrix DF(sdim, sdim);                 // propagator matrix segment
  
  // ---------------------------------------------------------------------
  //  Loop over layers and transport sv, F, and Q step by step
  // ---------------------------------------------------------------------
  Int_t ifr = fridx; // set index to the index of the intitial starting layer
  
  // here we make first make sure that the helix is at the crossing point of the current surface.
  // this is necessary to ensure that the material is only accounted for between fridx and toidx
  // otherwise it is possible to have inconsistencies with material treatment.
  // loop until we reach the index toidx, which is the surface we need to reach
  
  TRungeKuttaTrack rk;
  TVTrack* trackPtr = 0;
  
  for (Int_t ito=fridx; (di>0 && ito<=toidx)||(di<0 && ito>=toidx); ito += di) {
    
	Bool_t isout = di>0 ? kTRUE: kFALSE;

    // need to move to the from site as the helix may not be on the crossing point yet, 
	// meaning that the eloss and ms will be incorrectely attributed
	// the current TCylinder uses the Newtonian method to calculate crossing point, 
	// in which the mode is always 0.
    int mode = ito!=fridx ? di : 0;  

    const TVMeasLayer   &ml  = *dynamic_cast<TVMeasLayer *>(At(ifr)); // get the last layer 
	
	//The crossing point between a TRungeKuttaTrack and a TVSurface
    TVector3 rkxx;

	//The initial step of Runge-Kutta algorithm
    Double_t step = 0.01;

	if(ito==fridx) { 
		// If ito==fridx, it means the track will move from the current pivot (hit point)
		// to the crossing point at the SAME layer.
		// Because the distance between the two points is very small, so the difference of b field 
		// of two points are also small, and we use the helical track model.

		// helix is defined by local track parameters.
	    // xx is a global coordinate.
		// angle fid is difference of direction for pivot and crossing point in a helix.
		dynamic_cast<TVSurface &>(*At(ito)).CalcXingPointWith(hel, xx, fid, mode);
	}
	else {
	    // If ito!=fridx, it means the track will move from the currnt pivot (crossing point)
		// to the next crossing point of track and next layer.
		// Considering the non-uniformity of the magnetic field, we use the Runge-Kutta track model here, 
		// and we create a runge-kutta track to calculate the crossing point.		

		rk.SetFromTrack(hel);

		dynamic_cast<TVSurface &>(*At(ito)).CalcXingPointWith(rk, rkxx, step, mode);
    }

    TKalMatrix Qms(sdim, sdim);                                       

    if (IsMSOn()&& ito!=fridx ){
        ml.CalcQms(isout, hel, fid, Qms);                  
	   	// Qms for this step, using the fact that the material was found to be outgoing 
		// or incomming above, and the distance from the last layer 
    }
      
	if(ito==fridx) {
    	// Move the helix to the present crossing point.
		// Frame rotation is not needed.
        hel.MoveTo(xx, fid, &DF, 0, kFALSE);
		trackPtr = &hel;
	}
	else { 
		// Move the track to the new layer.
		// A rotation is done in TRungeKuttaTrack::MoveTo.
		rk.MoveTo(rkxx, step, DF);
		trackPtr = &rk;
		
		rk.SetToTrack(hel);
	}

    if (sdim == 6) DF(5, 5) = 1.;     // t0 stays the same

    F = DF * F;                       // update F

    TKalMatrix DFt  = TKalMatrix(TMatrixD::kTransposed, DF);
      
    Q = DF * (Q + Qms) * DFt;         // transport Q to the present crossing point
      
    if (IsDEDXOn() && ito!=fridx) {
        hel.PutInto(sv);                              // copy hel to sv

        // whether the helix is moving forwards or backwards is calculated using 
		// the sign of the charge and the sign of the deflection angle  
                         
		// Bool_t isfwd = ((cpa > 0 && df < 0) || (cpa <= 0 && df > 0)) ? kForward : kBackward;  
		// taken from TVMeasurmentLayer::GetEnergyLoss  not df = fid
        
		sv(2,0) += ml.GetEnergyLoss(isout, hel, fid); 
		// correct for dE/dx, returns delta kappa i.e. the change in pt 
        hel.SetTo(sv, hel.GetPivot());                // save sv back to hel
      }
      
	  ifr = ito; // for the next iteration set the "previous" layer to the current layer moved to 

  } // end of loop over surfaces
  
  //hel or rk
  x0 = trackPtr->GetPivot();
  trackPtr->PutInto(sv);                     // save updated hel to sv
  
  return 0;
}

//_________________________________________________________________________
// -----------------
//  Update
// -----------------
//    sorts meaurement layers according to layer's sorting policy
//    and puts index to layers from inside to outside.
//
void TKalDetCradle::Update()
{
    fDone = kTRUE;
    
    UnSort();   // unsort
    Sort();     // sort layers according to sorting policy
    
    TIter next(this);
    TVMeasLayer *mlp = 0;
    Int_t i = 0;
    
    while ((mlp = dynamic_cast<TVMeasLayer *>(next()))) {
        mlp->SetIndex(i++);
    }
    
}


