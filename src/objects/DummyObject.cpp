#include "common.h"

#include "DummyObject.h"
#include "Object.h"
#include "Pools.h"

CDummyObject::CDummyObject(CObject *obj)
{
	SetModelIndexNoCreate(obj->GetModelIndex());
	if(obj->m_rwObject)
		AttachToRwObject(obj->m_rwObject);
	obj->DetachFromRwObject();
	m_level = obj->m_level;
	m_area = obj->m_area;
	// The object keeps its state while it is only a dummy: a smashed object must
	// not be drawn as a whole one, and it must not come back alive. bHasBeenDamaged
	// has no field here, so it is lost over the round trip.
	bConvertedFromObject = true;
	bIsVisible = obj->bIsVisible;
	bObjectUsesCollision = obj->bUsesCollision;
	bObjectRenderDamaged = obj->bRenderDamaged;
	bObjectExplosionProof = obj->bExplosionProof;
	bObjectFelled = obj->bIsFelled;
}
