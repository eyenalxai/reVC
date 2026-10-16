#ifndef __GTA_DUMMYOBJECT_H__
#define __GTA_DUMMYOBJECT_H__

#include "Dummy.h"

class CObject;

class CDummyObject : public CDummy
{
public:
	// Object state that must survive the dummy round trip, so a smashed or
	// damaged object does not come back to life when the player returns.
	// bConvertedFromObject is false for dummies loaded straight from the map:
	// no object was converted into them and they have no state to give back.
	// bIsVisible rides on the dummy itself, the dummy has to be drawn or hidden
	// like the object was. The flags below are stored here because the dummy has
	// its own values for them that its own code must keep.
	bool bConvertedFromObject;
	bool bObjectUsesCollision;
	bool bObjectRenderDamaged;
	bool bObjectExplosionProof;

	CDummyObject(void) {
		bConvertedFromObject = false;
		bObjectUsesCollision = false;
		bObjectRenderDamaged = false;
		bObjectExplosionProof = false;
	}
	CDummyObject(CObject *obj);
};

#endif // __GTA_DUMMYOBJECT_H__
