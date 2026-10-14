#include "common.h"
#include "main.h"

#include "Vehicle.h"
#include "Automobile.h"
#include "DriftControl.h"

// All drift tuning lives in this block. Times are real seconds, speeds are a
// fraction of the handling top speed (fMaxVelocity).
#define DRIFT_ENTRY_SPEED	0.30f	// drift gate, below this the handbrake keeps braking
#define DRIFT_MIN_SPEED		0.25f	// below this the car blends back to normal grip
#define DRIFT_ENTRY_STEER	0.2f	// steering input needed to start a drift
#define DRIFT_GRIP_CUTOFF	0.30f	// rear grip at full effect, 1.0 = original handling
#define DRIFT_ENGAGE_TIME	0.3f	// rear grip ramp in
#define DRIFT_RELEASE_TIME	0.4f	// rear grip ramp out
#define DRIFT_BLEND_TIME	0.5f	// ramp out when the drift got too slow

CDriftControl::CDriftControl(void)
{
	m_Phase = DRIFT_PHASE_INACTIVE;
	m_fGripCut = 0.0f;
	m_fPhaseTimer = 0.0f;
	m_fSlipAngle = 0.0f;
	m_nEntryDir = 0;
}

float
CDriftControl::GetMinGrip(void)
{
	// Strength scales how far the rear grip is cut.
	return Clamp(1.0f - (1.0f - DRIFT_GRIP_CUTOFF)*gDriftStrength, 0.05f, 1.0f);
}

float
CDriftControl::GetRearGripScale(void)
{
	if(m_Phase == DRIFT_PHASE_INACTIVE)
		return 1.0f;
	return 1.0f - (1.0f - GetMinGrip())*m_fGripCut;
}

void
CDriftControl::StartRecover(void)
{
	m_Phase = DRIFT_PHASE_RECOVER;
	m_fPhaseTimer = 0.0f;
}

void
CDriftControl::Update(CAutomobile *car, float timeStep)
{
	if(!gDriftEnabled){
		m_Phase = DRIFT_PHASE_INACTIVE;
		m_fGripCut = 0.0f;
		return;
	}

	float dt = timeStep / 50.0f;	// the time step is 1.0 at 50 fps

	float fwdSpeed = DotProduct(car->m_vecMoveSpeed, car->GetForward());
	float rightSpeed = DotProduct(car->m_vecMoveSpeed, car->GetRight());
	m_fSlipAngle = Atan2(rightSpeed, fwdSpeed);

	float fMaxVelocity = car->pHandling->Transmission.fMaxVelocity;
	bool onGround = car->m_nWheelsOnGround >= 2;
	bool handbrake = !!car->bIsHandbrakeOn;

	// The drift pauses while the car is in the air.
	if(!onGround && m_Phase != DRIFT_PHASE_INACTIVE)
		return;

	switch(m_Phase){
	case DRIFT_PHASE_INACTIVE:
		// Entry: handbrake + steering at speed. Below the gate the handbrake
		// keeps the original braking path.
		if(handbrake && onGround &&
		   fwdSpeed > DRIFT_ENTRY_SPEED*fMaxVelocity &&
		   Abs(car->m_fSteerInput) > DRIFT_ENTRY_STEER){
			m_Phase = DRIFT_PHASE_ENGAGE;
			m_fPhaseTimer = 0.0f;
			m_nEntryDir = car->m_fSteerInput > 0.0f ? 1 : -1;
		}
		break;

	case DRIFT_PHASE_ENGAGE:
		m_fPhaseTimer += dt;
		m_fGripCut = Min(m_fPhaseTimer/DRIFT_ENGAGE_TIME, 1.0f);
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
		if(m_fGripCut <= 0.0f){
			m_fGripCut = 0.0f;
			m_Phase = DRIFT_PHASE_INACTIVE;
		}
		break;
	}

	// Exit conditions for a running drift. The handbrake release starts the
	// hook-up; a drift that got too slow blends back to normal grip.
	if(m_Phase == DRIFT_PHASE_ENGAGE || m_Phase == DRIFT_PHASE_DRIFT)
		if(!handbrake || fwdSpeed < DRIFT_MIN_SPEED*fMaxVelocity)
			StartRecover();
}
