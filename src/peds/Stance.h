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

#endif // __GTA_STANCE_H__
