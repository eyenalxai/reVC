#include "common.h"
#include "main.h"

#include "Debug.h"
#include "Vehicle.h"
#include "Automobile.h"
#include "DriftControl.h"
#include "DriftTelemetry.h"

// All drift tuning lives in this block. Times are real seconds, angles are
// degrees, speeds are a fraction of the handling top speed (fMaxVelocity).
#define DRIFT_ENTRY_SPEED	0.10f	// entry speed, below this the handbrake turns the car
#define DRIFT_MIN_SPEED		0.05f	// below this the car blends back to normal grip
#define DRIFT_TURN_YAW		45.0f	// yaw kick of a low-speed handbrake turn, degrees/s
#define DRIFT_TURN_ENGAGE	0.1f	// rear grip ramp in of a handbrake turn
#define DRIFT_GRIP_CUTOFF	0.22f	// rear grip at full effect, 1.0 = original handling
#define DRIFT_GRIP_STICKY	5.0f	// below this slip angle the rear tyre is sticky
#define DRIFT_GRIP_SLIDE	15.0f	// above this slip angle the rear tyre is let go
#define DRIFT_ENGAGE_TIME	0.3f	// rear grip ramp in
#define DRIFT_RELEASE_TIME	0.6f	// rear grip ramp out
#define DRIFT_BLEND_TIME	0.5f	// ramp out when the drift got too slow
#define DRIFT_HOLD_ANGLE	20.0f	// drift angle the model holds on entry
#define DRIFT_ANGLE_RATE	90.0f	// steering integration of the hold angle, degrees/s
#define DRIFT_ANGLE_MAX		45.0f	// hold angle cap
#define DRIFT_ANGLE_MIN		(-5.0f)	// hold angle floor; the drift ends past zero
#define DRIFT_SLIP_CAP		60.0f	// slip angle past which the align assist pulls
#define DRIFT_YAW_GAIN		4.0f	// angle error to yaw rate, 1/s
#define DRIFT_YAW_MAX		1.5f	// maximum yaw rate, rad/s
#define DRIFT_YAW_ACCEL		6.0f	// yaw rate ramp, rad/s^2
#define DRIFT_CURVE_RATE	60.0f	// curve rate at full angle, degrees/s
#define DRIFT_ALIGN_GAIN	2.0f	// aligning pull past the slip cap, 1/s
#define DRIFT_VEL_ALIGN_GAIN	3.0f	// slip error to velocity alignment, 1/s
#define DRIFT_VEL_ALIGN_RATE	120.0f	// cap on the velocity alignment, degrees/s
#define DRIFT_STEERLOCK_MULT	1.3f	// front wheel lock while drifting
#define DRIFT_SPEED_HOLD	1.0f	// fraction of the lost speed given back per second
#define DRIFT_SPEED_MIN_DIR	0.01f	// below this speed the velocity cannot be normalised
#define DRIFT_RECOVER_TIME	0.6f	// seconds of exit alignment after the handbrake
#define DRIFT_RECOVER_ALIGN	4.0f	// exit counter-yaw per radian of slip, 1/s
#define DRIFT_RECOVER_SLIP	2.0f	// below this remaining slip the assist stops
#define DRIFT_BLEED_GAIN	4.0f	// remaining slip to the exit bleed, 1/s
#define DRIFT_BLEED_RATE	90.0f	// cap on the exit bleed, degrees/s
#define DRIFT_SLIP_MIN		10.0f	// degrees of slide that charge the boost
#define DRIFT_TIER1_TIME	0.35f	// drift seconds for boost tier 1
#define DRIFT_TIER2_TIME	0.8f	// drift seconds for boost tier 2
#define DRIFT_BOOST_TIER1	0.04f	// boost of tier 1, fraction of top speed
#define DRIFT_BOOST_TIER2	0.07f	// boost of tier 2, fraction of top speed
#define DRIFT_BOOST_MAX		0.15f	// stacked boosts stop at 1.15x top speed
#define DRIFT_BOOST_PUSH	0.5f	// boost acceleration, fraction of top speed per second
#define DRIFT_BOOST_DRAIN	0.08f	// boost level that drains per second

// The readout mirrors the player's drift for the debug menu (see re3.cpp).
float gDriftDebugSlip;	// slip angle, degrees
float gDriftDebugTarget;	// target slip angle, degrees
float gDriftDebugYaw;	// yaw rate, degrees per second
float gDriftDebugCharge;	// seconds of slide that charge the boost
float gDriftDebugBoost;	// stacked boost, fraction of top speed
float gDriftDebugSpeed;	// speed change since entry, fraction of top speed

CDriftControl::CDriftControl(void)
{
	Reset();
}

void
CDriftControl::Reset(void)
{
	m_Phase = DRIFT_PHASE_INACTIVE;
	m_fGripCut = 0.0f;
	m_fPhaseTimer = 0.0f;
	m_fDriftTime = 0.0f;
	m_fSlipAngle = 0.0f;
	m_fTargetSlip = 0.0f;
	m_fBoostLevel = 0.0f;
	m_fEntrySpeed = 0.0f;
	m_nEntryDir = 0;
	m_bHandbrakeTurn = false;
}

float
CDriftControl::GetMinGrip(void)
{
	// Strength scales how far the rear grip is cut.
	return Clamp(1.0f - (1.0f - DRIFT_GRIP_CUTOFF)*gDriftStrength, 0.05f, 1.0f);
}

float
CDriftControl::GetMaxSpeed(CAutomobile *car)
{
	return car->pHandling->Transmission.fMaxVelocity;
}

// The car's speed along its nose: the share the boost cap and the entry gate
// use.
float
CDriftControl::GetForwardSpeed(CAutomobile *car)
{
	return DotProduct(car->m_vecMoveSpeed, car->GetForward());
}

float
CDriftControl::GetRearGripScale(void)
{
	if(m_Phase == DRIFT_PHASE_INACTIVE)
		return 1.0f;

	// The rear grip is not a constant cut: the tyre is sticky just off centre
	// and lets go as the slide grows. The player feels the steepness of this
	// curve, so it is hand-shaped on top of the surface and temperature terms
	// that the caller still applies.
	float grip;
	if(m_bHandbrakeTurn && IsDrifting()){
		// A handbrake turn cuts the grip hard, whatever the angle is. The
		// recovery goes back to the curve, so the exit hooks up.
		grip = GetMinGrip();
	}else{
		float t = (Abs(m_fSlipAngle) - DEGTORAD(DRIFT_GRIP_STICKY)) /
		          (DEGTORAD(DRIFT_GRIP_SLIDE) - DEGTORAD(DRIFT_GRIP_STICKY));
		grip = 1.0f + (GetMinGrip() - 1.0f)*Clamp(t, 0.0f, 1.0f);
	}

	// The phase ramps the request in on entry and out on exit.
	return 1.0f - (1.0f - grip)*m_fGripCut;
}

float
CDriftControl::GetSteerAngleMult(void)
{
	if(m_Phase == DRIFT_PHASE_INACTIVE)
		return 1.0f;

	// While the drift is on, the front wheels anchor the slide. A little more
	// lock than the original handling gives the counter-steer a readable feel.
	// It follows the grip cut, so it fades out during the exit.
	return 1.0f + (DRIFT_STEERLOCK_MULT - 1.0f)*m_fGripCut;
}

bool
CDriftControl::UpdateTargetAngle(CAutomobile *car, float dt)
{
	// A handbrake turn does not steer with the angle model, and the throttle
	// can carry the car past the entry gate. Then the turn becomes a real
	// drift that holds the angle the car has.
	if(m_bHandbrakeTurn){
		float fwdSpeed = GetForwardSpeed(car);
		if(fwdSpeed < DRIFT_ENTRY_SPEED*GetMaxSpeed(car)){
			// Keep the target on the angle the car has, so a promotion above
			// the gate happens without a kick.
			m_fTargetSlip = m_nEntryDir*Min(Abs(m_fSlipAngle), DEGTORAD(DRIFT_ANGLE_MAX));
			return true;
		}
		m_bHandbrakeTurn = false;
		CDriftTelemetry::Event(car, this, "PROMOTE");
	}

	// Torque balancing: the drift holds a target slip angle. Steering
	// integrates the target and nothing straightens the car on its own, so
	// letting go of the stick keeps the angle and counter-steering unwinds it.
	float target = m_fTargetSlip + car->m_fSteerInput*DEGTORAD(DRIFT_ANGLE_RATE)*dt;
	float angle = target*m_nEntryDir;

	// A hard counter flick winds the target down through zero. It must be
	// held to break the drift, so the target keeps its wind-down room and the
	// exit is time based, not a single gated frame.
	if(angle <= DEGTORAD(DRIFT_ANGLE_MIN))
		return false;

	m_fTargetSlip = m_nEntryDir*Clamp(angle, DEGTORAD(DRIFT_ANGLE_MIN), DEGTORAD(DRIFT_ANGLE_MAX));
	return true;
}

void
CDriftControl::ApplyYawRate(CAutomobile *car, float yawTargetRate, float dt)
{
	yawTargetRate = Clamp(yawTargetRate, -DRIFT_YAW_MAX, DRIFT_YAW_MAX);

	// Rate-limit the yaw acceleration and write it to the car's turn speed, as
	// the physical turn force would. Collisions still override this later.
	float yawRate = DotProduct(car->m_vecTurnSpeed, car->GetUp()) * DRIFT_TICK_RATE;
	float newYawRate = yawRate + Clamp(yawTargetRate - yawRate, -DRIFT_YAW_ACCEL*dt, DRIFT_YAW_ACCEL*dt);
	car->m_vecTurnSpeed += car->GetUp() * ((newYawRate - yawRate) / DRIFT_TICK_RATE);
}

// The rate the drift turns the whole car at, nose and direction of travel:
// the curve rate. It follows the target angle, so a small angle turns gently
// and a full angle turns hard, and it fades with the strength and the grip
// cut like the alignment does.
float
CDriftControl::GetCurveRate(void)
{
	float rate = m_nEntryDir * DEGTORAD(DRIFT_CURVE_RATE) *
	             Clamp(Abs(m_fTargetSlip)/DEGTORAD(DRIFT_ANGLE_MAX), 0.0f, 1.0f) *
	             GetEffectScale();
	// The nose can only turn at DRIFT_YAW_MAX, so the curve rate stops there
	// too; the nose and the velocity stay together and the slip keeps its
	// target.
	return Clamp(rate, -DRIFT_YAW_MAX, DRIFT_YAW_MAX);
}

// How strongly the drift's own effects apply right now: the player's strength
// setting and the ramp in and out of the grip cut.
float
CDriftControl::GetEffectScale(void)
{
	return gDriftStrength * m_fGripCut;
}

void
CDriftControl::UpdateYawControl(CAutomobile *car, float dt)
{
	// Move the car's yaw towards the value that closes the angle error, on
	// top of the drift's own turn. The yaw authority is absolute, so the
	// drift turns the same at any speed.
	float yawTargetRate;
	if(m_bHandbrakeTurn){
		// A handbrake turn is a fixed yaw kick towards the turn. It does not
		// depend on speed, so the car keeps coming around.
		yawTargetRate = m_nEntryDir*DEGTORAD(DRIFT_TURN_YAW);
	}else{
		// The curve rate carries the whole car around the corner; the angle
		// error on top of it brings the slip to the target and holds it.
		yawTargetRate = GetCurveRate() +
		                DRIFT_YAW_GAIN*gDriftAssist*(m_fTargetSlip - m_fSlipAngle);
	}

	// Light assist: past the cap an aligning torque pulls the car back in line.
	float slipCap = DEGTORAD(DRIFT_SLIP_CAP);
	float slipOverCap = m_fSlipAngle - Clamp(m_fSlipAngle, -slipCap, slipCap);
	yawTargetRate -= DRIFT_ALIGN_GAIN*gDriftAssist * slipOverCap;

	ApplyYawRate(car, yawTargetRate, dt);
}

// Rotates the car's velocity by angle radians around its up axis. The part of
// the velocity along the up axis does not turn, so the length is exact: the
// drift steers the direction of travel and never pays speed for it.
void
CDriftControl::RotateVelocity(CAutomobile *car, float angle)
{
	CVector up = car->GetUp();
	CVector v = car->m_vecMoveSpeed;
	CVector alongUp = DotProduct(v, up) * up;
	float c = Cos(angle);
	float s = Sin(angle);

	v -= alongUp;
	car->m_vecMoveSpeed = alongUp + v*c + CrossProduct(up, v)*s;
}

// Steers the velocity towards the slip angle the drift holds and turns it by
// the same curve rate as the nose. The yaw controller turns the nose; this
// turns the velocity with it, so the hold angle is reached at street speed,
// not only at the top end, and the car carries the corner instead of sliding
// straight.
void
CDriftControl::AlignVelocity(CAutomobile *car, float dt)
{
	// A handbrake turn only turns the car; the angle model is off.
	if(m_bHandbrakeTurn)
		return;

	// The whole car turns: nose and velocity rotate together, so the slip
	// stays at the target while the drift works through the corner. The
	// error correction on top brings the slip back to the target when it
	// drifts off it.
	RotateVelocity(car, GetCurveRate()*dt);

	SteerVelocity(car, m_fSlipAngle - m_fTargetSlip,
	              DRIFT_VEL_ALIGN_GAIN, DEGTORAD(DRIFT_VEL_ALIGN_RATE), dt);
}

// Bleeds the slip that is left towards zero during the exit, on top of the
// counter-yaw. It keeps a normal release from growing the slide and hooks the
// car up inside the recovery window.
void
CDriftControl::BleedSlip(CAutomobile *car, float dt)
{
	SteerVelocity(car, m_fSlipAngle, DRIFT_BLEED_GAIN, DEGTORAD(DRIFT_BLEED_RATE), dt);
}

// One bounded velocity rotation from an error. The rate follows the error with
// a cap and fades with the strength and the grip cut, so the entry and the
// exit stay smooth; a wild release can still spin.
void
CDriftControl::SteerVelocity(CAutomobile *car, float error, float gain, float rateCap, float dt)
{
	float rate = Clamp(error*gain, -rateCap, rateCap);
	RotateVelocity(car, rate*GetEffectScale()*dt);
}

void
CDriftControl::HoldDriftSpeed(CAutomobile *car, float dt)
{
	// The drift must not cost speed: give part of any speed the slide ate
	// back. The push never goes past the entry speed and follows the current
	// velocity, so it cannot make the car faster than it entered.
	float speed = car->m_vecMoveSpeed.Magnitude();
	if(speed > DRIFT_SPEED_MIN_DIR && speed < m_fEntrySpeed){
		CVector dir = car->m_vecMoveSpeed*(1.0f/speed);
		car->ApplyMoveForce(dir * ((m_fEntrySpeed - speed)*DRIFT_SPEED_HOLD*car->m_fMass*dt));
	}
}

void
CDriftControl::ApplyBoost(CAutomobile *car, float dt)
{
	if(m_fBoostLevel <= 0.0f)
		return;

	// A stacked boost pushes along the nose: at slip its sideways part works
	// against the slide, so the boost pulls the car straight while it lands
	// and the player feels the rear grip come back. The headroom is measured
	// on the forward speed, so the level still reads as a fraction of the top
	// speed; the total speed can pass that cap for a moment while a slide is
	// scrubbed, and that is accepted.
	float fMaxVelocity = GetMaxSpeed(car);
	CVector forward = car->GetForward();
	float push = Min(fMaxVelocity*(1.0f + m_fBoostLevel) - DotProduct(car->m_vecMoveSpeed, forward), DRIFT_BOOST_PUSH*fMaxVelocity*dt);
	if(push > 0.0f)
		car->ApplyMoveForce(forward * push * car->m_fMass);

	m_fBoostLevel = Max(m_fBoostLevel - DRIFT_BOOST_DRAIN*dt, 0.0f);
}

void
CDriftControl::EndDrift(void)
{
	// A drift that slid long enough stacks its boost. A boost that is still
	// running takes the new one on top, up to the stack cap.
	if(m_fDriftTime < DRIFT_TIER1_TIME)
		return;

	float boost = m_fDriftTime >= DRIFT_TIER2_TIME ? DRIFT_BOOST_TIER2 : DRIFT_BOOST_TIER1;
	m_fBoostLevel = Min(m_fBoostLevel + boost, DRIFT_BOOST_MAX);
}

void
CDriftControl::StartRecover(void)
{
	m_Phase = DRIFT_PHASE_RECOVER;
	m_fPhaseTimer = 0.0f;
	m_fDriftTime = 0.0f;
}

const char *
CDriftControl::GetPhaseName(eDriftPhase phase)
{
	static const char *names[] = { "INACTIVE", "ENGAGE", "DRIFT", "RECOVER" };
	return names[phase];
}

float
CDriftControl::GetSlipAngleDeg(void)
{
	return RADTODEG(m_fSlipAngle);
}

float
CDriftControl::GetTargetSlipDeg(void)
{
	return RADTODEG(m_fTargetSlip);
}

float
CDriftControl::GetYawRateDeg(CAutomobile *car)
{
	return RADTODEG(DotProduct(car->m_vecTurnSpeed, car->GetUp()) * DRIFT_TICK_RATE);
}

void
CDriftControl::PrintDebugState(CAutomobile *car)
{
	char buf[128];

	float fMaxVelocity = GetMaxSpeed(car);

	gDriftDebugSlip = GetSlipAngleDeg();
	gDriftDebugTarget = GetTargetSlipDeg();
	gDriftDebugYaw = GetYawRateDeg(car);
	gDriftDebugCharge = m_fDriftTime;
	gDriftDebugBoost = m_fBoostLevel;
	gDriftDebugSpeed = (car->m_vecMoveSpeed.Magnitude() - m_fEntrySpeed)/fMaxVelocity;

	sprintf(buf, "Drift %-8s%s slip %.1f/%.1f yaw %.0f charge %.2f boost %.2f speed %+.2f",
		GetPhaseName(m_Phase), m_bHandbrakeTurn ? " turn" : "",

		gDriftDebugSlip, gDriftDebugTarget, gDriftDebugYaw, gDriftDebugCharge, gDriftDebugBoost, gDriftDebugSpeed);
	CDebug::PrintAt(buf, 2, 5);
}

void
CDriftControl::Update(CAutomobile *car, float timeStep)
{
	if(!gDriftEnabled){
		Reset();
		return;
	}

	float dt = timeStep / DRIFT_TICK_RATE;	// the time step is 1.0 at 50 fps

	float fwdSpeed = GetForwardSpeed(car);
	float rightSpeed = DotProduct(car->m_vecMoveSpeed, car->GetRight());
	m_fSlipAngle = Atan2(rightSpeed, fwdSpeed);

	float fMaxVelocity = GetMaxSpeed(car);
	bool onGround = car->m_nWheelsOnGround >= 2;
	bool handbrake = !!car->bIsHandbrakeOn;

	// The boost stack lives outside the drift phases, so a chained boost keeps
	// pushing after the handbrake is released.
	ApplyBoost(car, dt);

	// The readout shows the live drift state while it is on (see re3.cpp);
	// the telemetry records it to a file (see DriftTelemetry.h).
	if(gDriftDebug){
		PrintDebugState(car);
		CDriftTelemetry::Tick(car, this);
	}

	// The drift pauses while the car is in the air.
	if(!onGround && m_Phase != DRIFT_PHASE_INACTIVE)
		return;

	switch(m_Phase){
	case DRIFT_PHASE_INACTIVE:
		// Entry: handbrake + steering. Above the gate that is a drift; below
		// it the car turns on the handbrake, and there is no speed gate for
		// that, only a direction check.
		if(handbrake && onGround &&
		   fwdSpeed > 0.0f &&
		   Abs(car->m_fSteerInput) > DRIFT_ENTRY_STEER){
			m_Phase = DRIFT_PHASE_ENGAGE;
			m_fPhaseTimer = 0.0f;
			m_fDriftTime = 0.0f;
			m_fEntrySpeed = car->m_vecMoveSpeed.Magnitude();
			m_nEntryDir = car->m_fSteerInput > 0.0f ? 1 : -1;
			m_bHandbrakeTurn = fwdSpeed < DRIFT_ENTRY_SPEED*fMaxVelocity;
			// The drift starts at the base hold angle and the player
			// modulates it from there.
			m_fTargetSlip = m_nEntryDir*DEGTORAD(DRIFT_HOLD_ANGLE);
		}
		break;

	case DRIFT_PHASE_ENGAGE:
		m_fPhaseTimer += dt;
		m_fGripCut = Min(m_fPhaseTimer/(m_bHandbrakeTurn ? DRIFT_TURN_ENGAGE : DRIFT_ENGAGE_TIME), 1.0f);
		if(m_fGripCut >= 1.0f)
			m_Phase = DRIFT_PHASE_DRIFT;
		break;

	case DRIFT_PHASE_DRIFT:
		m_fGripCut = 1.0f;
		break;

	case DRIFT_PHASE_RECOVER:
		m_fPhaseTimer += dt;
		if(fwdSpeed < DRIFT_MIN_SPEED*fMaxVelocity)
			m_fGripCut -= dt/DRIFT_BLEND_TIME;
		else
			m_fGripCut -= dt/DRIFT_RELEASE_TIME;
		// Exit alignment: a counter-yaw proportional to the slip that is left,
		// plus the velocity bleed that brings the direction of travel back to
		// the nose, over the first moments of the recovery.
		if(m_fPhaseTimer < DRIFT_RECOVER_TIME){
			if(Abs(m_fSlipAngle) > DEGTORAD(DRIFT_RECOVER_SLIP))
				ApplyYawRate(car, -DRIFT_RECOVER_ALIGN*gDriftAssist*m_fSlipAngle, dt);
			BleedSlip(car, dt);
		}
		if(m_fGripCut <= 0.0f){
			m_fGripCut = 0.0f;
			m_bHandbrakeTurn = false;
			m_Phase = DRIFT_PHASE_INACTIVE;
		}
		break;
	}

	// Exit conditions for a running drift. The handbrake release starts the
	// recovery; a drift that got too slow blends back to normal grip.
	if(m_Phase == DRIFT_PHASE_ENGAGE || m_Phase == DRIFT_PHASE_DRIFT){
		// A handbrake turn keeps working below the drift floor; only the
		// handbrake release or a dead stop ends it.
		bool deadStop = m_bHandbrakeTurn && fwdSpeed < 0.01f;
		bool tooSlow = !m_bHandbrakeTurn && fwdSpeed < DRIFT_MIN_SPEED*fMaxVelocity;
		if(!handbrake || tooSlow || deadStop){
			// A charged drift gives its boost when the handbrake is released.
			if(!handbrake){
				char event[32];
				EndDrift();
				sprintf(event, "RELEASE %.2f", m_fDriftTime);
				CDriftTelemetry::Event(car, this, event);
			}else if(tooSlow){
				CDriftTelemetry::Event(car, this, "SLOW");
			}else{
				CDriftTelemetry::Event(car, this, "STOP");
			}
			StartRecover();
		}else{
			// Only a real slide charges the boost.
			if(Abs(m_fSlipAngle) > DEGTORAD(DRIFT_SLIP_MIN))
				m_fDriftTime += dt;
			if(!UpdateTargetAngle(car, dt)){
				// The counter flick unwound the angle: the drift is over.
				CDriftTelemetry::Event(car, this, "FLICK");
				EndDrift();
				StartRecover();
			}else{
				UpdateYawControl(car, dt);
				AlignVelocity(car, dt);
				HoldDriftSpeed(car, dt);
			}
		}
	}
}
