#ifndef __GTA_DRIFTCONTROL_H__
#define __GTA_DRIFTCONTROL_H__

class CAutomobile;
class CVehicle;

// The tick rate and the entry gate are shared with the telemetry layer (see
// DriftTelemetry.h), so one value serves both.
#define DRIFT_TICK_RATE		50.0f	// game ticks per second; turn speed is per tick
#define DRIFT_ENTRY_STEER	0.15f	// steering input needed to start a drift

// True for the car the drift layer works on: the player's car. AI cars and
// traffic never drift (see ADR 0001).
bool IsDriftCar(CVehicle *car);

// Arcade drift handling for the player's car. See docs/adr/ for the drift
// decision records. The controller is a member of CAutomobile, but only the
// player's car steps it. The state is private; the readers use one snapshot
// (see tSnapshot).
class CDriftControl
{
public:
	enum eDriftPhase {
		DRIFT_PHASE_INACTIVE,
		DRIFT_PHASE_ENGAGE,
		DRIFT_PHASE_DRIFT,
		DRIFT_PHASE_RECOVER
	};

	// One read of the live state (see the Drift snapshot in GLOSSARY.md).
	// GetSnapshot takes it at the point of the read, so the feedback, the
	// telemetry and the on-screen readout all see the same values. Slip and
	// target are radians, the yaw rate is radians per second, the charge is
	// seconds of slide, the boost is a fraction of the top speed, the entry
	// speed is the speed when the drift started and the grip scale is the
	// rear grip the wheels request right now.
	struct tSnapshot
	{
		eDriftPhase ePhase;
		bool bActive;		// the drift phase is not INACTIVE
		bool bHandbrakeTurn;	// low-speed entry: a handbrake turn
		float fSlip;		// angle between the car's forward and its velocity
		float fTargetSlip;	// slip angle the drift holds
		float fYawRate;		// yaw rate of the car
		float fGripCut;		// 0.0 = rear wheels grip normally, 1.0 = full cutoff
		float fGripScale;	// rear grip the wheels request right now
		float fCharge;		// seconds of real sliding, charges the boost
		float fBoost;		// stacked boost, fraction of top speed on top
		float fEntrySpeed;	// speed when the drift started
	};

	CDriftControl(void);

	void Update(CAutomobile *car, float timeStep);
	void Reset(void);
	// The read surface of the live state (see tSnapshot).
	tSnapshot GetSnapshot(CAutomobile *car) const;
	// True while the drift owns the rear wheels (entry and full drift).
	bool IsDrifting(void) const { return m_Phase == DRIFT_PHASE_ENGAGE || m_Phase == DRIFT_PHASE_DRIFT; }
	// True from entry until the exit ramp finishes.
	bool IsActive(void) const { return m_Phase != DRIFT_PHASE_INACTIVE; }
	float GetRearGripScale(void) const;
	float GetSteerAngleMult(void);
	static const char *GetPhaseName(eDriftPhase phase);

private:
	bool CanStartDrift(CAutomobile *car, float fwdSpeed, bool onGround, bool handbrake);
	bool TryStartDrift(CAutomobile *car, float fwdSpeed, bool onGround, bool handbrake, bool fromRecover);
	void UpdateYawControl(CAutomobile *car, float dt);
	bool UpdateTargetAngle(CAutomobile *car, float dt);
	void ApplyYawRate(CAutomobile *car, float yawTargetRate, float dt);
	float GetCurveRate(void);
	float GetEffectScale(void);
	void AlignVelocity(CAutomobile *car, float dt);
	void BleedSlip(CAutomobile *car, float dt);
	void SteerVelocity(CAutomobile *car, float error, float gain, float rateCap, float dt);
	void RotateVelocity(CAutomobile *car, float angle);
	void HoldDriftSpeed(CAutomobile *car, float dt);
	void ApplyBoost(CAutomobile *car, float dt);
	void EndDrift(void);
	void StartRecover(void);
	float GetMinGrip(void) const;
	float GetMaxSpeed(CAutomobile *car);
	float GetForwardSpeed(CAutomobile *car);
	void PrintDebugState(CAutomobile *car);

	// The drift state. The layers outside the drift read the snapshot (see
	// tSnapshot); these members stay private.
	eDriftPhase m_Phase;
	float m_fGripCut;	// 0.0 = rear wheels grip normally, 1.0 = full cutoff
	float m_fPhaseTimer;	// seconds spent in the current phase
	float m_fDriftTime;	// seconds of real sliding, charges the boost
	float m_fSlipAngle;	// angle between the car's forward and its velocity, radians
	float m_fTargetSlip;	// slip angle the drift holds, radians
	float m_fBoostLevel;	// stacked boost, fraction of top speed on top
	float m_fEntrySpeed;	// speed when the drift started
	int8 m_nEntryDir;	// yaw direction of the drift, +1 or -1
	bool m_bHandbrakeTurn;	// low-speed entry: a handbrake turn, not a hold-angle drift
};

#endif // __GTA_DRIFTCONTROL_H__
