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

static uint32 g_stubRandomState = 0x9e3779b9u;

uint32
CGeneral::GetRandomNumber(void)
{
	g_stubRandomState ^= g_stubRandomState << 13;
	g_stubRandomState ^= g_stubRandomState >> 17;
	g_stubRandomState ^= g_stubRandomState << 5;
	return g_stubRandomState;
}

void
CGeneral::SetRandomSeed(uint32 seed)
{
	g_stubRandomState = seed ? seed : 0x9e3779b9u;
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

// ---------------------------------------------------------------- camera

CCamera TheCamera;

// ---------------------------------------------------------------- world / lines

bool
CWorld::GetIsLineOfSightClear(const CVector &, const CVector &, bool, bool, bool, bool, bool, bool, bool)
{
	return true;
}

bool
CWorld::GetIsLineOfSightClear(const CVector &, const CVector &, bool, bool, bool, bool, bool, bool)
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
