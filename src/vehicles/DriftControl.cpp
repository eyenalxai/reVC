#include "common.h"
#include "main.h"

#include "Vehicle.h"
#include "Automobile.h"
#include "DriftControl.h"

// All drift tuning lives in this block. Times are real seconds, angles are
// degrees, speeds are a fraction of the handling top speed (fMaxVelocity).
#define DRIFT_ENTRY_SPEED	0.10f	// entry speed, below this the handbrake turns the car
#define DRIFT_MIN_SPEED		0.05f	// below this the car blends back to normal grip
#define DRIFT_ENTRY_STEER	0.15f	// steering input needed to start a drift
#define DRIFT_TURN_YAW		45.0f	// yaw kick of a low-speed handbrake turn, degrees/s
#define DRIFT_TURN_ENGAGE	0.1f	// rear grip ramp in of a handbrake turn
#define DRIFT_GRIP_CUTOFF	0.22f	// rear grip at full effect, 1.0 = original handling
#define DRIFT_GRIP_STICKY	5.0f	// below this slip angle the rear tyre is sticky
#define DRIFT_GRIP_SLIDE	15.0f	// above this slip angle the rear tyre is let go
#define DRIFT_ENGAGE_TIME	0.3f	// rear grip ramp in
#define DRIFT_RELEASE_TIME	0.4f	// rear grip ramp out
#define DRIFT_BLEND_TIME	0.5f	// ramp out when the drift got too slow
#define DRIFT_HOLD_ANGLE	20.0f	// drift angle the model holds on entry
#define DRIFT_ANGLE_RATE	90.0f	// steering integration of the hold angle, degrees/s
#define DRIFT_ANGLE_MAX		45.0f	// hold angle cap
#define DRIFT_ANGLE_MIN		(-5.0f)	// hold angle floor; the drift ends past zero
#define DRIFT_SLIP_CAP		60.0f	// slip angle past which the align assist pulls
#define DRIFT_YAW_GAIN		4.0f	// angle error to yaw rate, 1/s
#define DRIFT_YAW_MAX		1.5f	// maximum yaw rate, rad/s
#define DRIFT_YAW_ACCEL		6.0f	// yaw rate ramp, rad/s^2
#define DRIFT_ALIGN_GAIN	2.0f	// aligning pull past the slip cap, 1/s
#define DRIFT_STEERLOCK_MULT	1.3f	// front wheel lock while drifting
#define DRIFT_SPEED_HOLD	1.0f	// fraction of the lost speed given back per second
#define DRIFT_RECOVER_TIME	0.3f	// seconds of exit alignment after the handbrake
#define DRIFT_RECOVER_ALIGN	4.0f	// exit counter-yaw per radian of slip, 1/s
#define DRIFT_RECOVER_SLIP	2.0f	// below this remaining slip the assist stops
#define DRIFT_SLIP_MIN		10.0f	// degrees of slide that charge the boost
#define DRIFT_TIER1_TIME	0.6f	// drift seconds for boost tier 1
#define DRIFT_TIER2_TIME	1.2f	// drift seconds for boost tier 2
#define DRIFT_BOOST_TIER1	0.05f	// forward push of tier 1, fraction of top speed
#define DRIFT_BOOST_TIER2	0.09f	// forward push of tier 2, fraction of top speed
#define DRIFT_BOOST_MAX		0.25f	// stacked boosts stop at 1.25x top speed
#define DRIFT_BOOST_PUSH	0.8f	// boost acceleration, fraction of top speed per second
#define DRIFT_BOOST_DRAIN	0.04f	// boost level that drains per second
#define DRIFT_TICK_RATE		50.0f	// game ticks per second; turn speed is per tick

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
	if(m_bHandbrakeTurn){
		// A handbrake turn cuts the grip hard, whatever the angle is.
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
		float fwdSpeed = DotProduct(car->m_vecMoveSpeed, car->GetForward());
		if(fwdSpeed < DRIFT_ENTRY_SPEED*GetMaxSpeed(car)){
			// Keep the target on the angle the car has, so a promotion above
			// the gate happens without a kick.
			m_fTargetSlip = m_nEntryDir*Min(Abs(m_fSlipAngle), DEGTORAD(DRIFT_ANGLE_MAX));
			return true;
		}
		m_bHandbrakeTurn = false;
	}

	// Torque balancing: the drift holds a target slip angle. Steering
	// integrates the target and nothing straightens the car on its own, so
	// letting go of the stick keeps the angle and counter-steering unwinds it.
	float target = m_fTargetSlip + car->m_fSteerInput*DEGTORAD(DRIFT_ANGLE_RATE)*dt;
	float angle = target*m_nEntryDir;

	// A hard counter flick crosses zero and ends the drift.
	if(angle < DEGTORAD(DRIFT_ANGLE_MIN))
		return false;

	m_fTargetSlip = m_nEntryDir*Clamp(angle, 0.0f, DEGTORAD(DRIFT_ANGLE_MAX));
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

void
CDriftControl::UpdateYawControl(CAutomobile *car, float dt)
{
	// Move the car's yaw towards the value that closes the angle error. The
	// yaw authority is absolute, so the drift turns the same at any speed.
	float yawTargetRate;
	if(m_bHandbrakeTurn){
		// A handbrake turn is a fixed yaw kick towards the turn. It does not
		// depend on speed, so the car keeps coming around.
		yawTargetRate = m_nEntryDir*DEGTORAD(DRIFT_TURN_YAW);
	}else{
		yawTargetRate = DRIFT_YAW_GAIN*gDriftAssist*(m_fTargetSlip - m_fSlipAngle);
	}

	// Light assist: past the cap an aligning torque pulls the car back in line.
	float slipCap = DEGTORAD(DRIFT_SLIP_CAP);
	float slipOverCap = m_fSlipAngle - Clamp(m_fSlipAngle, -slipCap, slipCap);
	yawTargetRate -= DRIFT_ALIGN_GAIN*gDriftAssist * slipOverCap;

	ApplyYawRate(car, yawTargetRate, dt);
}

void
CDriftControl::HoldDriftSpeed(CAutomobile *car, float dt)
{
	// The drift must not cost speed: give part of any speed the slide ate
	// back. The push never goes past the entry speed and follows the current
	// velocity, so it cannot make the car faster than it entered.
	float speed = car->m_vecMoveSpeed.Magnitude();
	if(speed > 0.01f && speed < m_fEntrySpeed){
		CVector dir = car->m_vecMoveSpeed*(1.0f/speed);
		car->ApplyMoveForce(dir * ((m_fEntrySpeed - speed)*DRIFT_SPEED_HOLD*car->m_fMass*dt));
	}
}

void
CDriftControl::ApplyBoost(CAutomobile *car, float dt)
{
	if(m_fBoostLevel <= 0.0f)
		return;

	// A stacked boost pushes towards the speed the stack asks for, on top of
	// the normal top speed. It drains on its own, so a chain of clean drifts
	// keeps the car fast while the player can hold the line.
	float fMaxVelocity = GetMaxSpeed(car);
	float fwdSpeed = DotProduct(car->m_vecMoveSpeed, car->GetForward());
	float push = Min(fMaxVelocity*(1.0f + m_fBoostLevel) - fwdSpeed, DRIFT_BOOST_PUSH*fMaxVelocity*dt);
	if(push > 0.0f)
		car->ApplyMoveForce(car->GetForward() * push * car->m_fMass);

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

void
CDriftControl::Update(CAutomobile *car, float timeStep)
{
	if(!gDriftEnabled){
		Reset();
		return;
	}

	float dt = timeStep / DRIFT_TICK_RATE;	// the time step is 1.0 at 50 fps

	float fwdSpeed = DotProduct(car->m_vecMoveSpeed, car->GetForward());
	float rightSpeed = DotProduct(car->m_vecMoveSpeed, car->GetRight());
	m_fSlipAngle = Atan2(rightSpeed, fwdSpeed);

	float fMaxVelocity = GetMaxSpeed(car);
	bool onGround = car->m_nWheelsOnGround >= 2;
	bool handbrake = !!car->bIsHandbrakeOn;

	// The boost stack lives outside the drift phases, so a chained boost keeps
	// pushing after the handbrake is released.
	ApplyBoost(car, dt);

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
		// over the first moments of the recovery, on top of the grip ramp.
		if(m_fPhaseTimer < DRIFT_RECOVER_TIME && Abs(m_fSlipAngle) > DEGTORAD(DRIFT_RECOVER_SLIP))
			ApplyYawRate(car, -DRIFT_RECOVER_ALIGN*gDriftAssist*m_fSlipAngle, dt);
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
		if(!handbrake || fwdSpeed < DRIFT_MIN_SPEED*fMaxVelocity){
			// A charged drift gives its boost when the handbrake is released.
			if(!handbrake)
				EndDrift();
			StartRecover();
		}else{
			// Only a real slide charges the boost.
			if(Abs(m_fSlipAngle) > DEGTORAD(DRIFT_SLIP_MIN))
				m_fDriftTime += dt;
			if(!UpdateTargetAngle(car, dt)){
				// The counter flick unwound the angle: the drift is over.
				EndDrift();
				StartRecover();
			}else{
				UpdateYawControl(car, dt);
				HoldDriftSpeed(car, dt);
			}
		}
	}
}
