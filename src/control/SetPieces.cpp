#include "common.h"

#include "SetPieces.h"
#include "Automobile.h"
#include "CarAI.h"
#include "CarCtrl.h"
#include "CopPed.h"
#include "GenericGameStorage.h"
#include "ModelInfo.h"
#include "PlayerPed.h"
#include "Timer.h"
#include "Vehicle.h"
#include "Wanted.h"
#include "World.h"
#include "VarConsole.h"
#include "SaveBuf.h"

#define TIME_BETWEEN_SETPIECE_SPAWNS 20000

bool CSetPieces::bDebug;
uint32 CSetPieces::NumSetPieces;
CSetPiece CSetPieces::aSetPieces[NUM_SETPIECES];

void CSetPieces::Init(void)
{
	bDebug = false;
	NumSetPieces = 0;
#ifndef MASTER
	VarConsole.Add("Show set pieces", &bDebug, true);
#endif
}

void CSetPieces::AddOne(uint8 type, CVector2D vTriggerInf, CVector2D vTriggerSup, CVector2D vSpawn1, CVector2D vTarget1, CVector2D vSpawn2, CVector2D vTarget2)
{
	if (NumSetPieces >= NUM_SETPIECES)
		return;
	aSetPieces[NumSetPieces].m_nType = type;
	aSetPieces[NumSetPieces].m_vTriggerInf.x = Min(vTriggerInf.x, vTriggerSup.x);
	aSetPieces[NumSetPieces].m_vTriggerInf.y = Min(vTriggerInf.y, vTriggerSup.y);
	aSetPieces[NumSetPieces].m_vTriggerSup.x = Max(vTriggerInf.x, vTriggerSup.x);
	aSetPieces[NumSetPieces].m_vTriggerSup.y = Max(vTriggerInf.y, vTriggerSup.y);
	aSetPieces[NumSetPieces].m_vSpawn1 = vSpawn1;
	aSetPieces[NumSetPieces].m_vSpawn2 = vSpawn2;
	aSetPieces[NumSetPieces].m_vTarget1 = vTarget1;
	aSetPieces[NumSetPieces].m_vTarget2 = vTarget2;
	++NumSetPieces;
}

void CSetPieces::Update(void)
{
	int nFirst = NumSetPieces * (CTimer::GetFrameCounter() % 8) / 8;
	int nLast = NumSetPieces * (CTimer::GetFrameCounter() % 8 + 1) / 8;
	for (int i = nFirst; i < nLast; i++)
		aSetPieces[i].Update();
#ifndef MASTER
	// TODO: debug code from mobile
#endif // !MASTER
}

void CSetPieces::Save(uint8* buf, uint32* size)
{
INITSAVEBUF
	WriteSaveBuf(buf, NumSetPieces);
	for (int i = 0; i < NUM_SETPIECES; i++)
		WriteSaveBuf(buf, aSetPieces[i]);
	*size = sizeof(NumSetPieces) + NUM_SETPIECES * sizeof(CSetPiece);
VALIDATESAVEBUF(*size)
}

void CSetPieces::Load(uint8* buf, uint32 size)
{
INITSAVEBUF
	ReadSaveBuf(&NumSetPieces, buf);
	for (int i = 0; i < NUM_SETPIECES; i++)
		ReadSaveBuf(&aSetPieces[i], buf);
VALIDATESAVEBUF(size)
}

void CSetPiece::Update(void)
{
	if (m_nLastTimeCreated != 0 && CTimer::GetTimeInMilliseconds() <= m_nLastTimeCreated + TIME_BETWEEN_SETPIECE_SPAWNS)
		return;
	CVector pos = FindPlayerCoors();
	if (pos.x < m_vTriggerInf.x || pos.x > m_vTriggerSup.x ||
		pos.y < m_vTriggerInf.y || pos.y > m_vTriggerSup.y)
		return;
	switch (m_nType) {
	case SETPIECE_TWOCOPCARSINALLEY:
	{
		if (FindPlayerPed()->m_pWanted->GetWantedLevel() < 1 || FindPlayerVehicle())
			return;
		CVehicle* pVehicle1 = TryToGenerateCopCar(m_vSpawn1, m_vTarget1);
		if (!pVehicle1)
			return;
		CVehicle* pVehicle2 = TryToGenerateCopCar(m_vSpawn2, m_vTarget2);
		if (!pVehicle2) {
			CWorld::Remove(pVehicle1);
			delete pVehicle1;
			return;
		}
		pVehicle1->SetStatus(STATUS_PHYSICS);
		pVehicle1->AutoPilot.m_fMaxTrafficSpeed = pVehicle1->AutoPilot.m_nCruiseSpeed = 4;
		pVehicle1->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_SLOW_DOWN_FOR_CARS;
		pVehicle1->AutoPilot.m_nCarMission = MISSION_SLOWLY_DRIVE_TOWARDS_PLAYER_1;
		pVehicle1->AutoPilot.m_vecDestinationCoors.x = m_vTarget1.x;
		pVehicle1->AutoPilot.m_vecDestinationCoors.y = m_vTarget1.y;
		pVehicle1->AutoPilot.m_vecDestinationCoors.z = 0.0f;
		pVehicle1->m_nRemovalProtectedUntil = CTimer::GetTimeInMilliseconds() + 25000;
		CCarAI::AddPoliceCarOccupants(pVehicle1);
		pVehicle2->SetStatus(STATUS_PHYSICS);
		pVehicle2->AutoPilot.m_fMaxTrafficSpeed = pVehicle2->AutoPilot.m_nCruiseSpeed = 4;
		pVehicle2->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_SLOW_DOWN_FOR_CARS;
		pVehicle2->AutoPilot.m_nCarMission = MISSION_SLOWLY_DRIVE_TOWARDS_PLAYER_1;
		pVehicle2->AutoPilot.m_vecDestinationCoors.x = m_vTarget2.x;
		pVehicle2->AutoPilot.m_vecDestinationCoors.y = m_vTarget2.y;
		pVehicle2->AutoPilot.m_vecDestinationCoors.z = 0.0f;
		pVehicle2->m_nRemovalProtectedUntil = CTimer::GetTimeInMilliseconds() + 25000;
		CCarAI::AddPoliceCarOccupants(pVehicle2);
		m_nLastTimeCreated = CTimer::GetTimeInMilliseconds();
		break;
	}
	case SETPIECE_CARBLOCKINGPLAYERFROMSIDE:
	{
		if (FindPlayerPed()->m_pWanted->GetWantedLevel() < 2)
			return;
		if (!FindPlayerVehicle())
			return;
		if (DotProduct2D(FindPlayerSpeed(), (CVector2D)FindPlayerCoors() - m_vSpawn1) >= 0.0f)
			return;
		CVehicle* pVehicle1 = TryToGenerateCopCar(m_vSpawn1, m_vTarget1);
		if (!pVehicle1)
			return;
		pVehicle1->SetStatus(STATUS_PHYSICS);
		pVehicle1->AutoPilot.m_fMaxTrafficSpeed = pVehicle1->AutoPilot.m_nCruiseSpeed = 16;
		pVehicle1->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_PLOUGH_THROUGH;
		pVehicle1->AutoPilot.m_nCarMission = MISSION_BLOCKPLAYER_FORWARDANDBACK;
		pVehicle1->AutoPilot.m_nTempAction = TEMPACT_GOFORWARD;
		pVehicle1->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds() + 100;
		pVehicle1->m_nRemovalProtectedUntil = CTimer::GetTimeInMilliseconds() + 10000;
		pVehicle1->SetMoveSpeed(2.0f * pVehicle1->GetForward() / 3.0f);
		CCarAI::AddPoliceCarOccupants(pVehicle1);
		m_nLastTimeCreated = CTimer::GetTimeInMilliseconds();
		return;
	}
	case SETPIECE_CARRAMMINGPLAYERFROMSIDE:
	{
		if (FindPlayerPed()->m_pWanted->GetWantedLevel() < 2)
			return;
		if (!FindPlayerVehicle())
			return;
		if (DotProduct2D(FindPlayerSpeed(), (CVector2D)FindPlayerCoors() - m_vSpawn1) >= 0.0f)
			return;
		CVehicle* pVehicle1 = TryToGenerateCopCar(m_vSpawn1, m_vTarget1);
		if (!pVehicle1)
			return;
		pVehicle1->SetStatus(STATUS_PHYSICS);
		pVehicle1->AutoPilot.m_fMaxTrafficSpeed = pVehicle1->AutoPilot.m_nCruiseSpeed = 16;
		pVehicle1->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
		pVehicle1->AutoPilot.m_nCarMission = MISSION_RAMCAR_CLOSE;
		pVehicle1->AutoPilot.m_nTempAction = TEMPACT_GOFORWARD;
		pVehicle1->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds() + 100;
		pVehicle1->m_nRemovalProtectedUntil = CTimer::GetTimeInMilliseconds() + 10000;
		pVehicle1->SetMoveSpeed(2.0f * pVehicle1->GetForward() / 3.0f);
		CCarAI::AddPoliceCarOccupants(pVehicle1);
		m_nLastTimeCreated = CTimer::GetTimeInMilliseconds();
		return;
	}
	case SETPIECE_CREATECOPPERONFOOT:
	{
		if (FindPlayerPed()->m_pWanted->GetWantedLevel() < 1 || FindPlayerVehicle())
			return;
		CCopPed* pCop = TryToGenerateCopPed(m_vSpawn1);
		if (!pCop)
			return;
		float z = CWorld::FindGroundZForCoord(m_vTarget1.x, m_vTarget1.y);
		pCop->bScriptObjectiveCompleted = false;
		pCop->SetObjective(OBJECTIVE_GOTO_AREA_ON_FOOT, CVector(m_vTarget1.x, m_vTarget1.y, z));
		pCop->m_nExtendedRangeTimer = CTimer::GetTimeInMilliseconds() + 10000;
		m_nLastTimeCreated = CTimer::GetTimeInMilliseconds();
		return;
	}
	case SETPIECE_CREATETWOCOPPERSONFOOT:
	{
		if (FindPlayerPed()->m_pWanted->GetWantedLevel() < 1 || FindPlayerVehicle())
			return;
		CCopPed* pCop = TryToGenerateCopPed(m_vSpawn1);
		if (!pCop)
			return;
		float z = CWorld::FindGroundZForCoord(m_vTarget1.x, m_vTarget1.y);
		pCop->bScriptObjectiveCompleted = false;
		pCop->SetObjective(OBJECTIVE_GOTO_AREA_ON_FOOT, CVector(m_vTarget1.x, m_vTarget1.y, z));
		pCop->m_nExtendedRangeTimer = CTimer::GetTimeInMilliseconds() + 10000;
		CCopPed* pCop2 = TryToGenerateCopPed(m_vSpawn2);
		if (!pCop2) {
			CWorld::Remove(pCop);
			delete pCop;
			return;
		}
		z = CWorld::FindGroundZForCoord(m_vTarget2.x, m_vTarget2.y);
		pCop2->bScriptObjectiveCompleted = false;
		pCop2->SetObjective(OBJECTIVE_GOTO_AREA_ON_FOOT, CVector(m_vTarget2.x, m_vTarget2.y, z));
		pCop2->m_nExtendedRangeTimer = CTimer::GetTimeInMilliseconds() + 10000;
		m_nLastTimeCreated = CTimer::GetTimeInMilliseconds();
		return;
	}
	case SETPIECE_TWOCARSBLOCKINGPLAYERFROMSIDE:
	{
		if (FindPlayerPed()->m_pWanted->GetWantedLevel() < 2)
			return;
		if (!FindPlayerVehicle())
			return;
		if (DotProduct2D(FindPlayerSpeed(), (CVector2D)FindPlayerCoors() - m_vSpawn1) >= 0.0f)
			return;
		CVehicle* pVehicle1 = TryToGenerateCopCar(m_vSpawn1, m_vTarget1);
		if (!pVehicle1)
			return;
		pVehicle1->SetStatus(STATUS_PHYSICS);
		pVehicle1->AutoPilot.m_fMaxTrafficSpeed = pVehicle1->AutoPilot.m_nCruiseSpeed = 16;
		pVehicle1->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_PLOUGH_THROUGH;
		pVehicle1->AutoPilot.m_nCarMission = MISSION_BLOCKPLAYER_FORWARDANDBACK;
		pVehicle1->AutoPilot.m_nTempAction = TEMPACT_GOFORWARD;
		pVehicle1->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds() + 100;
		pVehicle1->m_nRemovalProtectedUntil = CTimer::GetTimeInMilliseconds() + 10000;
		pVehicle1->SetMoveSpeed(2.0f * pVehicle1->GetForward() / 3.0f);
		CCarAI::AddPoliceCarOccupants(pVehicle1);
		CVehicle* pVehicle2 = TryToGenerateCopCar(m_vSpawn2, m_vTarget2);
		if (!pVehicle2) {
			CWorld::Remove(pVehicle1);
			delete pVehicle1;
			return;
		}
		pVehicle2->SetStatus(STATUS_PHYSICS);
		pVehicle2->AutoPilot.m_fMaxTrafficSpeed = pVehicle1->AutoPilot.m_nCruiseSpeed = 16;
		pVehicle2->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_PLOUGH_THROUGH;
		pVehicle2->AutoPilot.m_nCarMission = MISSION_BLOCKPLAYER_FORWARDANDBACK;
		pVehicle2->AutoPilot.m_nTempAction = TEMPACT_GOFORWARD;
		pVehicle2->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds() + 100;
		pVehicle2->m_nRemovalProtectedUntil = CTimer::GetTimeInMilliseconds() + 10000;
		pVehicle2->SetMoveSpeed(2.0f * pVehicle2->GetForward() / 3.0f);
		CCarAI::AddPoliceCarOccupants(pVehicle2);
		m_nLastTimeCreated = CTimer::GetTimeInMilliseconds();
		return;
	}
	case SETPIECE_TWOCARSRAMMINGPLAYERFROMSIDE:
	{
		if (FindPlayerPed()->m_pWanted->GetWantedLevel() < 2)
			return;
		if (!FindPlayerVehicle())
			return;
		if (DotProduct2D(FindPlayerSpeed(), (CVector2D)FindPlayerCoors() - m_vSpawn1) >= 0.0f)
			return;
		CVehicle* pVehicle1 = TryToGenerateCopCar(m_vSpawn1, m_vTarget1);
		if (!pVehicle1)
			return;
		pVehicle1->SetStatus(STATUS_PHYSICS);
		pVehicle1->AutoPilot.m_fMaxTrafficSpeed = pVehicle1->AutoPilot.m_nCruiseSpeed = 16;
		pVehicle1->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
		pVehicle1->AutoPilot.m_nCarMission = MISSION_RAMCAR_CLOSE;
		pVehicle1->AutoPilot.m_nTempAction = TEMPACT_GOFORWARD;
		pVehicle1->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds() + 100;
		pVehicle1->m_nRemovalProtectedUntil = CTimer::GetTimeInMilliseconds() + 10000;
		pVehicle1->SetMoveSpeed(2.0f * pVehicle1->GetForward() / 3.0f);
		CCarAI::AddPoliceCarOccupants(pVehicle1);
		CVehicle* pVehicle2 = TryToGenerateCopCar(m_vSpawn2, m_vTarget2);
		if (!pVehicle2) {
			CWorld::Remove(pVehicle1);
			delete pVehicle1;
			return;
		}
		pVehicle2->SetStatus(STATUS_PHYSICS);
		pVehicle2->AutoPilot.m_fMaxTrafficSpeed = pVehicle2->AutoPilot.m_nCruiseSpeed = 16;
		pVehicle2->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
		pVehicle2->AutoPilot.m_nCarMission = MISSION_RAMCAR_CLOSE;
		pVehicle2->AutoPilot.m_nTempAction = TEMPACT_GOFORWARD;
		pVehicle2->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds() + 100;
		pVehicle2->m_nRemovalProtectedUntil = CTimer::GetTimeInMilliseconds() + 10000;
		pVehicle2->SetMoveSpeed(2.0f * pVehicle2->GetForward() / 3.0f);
		CCarAI::AddPoliceCarOccupants(pVehicle2);
		m_nLastTimeCreated = CTimer::GetTimeInMilliseconds();
		return;
	}
	}
}

CVehicle* CSetPiece::TryToGenerateCopCar(CVector2D vSpawn, CVector2D vTarget)
{
	/* The set piece is a mission-style scenario actor: the ticket names its
	 * purpose MISSION, and it forces because the old path could not fail on a
	 * scheduling decision (only on the collision guard below). The service
	 * owns the constructor, the ground cast (the model's own height above the
	 * road) and the world insert. */
	CColModel *pColModel = CModelInfo::GetColModel(MI_POLICE);
	CVector vDirection(vTarget.x - vSpawn.x, vTarget.y - vSpawn.y, 0.0f);
	CColPoint groundPoint;
	CEntity *groundEntity;
	CVector checkPos(vSpawn.x, vSpawn.y, 1000.0f);
	int16 total;

	/* The old guard ran before the world insert; the service inserts at
	 * once, so the guard runs here at the model's base over the ground the
	 * ground policy will use (the same dummies-inclusive cast). The exact
	 * final height is the constructor's (the suspension), which only the
	 * service can read after it creates. */
	if (CWorld::ProcessVerticalLine(checkPos, -1000.0f, groundPoint, groundEntity,
		true, false, false, false, true, false, nil))
		checkPos.z = groundPoint.point.z;
	checkPos.z -= pColModel->boundingBox.min.z;
	CWorld::FindObjectsKindaColliding(checkPos, pColModel->spheres->radius, false, &total, 16, nil,
		false, true, true, false, false);
	if (total != 0)
		return nil;

	vDirection.Normalise();

	SSpawnExplicitRequest request;
	request.purpose = SPAWN_PURPOSE_MISSION;
	request.force = true;
	request.carModel = MI_POLICE;
	request.x = vSpawn.x;
	request.y = vSpawn.y;
	request.z = 1000.0f;
	request.dirX = vDirection.x;
	request.dirY = vDirection.y;
	request.lifetimeMs = 0;
	request.createdBy = 0;		/* the traffic's tag: AddPoliceCarOccupants needs RANDOM_VEHICLE */
	request.ground = SPAWN_EXPLICIT_GROUND_HEIGHT;
	request.groundProbeZ = 1000.0f;
	request.groundNoHitZ = 1000.0f;
	request.groundDummies = true;
	request.clearSpace = false;

	CVehicle* pVehicle;
	if (!CSpawnService::RequestExplicit(request, &pVehicle))
		return nil;
	pVehicle->ChangeLawEnforcerState(true);
	return pVehicle;
}

CCopPed* CSetPiece::TryToGenerateCopPed(CVector2D vSpawn)
{
	/* A set piece's cop on foot: the same MISSION purpose and force, and the
	 * population's own cop constructor through the ped arm. The old guard
	 * (the collision at the resolved spot) runs before the request because
	 * the service inserts at once; the request carries the exact resolved z,
	 * so the service does not ground it a second time. */
	CColModel *pColModel = CModelInfo::GetColModel(MI_COP);
	CVector pos(vSpawn.x, vSpawn.y, 1000.0f);
	CColPoint point;
	CEntity* pEntity;
	int16 total;

	if (CWorld::ProcessVerticalLine(pos, -1000.0f, point, pEntity, true, false, false, false, true, false, nil))
		pos.z = point.point.z + 0.9f;
	CWorld::FindObjectsKindaColliding(pos, pColModel->spheres->radius, false, &total, 16, nil,
		false, true, true, false, false);
	if (total != 0)
		return nil;

	SSpawnExplicitPedRequest request;
	request.purpose = SPAWN_PURPOSE_MISSION;
	request.force = true;
	request.place = SPAWN_PED_PLACE_STANDING;
	request.pedType = PEDTYPE_COP;
	request.pedModel = COP_STREET;
	request.modifier = 0;
	request.x = pos.x;
	request.y = pos.y;
	request.z = pos.z;
	request.heading = 0.0f;
	request.createdBy = 0;		/* a random character, the old constructor's class */
	request.missionFlags = false;
	request.vehicle = nil;

	CCopPed* pCop;
	CPed* pCreated;
	if (!CSpawnService::RequestExplicitPed(request, &pCreated))
		return nil;
	pCop = (CCopPed*)pCreated;
	return pCop;
}
