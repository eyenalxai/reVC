#include "common.h"

#include "Automobile.h"
#include "CarCtrl.h"
#include "Camera.h"
#include "Remote.h"
#include "Timer.h"
#include "World.h"
#include "PlayerInfo.h"
#include "Vehicle.h"

void
CRemote::GivePlayerRemoteControlledCar(float x, float y, float z, float rot, uint16 model)
{
	/* The RC car is a special actor: the purpose SPECIAL, forced so the
	 * remote session cannot fail on a budget. The service creates at the
	 * caller's spot with the model's base height over the ground (the old
	 * GetDistanceFromCentreOfMassToBaseOfModel) and inserts it; the session's
	 * own status, lock, route and engine setup follow. */
	SSpawnExplicitRequest request;
	request.purpose = SPAWN_PURPOSE_SPECIAL;
	request.force = true;
	request.carModel = model;
	request.x = x;
	request.y = y;
	request.z = z;
	request.dirX = cosf(rot);
	request.dirY = sinf(rot);
	request.lifetimeMs = 0;
	request.createdBy = SPAWN_VEHICLE_TAG_MISSION;
	request.ground = SPAWN_EXPLICIT_GROUND_BASE;
	request.groundProbeZ = z + 2.0f;
	request.groundNoHitZ = 0.0f;
	request.groundDummies = false;
	request.clearSpace = false;
	CVehicle *created;
	if (!CSpawnService::RequestExplicit(request, &created))
		return;
	CAutomobile *car = (CAutomobile*)created;

	car->SetStatus(STATUS_PLAYER_REMOTE);
	car->bIsLocked = true;

	CCarCtrl::JoinCarWithRoadSystem(car);
	car->AutoPilot.m_nCarMission = MISSION_NONE;
	car->AutoPilot.m_nTempAction = TEMPACT_NONE;
	car->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_STOP_FOR_CARS;
	car->AutoPilot.m_nCruiseSpeed = car->AutoPilot.m_fMaxTrafficSpeed = 9.0f;
	car->AutoPilot.m_nNextLane = car->AutoPilot.m_nCurrentLane = 0;
	car->bEngineOn = true;
	if (FindPlayerVehicle() != nil) 
		FindPlayerVehicle()->SetStatus(STATUS_PLAYER_DISABLED);

	CWorld::Players[CWorld::PlayerInFocus].m_pRemoteVehicle = car;
	CWorld::Players[CWorld::PlayerInFocus].m_pRemoteVehicle->RegisterReference((CEntity**)&CWorld::Players[CWorld::PlayerInFocus].m_pRemoteVehicle);
	if (car->GetVehicleAppearance() == VEHICLE_APPEARANCE_PLANE || car->GetVehicleAppearance() == VEHICLE_APPEARANCE_HELI) {
		TheCamera.TakeControl(car, CCam::MODE_CAM_ON_A_STRING, INTERPOLATION, CAMCONTROL_SCRIPT);
		TheCamera.SetZoomValueCamStringScript(0);
	} else
		TheCamera.TakeControl(car, CCam::MODE_BEHINDCAR, INTERPOLATION, CAMCONTROL_SCRIPT);
}

void
CRemote::TakeRemoteControlledCarFromPlayer(bool blowUp)
{
	if (CWorld::Players[CWorld::PlayerInFocus].m_pRemoteVehicle->VehicleCreatedBy == MISSION_VEHICLE) {
		CWorld::Players[CWorld::PlayerInFocus].m_pRemoteVehicle->VehicleCreatedBy = RANDOM_VEHICLE;
		CCarCtrl::NumMissionCars--;
		CCarCtrl::NumRandomCars++;
	}
	CWorld::Players[CWorld::PlayerInFocus].m_pRemoteVehicle->bIsLocked = false;
	CWorld::Players[CWorld::PlayerInFocus].m_nTimeLostRemoteCar = CTimer::GetTimeInMilliseconds();
	CWorld::Players[CWorld::PlayerInFocus].m_bInRemoteMode = true;
	CWorld::Players[CWorld::PlayerInFocus].field_D5 = blowUp;
	CWorld::Players[CWorld::PlayerInFocus].field_D6 = true;
}
