#include "common.h"

#include "CarGen.h"

#include "Camera.h"
#include "CutsceneMgr.h"
#include "ModelInfo.h"
#include "Occlusion.h"
#include "Pools.h"
#include "SpawnService.h"
#include "Timer.h"
#include "TrafficRings.h"
#include "Vehicle.h"
#include "World.h"
#include "SaveBuf.h"

/*
 * The car generators (VICE-155). This file is the generator's own policy: when
 * it asks (the round-robin of CTheCarGenerators::Process, the timer and the
 * range gate of CCarGenerator::Process and CheckIfWithinRangeOfAnyPlayers),
 * the uses, the handle, the blocking flag, and the state the save file carries
 * (CTheCarGenerators::SaveAllCarGenerators). The placement - the parked target
 * gate, the model policy, the spot's validity (the blockage, the ground, the
 * near bubble without rays) and the creation with its setup - is the spawn
 * service's parked kind (SpawnService.h and .cpp). Every
 * CCarGenerator::DoInternalProcessing is one CSpawnService::RequestParked
 * call; no placement decision and no dice is left here. The generators the
 * scripts create and the save/load rebuild are this file's own and unchanged.
 */

uint8 CTheCarGenerators::ProcessCounter;
uint32 CTheCarGenerators::NumOfCarGenerators;
CCarGenerator CTheCarGenerators::CarGeneratorArray[NUM_CARGENS];
uint8 CTheCarGenerators::GenerateEvenIfPlayerIsCloseCounter;
uint32 CTheCarGenerators::CurrentActiveCount;

void CCarGenerator::SwitchOff()
{
#ifdef FIX_BUGS
	if (m_nUsesRemaining != 0)
#endif
	{
		m_nUsesRemaining = 0;
		--CTheCarGenerators::CurrentActiveCount;
	}
}

void CCarGenerator::SwitchOn()
{
	m_nUsesRemaining = UINT16_MAX;
	m_nTimer = CalcNextGen();
	++CTheCarGenerators::CurrentActiveCount;
}

uint32 CCarGenerator::CalcNextGen()
{
	return CTimer::GetTimeInMilliseconds() + 4;
}

void CCarGenerator::DoInternalProcessing()
{
	SSpawnParkedRequest request;
	SSpawnParkedDecision decision;
	CVehicle *pVehicle = nil;
	ESpawnRefusal refusal;

	/* The parked request (VICE-155): this generator's position, model state
	 * and setup policy, through the service's parked path. The target gate,
	 * the model draw, the spot's validity and the creation are the service's;
	 * the refusal comes back by reason, because two of them are this
	 * generator's policy (the blockage's flag and timer below). */
	request.purpose = SPAWN_PURPOSE_PARKED;
	request.modelIndex = m_nModelIndex;
	request.x = m_vecPos.x;
	request.y = m_vecPos.y;
	request.z = m_vecPos.z;
	request.angle = m_fAngle;
	request.color1 = m_nColor1;
	request.color2 = m_nColor2;
	request.alarm = m_nAlarm;
	request.doorlock = m_nDoorlock;

	refusal = CSpawnService::RequestParked(request, &decision, &pVehicle);

	/* The dynamic model policy's own state: a re-choice stores the new field
	 * and resets the colours (the service made the draw). */
	if (decision.modelRechosen) {
		m_nModelIndex = decision.modelIndex;
		m_nColor1 = -1;
		m_nColor2 = -1;
	}

	if (refusal == SPAWN_REFUSAL_OCCUPANCY) {
		/* The spot is blocked: the generator waits it out. The flag holds
		 * the spawns until the player leaves the ring (the range gate
		 * clears it) and the timer moves on, the old behavior. */
		m_bIsBlocking = true;
		m_nTimer += 4;
		return;
	}
	if (refusal != SPAWN_REFUSAL_NONE)
		return;

	/* The dynamic generator remembers the colours of its own first car. */
	if ((m_nColor1 == -1 || m_nColor2 == -1) && m_nModelIndex < -1) {
		m_nColor1 = pVehicle->m_currentColour1;
		m_nColor2 = pVehicle->m_currentColour2;
	}
	m_nVehicleHandle = CPools::GetVehiclePool()->GetIndex(pVehicle);
	/* I don't think this is a correct comparasion */
#ifdef FIX_BUGS
	if (m_nUsesRemaining < UINT16_MAX)
#else
	if (m_nUsesRemaining < ~0)
#endif
		--m_nUsesRemaining;
	m_nTimer = CalcNextGen();
	if (m_nUsesRemaining == 0)
		--CTheCarGenerators::CurrentActiveCount;
}

void CCarGenerator::Process()
{
	if (m_nVehicleHandle == -1 && 
		(CTheCarGenerators::GenerateEvenIfPlayerIsCloseCounter || CTimer::GetTimeInMilliseconds() >= m_nTimer) &&
		m_nUsesRemaining != 0 && CheckIfWithinRangeOfAnyPlayers())
		DoInternalProcessing();
	if (m_nVehicleHandle == -1)
		return;
	CVehicle* pVehicle = CPools::GetVehiclePool()->GetAt(m_nVehicleHandle);
	if (!pVehicle){
		m_nVehicleHandle = -1;
		return;
	}
	if (pVehicle->GetStatus() != STATUS_PLAYER)
		return;
	m_nTimer += 60000;
	m_nVehicleHandle = -1;
	m_bIsBlocking = true;
	pVehicle->bExtendedRange = false;
	if (m_nModelIndex < 0)
		m_nModelIndex = -1;
}

void CCarGenerator::Setup(float x, float y, float z, float angle, int32 mi, int16 color1, int16 color2, uint8 force, uint8 alarm, uint8 lock, uint16 min_delay, uint16 max_delay)
{
	CMatrix m1, m2, m3; /* Unused but present on stack, so I'll leave them. */
	m_vecPos = CVector(x, y, z);
	m_fAngle = angle;
	m_nModelIndex = mi;
	m_nColor1 = color1;
	m_nColor2 = color2;
	m_bForceSpawn = force;
	m_nAlarm = alarm;
	m_nDoorlock = lock;
	m_nMinDelay = min_delay;
	m_nMaxDelay = max_delay;
	m_nVehicleHandle = -1;
	m_nTimer = CTimer::GetTimeInMilliseconds() + 1;
	m_nUsesRemaining = 0;
	m_bIsBlocking = false;
}

bool CCarGenerator::CheckIfWithinRangeOfAnyPlayers()
{
	CVector2D direction = FindPlayerCentreOfWorld(CWorld::PlayerInFocus) - m_vecPos;
	float distance = direction.Magnitude();
	/* The generator ring comes from the one table: it holds its car between the
	 * near and the far ring, and it keeps a visible boat car up to the boat
	 * ring. The whole ring follows the spawn distance, the FOV factor and the
	 * visible range, exactly like the random traffic. */
	const STrafficRings rings = CurrentTrafficRings();
	float farclip = rings.generatorFar;
	float nearclip = rings.generatorNear;
	bool canBeRemoved = (m_nModelIndex > 0 && CModelInfo::IsBoatModel(m_nModelIndex) && rings.generatorBoatFar > distance &&
		TheCamera.IsSphereVisible(m_vecPos, 0.0f) && !COcclusion::IsPositionOccluded(m_vecPos, 0.0f)); 
	if (distance >= farclip && !canBeRemoved){
		if (m_bIsBlocking)
			m_bIsBlocking = false;
		return false;
	}
	if (CTheCarGenerators::GenerateEvenIfPlayerIsCloseCounter)
		return !TheCamera.IsSphereVisible(m_vecPos, 0.0f);
	if (m_bIsBlocking)
		return false;
	if (distance < nearclip && !m_bForceSpawn)
		return false;
	return DotProduct2D(direction, FindPlayerSpeed()) <= 0;
}

void CTheCarGenerators::Process()
{
	if (FindPlayerTrain() || CCutsceneMgr::IsCutsceneProcessing())
		return;
	if (++CTheCarGenerators::ProcessCounter == 4)
		CTheCarGenerators::ProcessCounter = 0;
	for (uint32 i = ProcessCounter; i < NumOfCarGenerators; i += 4)
		CTheCarGenerators::CarGeneratorArray[i].Process();
	if (GenerateEvenIfPlayerIsCloseCounter)
		GenerateEvenIfPlayerIsCloseCounter--;
}

int32 CTheCarGenerators::CreateCarGenerator(float x, float y, float z, float angle, int32 mi, int16 color1, int16 color2, uint8 force, uint8 alarm, uint8 lock, uint16 min_delay, uint16 max_delay)
{
	if (NumOfCarGenerators < NUM_CARGENS)
		CarGeneratorArray[NumOfCarGenerators++].Setup(x, y, z, angle, mi, color1, color2, force, alarm, lock, min_delay, max_delay);
	return NumOfCarGenerators - 1;
}

void CTheCarGenerators::Init()
{
	GenerateEvenIfPlayerIsCloseCounter = 0;
	NumOfCarGenerators = 0;
	ProcessCounter = 0;
	CurrentActiveCount = 0;
}

void CTheCarGenerators::SaveAllCarGenerators(uint8 *buffer, uint32 *size)
{
	const uint32 nGeneralDataSize = sizeof(NumOfCarGenerators) + sizeof(CurrentActiveCount) + sizeof(ProcessCounter) + sizeof(GenerateEvenIfPlayerIsCloseCounter) + sizeof(int16);
	*size = sizeof(int) + nGeneralDataSize + sizeof(uint32) + sizeof(CarGeneratorArray) + SAVE_HEADER_SIZE;
INITSAVEBUF
	WriteSaveHeader(buffer, 'C','G','N','\0', *size - SAVE_HEADER_SIZE);

	WriteSaveBuf(buffer, nGeneralDataSize);
	WriteSaveBuf(buffer, NumOfCarGenerators);
	WriteSaveBuf(buffer, CurrentActiveCount);
	WriteSaveBuf(buffer, ProcessCounter);
	WriteSaveBuf(buffer, GenerateEvenIfPlayerIsCloseCounter);
	WriteSaveBuf(buffer, (int16)0); // alignment
	WriteSaveBuf(buffer, (uint32)sizeof(CarGeneratorArray));
	for (int i = 0; i < NUM_CARGENS; i++)
		WriteSaveBuf(buffer, CarGeneratorArray[i]);
VALIDATESAVEBUF(*size)
}

void CTheCarGenerators::LoadAllCarGenerators(uint8* buffer, uint32 size)
{
	NumOfCarGenerators = 0;
	GenerateEvenIfPlayerIsCloseCounter = 0;
	CurrentActiveCount = 0;
	ProcessCounter = 0;

	const int32 nGeneralDataSize = sizeof(NumOfCarGenerators) + sizeof(CurrentActiveCount) + sizeof(ProcessCounter) + sizeof(GenerateEvenIfPlayerIsCloseCounter) + sizeof(int16);
	Init();
INITSAVEBUF
	CheckSaveHeader(buffer, 'C','G','N','\0', size - SAVE_HEADER_SIZE);
	uint32 tmp;
	ReadSaveBuf(&tmp, buffer);
	assert(tmp == nGeneralDataSize);
	ReadSaveBuf(&NumOfCarGenerators, buffer);
	ReadSaveBuf(&CurrentActiveCount, buffer);
	ReadSaveBuf(&ProcessCounter, buffer);
	ReadSaveBuf(&GenerateEvenIfPlayerIsCloseCounter, buffer);
	SkipSaveBuf(buffer, 2);
	ReadSaveBuf(&tmp, buffer);
	assert(tmp == sizeof(CarGeneratorArray));
	for (int i = 0; i < NUM_CARGENS; i++) 
		ReadSaveBuf(&CarGeneratorArray[i], buffer);
VALIDATESAVEBUF(size)
}
