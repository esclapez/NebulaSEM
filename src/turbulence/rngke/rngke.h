#ifndef __RNG_KE_H
#define __RNG_KE_H

#include "ke.h"

struct RNG_KE_Model : public KE_Model {
    /*model coefficients*/
    Scalar eta0;
    Scalar beta;
    /*calculate C2eStar*/
    ScalarCellField C2eStar;

    /*constructor*/
    RNG_KE_Model(VectorCellField&,ScalarFacetField&,ScalarCellField&,Scalar&);

    virtual void enroll();
    virtual void solve();
    virtual void calcEddyViscosity(const TensorCellField& gradU);
};

#endif
