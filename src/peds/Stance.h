#ifndef __GTA_STANCE_H__
#define __GTA_STANCE_H__

// The player's body stance, an axis of its own beside the movement state (see
// the stance entry of GLOSSARY.md). It lives in its own header with no include
// of its own, because both the ped (Ped.h) and the engine free movement solver
// (PlayerMoveSolver.h) name it and the solver must not pull an engine header in.

enum eStance
{
	STANCE_STAND,
	STANCE_CROUCH,
	STANCE_PRONE,
	STANCE_NUM,
};

// A stance low enough that the body is off the standing height. The crouch and
// the prone share the exits, the headroom rule and the camera offsets.
static inline bool
IsLowStance(eStance stance)
{
	return stance == STANCE_CROUCH || stance == STANCE_PRONE;
}

#endif // __GTA_STANCE_H__
