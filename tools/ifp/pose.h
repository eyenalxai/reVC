#ifndef __TOOLS_IFP_POSE_H__
#define __TOOLS_IFP_POSE_H__

// The pose machinery the two content generators share: one rotation type with
// its product, one bone-name lookup and one place a pose offset meets a key
// (tools/ifp/crouch.cpp, VICE-186 and tools/ifp/prone.cpp, VICE-187). Each
// generator keeps its own parameter tables and its own transform; nothing here
// reads a table.
//
// A rotation is the pose rotation the loader ends up with (see ifp.h), so a
// composition lives in that space and the writer conjugates it again. Two
// rotations compose with the applied-first one second, the way the engine
// composes two frames (RwMatrix::mult). The frame a rotation is expressed in
// decides the side a pose offset composes on, so each call site says which
// frame its offset is in.

#include "ifp.h"

namespace ifp {

// One rotation, the four numbers a key stores (Key::rotation).
struct Quat {
	float x, y, z, w;
};

Quat QuatMake(float x, float y, float z, float w);

// Two rotations as one: b applied first, then a. The tool's only quaternion
// product: a pose offset in the key's parent frame pre-multiplies (the crouch
// pose) and one in the bone's own frame post-multiplies (the prone set's own
// attitudes).
Quat QuatMultiply(const Quat &a, const Quat &b);

// The same rotation with its length removed, the identity when the length is
// zero, so no caller ever divides by it.
Quat QuatNormalise(const Quat &q);

// The rotation the share t of the way from a to b along the shorter arc. A
// sample of a shipped set, so a key of the output is an interpolation of keys
// the game already holds.
Quat QuatSlerp(Quat a, Quat b, float t);

// A stored key's rotation, read and written through one pair, so a key's four
// floats have one reader and one writer.
Quat QuatFromKey(const Key &key);
void SetKeyRotation(Key *key, const Quat &q);

// The pose offset composed into a stored key: the key's own rotation with the
// offset applied after it (q' = offset * q), the joint rotation an offset in the
// key's parent frame is, renormalised because a product drifts. This is the one
// place a pose offset meets a key.
void ComposeKeyOffset(Key *key, const Quat &offset);

// The tool's one bone-name lookup: the index of the sequence a hierarchy holds
// under this bone name, and the index of a bone in a table of bone names, both
// -1 when the name is absent. Every table the generators key by bone name is
// searched through these.
int FindSequence(const Hierarchy &hier, const char *name);
int FindBone(const char *const bones[], int count, const char *name);

} // namespace ifp

#endif // __TOOLS_IFP_POSE_H__
