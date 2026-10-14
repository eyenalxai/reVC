#include "common.h"
#include "main.h"

#include "General.h"
#include "Timer.h"
#include "Vehicle.h"
#include "Automobile.h"
#include "DriftControl.h"
#include "DriftFeedback.h"

// One set of state is enough: only the player's car can drift.
static float ms_fStrength;	// signed drift strength, -1..1
static uint32 ms_nLastFrame = (uint32)-1;
static RwMatrix ms_savedRootMatrix;
static RwFrame *ms_pLeanedFrame;

// The target the strength eases towards. It comes from the slip angle alone,
// so a counter-steered hold and a handbrake spin both read the same way.
static float
DriftFeedbackTarget(CAutomobile *car)
{
	if(!gDriftEnabled || car == nil)
		return 0.0f;
	// AI cars and traffic never feed the feedback.
	if(car->GetStatus() != STATUS_PLAYER)
		return 0.0f;
	// The drift controller only keeps the slip angle fresh while it runs.
	if(!car->m_DriftControl.IsActive())
		return 0.0f;

	float slip = car->m_DriftControl.m_fSlipAngle;
	float slipDeg = Abs(slip)*RADTODEG(1.0f);
	if(slipDeg <= DRIFT_FEEDBACK_SLIP_MIN)
		return 0.0f;

	float strength = Min((slipDeg - DRIFT_FEEDBACK_SLIP_MIN)/(DRIFT_FEEDBACK_SLIP_FULL - DRIFT_FEEDBACK_SLIP_MIN), 1.0f);
	return slip < 0.0f ? -strength : strength;
}

void
CDriftFeedback::Update(CAutomobile *car, float timeStep)
{
	if(!gDriftEnabled){
		ms_fStrength = 0.0f;
		return;
	}

	if(ms_nLastFrame == CTimer::GetFrameCounter())
		return;
	ms_nLastFrame = CTimer::GetFrameCounter();

	float target = DriftFeedbackTarget(car);
	float dt = Min(timeStep, DRIFT_FEEDBACK_MAX_STEP);
	float rate = Abs(target) > Abs(ms_fStrength) ? DRIFT_FEEDBACK_ATTACK : DRIFT_FEEDBACK_RELEASE;
	ms_fStrength += (target - ms_fStrength)*Min(rate*dt, 1.0f);
	if(Abs(ms_fStrength) < 0.001f)
		ms_fStrength = 0.0f;
}

float
CDriftFeedback::GetStrength(void)
{
	// Never hand out a stale lean while the feature is off.
	return gDriftEnabled ? ms_fStrength : 0.0f;
}

int
CDriftFeedback::SmokeCount(float strength)
{
	// Time based, so the amount of smoke does not follow the frame rate.
	float expected = DRIFT_SKID_SMOKE_PER_SEC*strength*CTimer::GetTimeStepInSeconds();
	int count = (int)expected;
	if(CGeneral::GetRandomNumberInRange(0.0f, 1.0f) < expected - count)
		count++;
	if(count > DRIFT_SKID_SMOKE_MAX)
		count = DRIFT_SKID_SMOKE_MAX;
	return count;
}

bool
CDriftFeedback::BeginLean(CAutomobile *car)
{
	float strength = GetStrength();
	if(strength == 0.0f || ms_pLeanedFrame)
		return false;
	if(car == nil || car->m_rwObject == nil)
		return false;

	RwFrame *frame = RpClumpGetFrame((RpClump*)car->m_rwObject);
	RwMatrix *root = RwFrameGetMatrix(frame);
	RwMatrixCopy(&ms_savedRootMatrix, root);

	float roll = DEGTORAD(DRIFT_LEAN_ROLL_MAX)*strength;
	float pitch = DEGTORAD(DRIFT_LEAN_PITCH_MAX)*Abs(strength);
	float cr = Cos(roll), sr = Sin(roll);
	float cp = Cos(pitch), sp = Sin(pitch);

	// CMatrix knows how the game axes sit in the rw matrix, so build the
	// leaned basis through it instead of poking the rw fields.
	CMatrix mat(root);
	CVector right = mat.GetRight();
	CVector fwd = mat.GetForward();
	CVector up = mat.GetUp();

	// Roll about the forward axis: the roof leans to the side the car slides
	// to. Pitch about the right axis: the nose lifts a little.
	CVector leanedRight = cr*right - sr*up;
	CVector leanedUp = sr*right + cr*up;
	mat.GetRight() = leanedRight;
	mat.GetForward() = cp*fwd + sp*leanedUp;
	mat.GetUp() = cp*leanedUp - sp*fwd;
	mat.UpdateRW();
	RwFrameUpdateObjects(frame);

	ms_pLeanedFrame = frame;
	return true;
}

void
CDriftFeedback::EndLean(void)
{
	if(ms_pLeanedFrame == nil)
		return;

	RwMatrix *root = RwFrameGetMatrix(ms_pLeanedFrame);
	RwMatrixCopy(root, &ms_savedRootMatrix);
	RwMatrixUpdate(root);
	RwFrameUpdateObjects(ms_pLeanedFrame);

	ms_pLeanedFrame = nil;
}
