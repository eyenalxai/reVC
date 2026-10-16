// Minimal stand-ins for the game services the pathfinding code links against.
// The checker never calls them: the modes under test are the path searches,
// which only touch CPathFind's own arrays, CGeneral's random and the traffic
// ring table. Everything else exists so PathFind.cpp links outside the game.

#include "common.h"
#include "PathFind.h"
#include "TrafficRings.h"
#include "General.h"
#include "Camera.h"
#include "World.h"
#include "Lines.h"
#include "Vehicle.h"

// ---------------------------------------------------------------- random

static uint32 g_stubRandomSeed = 1;

// The game's CGeneral::GetRandomNumber is inline in General.h and calls these
// two; the game defines them in re3.cpp. The checker only needs a
// deterministic stream, shared by the base and the changed build.
int
myrand(void)
{
	return (int)(g_stubRandomSeed = g_stubRandomSeed * 1103515245u + 12345u) & 0x7FFF;
}

void
mysrand(unsigned int seed)
{
	g_stubRandomSeed = seed;
}

// ---------------------------------------------------------------- traffic rings

// The ring table itself is header-only; the live values come from the game.
// The checker sets the input directly so a run is reproducible.
STrafficRingInput g_ringInput = { 300.0f, 1.0f, 2000.0f };

STrafficRings
CurrentTrafficRings(void)
{
	return ComputeTrafficRings(g_ringInput);
}

// ---------------------------------------------------------------- placeable / camera

// The camera object is only the carrier of GenerationDistMultiplier for the
// checker; its constructor builds sub-objects the checker never reads, so
// their constructors are the empty stand-ins below.
CMatrix::CMatrix()
{
}

CMatrix::~CMatrix()
{
}

void
CCam::Init(void)
{
}

CCamPathSplines::CCamPathSplines(void)
{
}

CPlaceable::CPlaceable(void)
{
}

CCamera::CCamera(void)
{
}

CCamera TheCamera;

// ---------------------------------------------------------------- world / lines

bool
CWorld::GetIsLineOfSightClear(const CVector &, const CVector &, bool, bool, bool, bool, bool, bool, bool)
{
	return true;
}

float
CWorld::FindGroundZFor3DCoord(float, float, float, bool *)
{
	return 0.0f;
}

void
CLines::RenderLineWithClipping(float, float, float, float, float, float, uint32, uint32)
{
}
