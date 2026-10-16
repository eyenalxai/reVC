#ifndef __GTA_DRIFTCONTROL_H__
#define __GTA_DRIFTCONTROL_H__

class CAutomobile;

// The tick rate and the entry gate are shared with the telemetry layer (see
// DriftTelemetry.h), so one value serves both.
#define DRIFT_TICK_RATE		50.0f	// game ticks per second; turn speed is per tick
#define DRIFT_ENTRY_STEER	0.15f	// steering input needed to start a drift

// Arcade drift handling for the player's car. See docs/adr/ for the drift
// decision records. The controller is a member of CAutomobile, but only the
// player's car steps it.
class CDriftControl
{
public:
	enum eDriftPhase {
		DRIFT_PHASE_INACTIVE,
		DRIFT_PHASE_ENGAGE,
		DRIFT_PHASE_DRIFT,
		DRIFT_PHASE_RECOVER
	};

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

	CDriftControl(void);

	void Update(CAutomobile *car, float timeStep);
	void Reset(void);
	// True while the drift owns the rear wheels (entry and full drift).
	bool IsDrifting(void) { return m_Phase == DRIFT_PHASE_ENGAGE || m_Phase == DRIFT_PHASE_DRIFT; }
	// True from entry until the exit ramp finishes.
	bool IsActive(void) { return m_Phase != DRIFT_PHASE_INACTIVE; }
	float GetRearGripScale(void);
	float GetSteerAngleMult(void);
	// Readouts that the on-screen debug state and the telemetry file share.
	static const char *GetPhaseName(eDriftPhase phase);
	float GetSlipAngleDeg(void);
	float GetTargetSlipDeg(void);
	float GetYawRateDeg(CAutomobile *car);
	void PrintDebugState(CAutomobile *car);

private:
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
	float GetMinGrip(void);
	float GetMaxSpeed(CAutomobile *car);
	float GetForwardSpeed(CAutomobile *car);
};

#endif // __GTA_DRIFTCONTROL_H__
