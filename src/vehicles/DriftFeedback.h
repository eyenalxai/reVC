#ifndef __GTA_DRIFTFEEDBACK_H__
#define __GTA_DRIFTFEEDBACK_H__

class CAutomobile;

// The feedback layer that sells the player's arcade drift (see
// docs/adr/0001-arcade-drift-handling.md). It only reads the drift state, it
// changes no physics, and it is inert while gDriftEnabled is false.
//
// Everything is driven by one signed strength in -1..1 that is smoothed from
// the slip angle: the sign is the side the car slides to (+1 = its own right).
//
// All drift feedback tuning lives in this block. Slip angles are degrees,
// rates are 1/second and every value is visual only.
#define DRIFT_FEEDBACK_SLIP_MIN		6.0f	// slip that starts the feedback, degrees
#define DRIFT_FEEDBACK_SLIP_FULL	25.0f	// slip that gives full feedback, degrees
#define DRIFT_FEEDBACK_ATTACK		7.0f	// strength ramp up
#define DRIFT_FEEDBACK_RELEASE		2.8f	// strength fade out, about 0.36s
#define DRIFT_FEEDBACK_MAX_STEP		0.1f	// longest smoothing step, seconds
#define DRIFT_LEAN_ROLL_MAX		5.0f	// visible roll at full slip, degrees
#define DRIFT_LEAN_PITCH_MAX		1.5f	// visible nose-up pitch at full slip, degrees
#define DRIFT_CAM_FOV_MAX		6.0f	// extra chase camera FOV, degrees
#define DRIFT_CAM_LEAD_MAX		6.0f	// camera lead towards the velocity, degrees
#define DRIFT_SKID_WIDTH_MAX		1.6f	// skid mark width multiplier at full slip
#define DRIFT_SKID_SMOKE_PER_SEC	45.0f	// extra tyre smoke per wheel, particles/second
#define DRIFT_SKID_SMOKE_MAX		3	// smoke particle cap per wheel and frame

class CDriftFeedback
{
public:
	// Steps the smoothed strength once a frame. The player's car calls it from
	// its PreRender (see Automobile.cpp), before the readers of the strength;
	// the frame guard keeps a second pre-render of the car in a frame from
	// stepping twice.
	static void Update(CAutomobile *car, float timeStep);

	// Signed strength of the drift, 0 while the feature is off.
	static float GetStrength(void);

	// How many extra smoke particles a wheel should throw this frame.
	static int SmokeCount(float strength);

	// The visual car lean. BeginLean turns the rendered model only, EndLean
	// puts the matrix back right after the draw call, so the physics and the
	// collision orientation never see it.
	static bool BeginLean(CAutomobile *car);
	static void EndLean(void);
};

#endif // __GTA_DRIFTFEEDBACK_H__
