// The pose arithmetic the two content generators share (tools/ifp/pose.h).
// Every rotation here is the pose rotation the loader ends up with (see ifp.h),
// so a composition lives in that space and the writer conjugates it again.

#include "pose.h"

#include <math.h>
#include <string.h>

namespace ifp {

Quat
QuatMake(float x, float y, float z, float w)
{
	Quat q;
	q.x = x;
	q.y = y;
	q.z = z;
	q.w = w;
	return q;
}

Quat
QuatMultiply(const Quat &a, const Quat &b)
{
	// b applied first, then a, the product the engine composes two frames with
	// (RwMatrix::mult takes the applied-first matrix first).
	return QuatMake(
		a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
		a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
		a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
		a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z);
}

Quat
QuatNormalise(const Quat &q)
{
	float n = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
	if(n == 0.0f)
		return QuatMake(0.0f, 0.0f, 0.0f, 1.0f);
	return QuatMake(q.x/n, q.y/n, q.z/n, q.w/n);
}

static float
QuatDot(const Quat &a, const Quat &b)
{
	return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
}

Quat
QuatSlerp(Quat a, Quat b, float t)
{
	float d = QuatDot(a, b);
	if(d < 0.0f){
		// The antipodal form is the same rotation: take the shorter arc.
		b = QuatMake(-b.x, -b.y, -b.z, -b.w);
		d = -d;
	}
	if(d > 0.9995f){
		// The arc is short enough to interpolate and renormalise.
		return QuatNormalise(QuatMake(a.x + t*(b.x - a.x), a.y + t*(b.y - a.y),
			a.z + t*(b.z - a.z), a.w + t*(b.w - a.w)));
	}
	float th = acosf(d);
	float s = sinf(th);
	float wa = sinf((1.0f - t)*th)/s;
	float wb = sinf(t*th)/s;
	return QuatNormalise(QuatMake(wa*a.x + wb*b.x, wa*a.y + wb*b.y,
		wa*a.z + wb*b.z, wa*a.w + wb*b.w));
}

Quat
QuatFromKey(const Key &key)
{
	return QuatMake(key.rotation[0], key.rotation[1], key.rotation[2],
		key.rotation[3]);
}

void
SetKeyRotation(Key *key, const Quat &q)
{
	key->rotation[0] = q.x;
	key->rotation[1] = q.y;
	key->rotation[2] = q.z;
	key->rotation[3] = q.w;
}

void
ComposeKeyOffset(Key *key, const Quat &offset)
{
	// The offset is an angle of the key's parent frame, the frame the key's
	// quaternion rotates in, so it applies after the key's own rotation.
	SetKeyRotation(key, QuatNormalise(QuatMultiply(offset, QuatFromKey(*key))));
}

int
FindSequence(const Hierarchy &hier, const char *name)
{
	for(size_t i = 0; i < hier.sequences.size(); i++)
		if(hier.sequences[i].name == name)
			return (int)i;
	return -1;
}

int
FindBone(const char *const bones[], int count, const char *name)
{
	for(int i = 0; i < count; i++)
		if(strcmp(bones[i], name) == 0)
			return i;
	return -1;
}

} // namespace ifp
