#ifndef __GTA_MOVEMENTTELEMETRYFORMAT_H__
#define __GTA_MOVEMENTTELEMETRYFORMAT_H__

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "PlayerMoveSolver.h"

// The wire format of movement-telemetry.csv (VICE-173, VICE-193). The game's
// writer (MovementTelemetry.cpp) and the harness's replay
// (utils/movement-solver/harness.cpp) share this one file, so a column cannot
// move on one side alone.
//
// One row is one solver step: the snapshot the step returned and the intent
// the step consumed. A state change and the step's events write extra rows
// with the same `step`, so the replay steps the solver once per distinct step
// and compares every row of that step.
//
// The floats are written with %.9g: nine significant digits round-trip a
// 32-bit float exactly, so a replay reads back the same bits. `speed` and
// `target` are units per second; the intent's velocities and speeds are engine
// units (one unit per 1/50 second, MOVE_ENGINE_UNITS_PER_SECOND).
//
// `contacts` is the eMoveContact mask of the snapshot: 1 ground, 2 wall,
// 4 head. `flags` is the intent's own mask (eMoveTelemFlag); the headroom bit
// is set when the body can rise.
enum eMoveTelemFlag
{
	MOVE_TELEM_ON_GROUND = 1,
	MOVE_TELEM_STEEP_CLIFF = 2,
	MOVE_TELEM_HAS_INPUT = 4,
	MOVE_TELEM_JUMP = 8,
	MOVE_TELEM_DIVE = 16,
	MOVE_TELEM_CROUCH = 32,
	MOVE_TELEM_SPRINT = 64,
	MOVE_TELEM_EXIT_VEHICLE = 128,
	MOVE_TELEM_HEADROOM_CLEAR = 256,
	MOVE_TELEM_PRONE = 512,
};

// The header line and the row format, in the column order. Keep the two in
// step: the harness reads the header by name, so a new column needs a header
// entry and a format field.
#define MOVEMENT_TELEMETRY_COLUMNS \
	"ms,step,state,stance,speed,target,tier,fwd,side,spin,contacts,surface,slope," \
	"dt,vel_x,vel_y,surf_x,surf_y,heading,facing,target_in,tier_in,surface_in,flags," \
	"slope_x,slope_y,slope_z,event"

#define MOVEMENT_TELEMETRY_ROW_FORMAT \
	"%u,%u,%s,%s,%.9g,%.9g,%s,%.9g,%.9g,%.9g,%u,%u,%.9g," \
	"%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%s,%u,%u,%.9g,%.9g,%.9g,%s"

// The longest row the format can write, the line buffer of the writer and of
// the harness.
#define MOVEMENT_TELEMETRY_MAX_ROW 512

// The intent's flags mask (eMoveTelemFlag).
static inline uint32_t
MoveTelemetryFlags(const SMoveIntent &intent)
{
	uint32_t flags = 0;

	if(intent.onGround)
		flags |= MOVE_TELEM_ON_GROUND;
	if(intent.steepCliff)
		flags |= MOVE_TELEM_STEEP_CLIFF;
	if(intent.hasInput)
		flags |= MOVE_TELEM_HAS_INPUT;
	if(intent.jumpRequested)
		flags |= MOVE_TELEM_JUMP;
	if(intent.diveRequested)
		flags |= MOVE_TELEM_DIVE;
	if(intent.crouchRequested)
		flags |= MOVE_TELEM_CROUCH;
	if(intent.sprintRequested)
		flags |= MOVE_TELEM_SPRINT;
	if(intent.exitVehiclePressed)
		flags |= MOVE_TELEM_EXIT_VEHICLE;
	if(intent.headroomClear)
		flags |= MOVE_TELEM_HEADROOM_CLEAR;
	if(intent.proneRequested)
		flags |= MOVE_TELEM_PRONE;
	return flags;
}

// One CSV row of one step: the snapshot columns, then the intent columns and
// the event token. The caller passes a buffer of MOVEMENT_TELEMETRY_MAX_ROW
// bytes; the return value is the length snprintf would write.
static inline int
MoveTelemetryFormatRow(char *line, size_t size, uint32_t ms, uint32_t step,
	const SMoveIntent &intent, const SMoveSnapshot &snapshot, const char *event)
{
	return snprintf(line, size, MOVEMENT_TELEMETRY_ROW_FORMAT,
		ms,
		step,
		CPlayerMoveSolver::GetStateName(snapshot.state),
		CPlayerMoveSolver::GetStanceName(snapshot.stance),
		snapshot.speed*MOVE_ENGINE_UNITS_PER_SECOND,
		snapshot.targetSpeed*MOVE_ENGINE_UNITS_PER_SECOND,
		CPlayerMoveSolver::GetTierName(snapshot.tier),
		snapshot.facingDir.x,
		snapshot.facingDir.y,
		snapshot.spin*180.0f/3.14159265358979323846f,
		(uint32_t)snapshot.contacts,
		(uint32_t)snapshot.surface,
		snapshot.slope*180.0f/3.14159265358979323846f,
		intent.dt,
		intent.vel.x,
		intent.vel.y,
		intent.surfaceSpeed.x,
		intent.surfaceSpeed.y,
		intent.moveHeading,
		intent.facing,
		intent.targetSpeed,
		CPlayerMoveSolver::GetTierName(intent.tier),
		(uint32_t)intent.surface,
		(uint32_t)MoveTelemetryFlags(intent),
		intent.slopeNormal.x,
		intent.slopeNormal.y,
		intent.slopeNormal.z,
		event != NULL ? event : "");
}

#endif // __GTA_MOVEMENTTELEMETRYFORMAT_H__
