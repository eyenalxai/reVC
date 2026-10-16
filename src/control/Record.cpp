#include "common.h"

#include "Record.h"

uint16 CRecordDataForGame::RecordingState;

void CRecordDataForGame::Init(void)
{
	RecordingState = STATE_NONE;
}

void CRecordDataForGame::SaveOrRetrieveDataForThisFrame(void)
{
}

uint8 CRecordDataForChase::Status;

void CRecordDataForChase::Init(void)
{
	Status = STATE_NONE;
}

void CRecordDataForChase::SaveOrRetrieveDataForThisFrame(void)
{
}

void CRecordDataForChase::SaveOrRetrieveCarPositions(void)
{
}

void CRecordDataForChase::ProcessControlCars(void)
{
}

bool CRecordDataForChase::ShouldThisPadBeLeftAlone(uint8 pad)
{
	return false;
}

void CRecordDataForChase::StartChaseScene(float startTime)
{
}

void CRecordDataForChase::CleanUpChaseScene(void)
{
}

void CRecordDataForChase::SetUpCarsForChaseScene(void)
{
}

void CRecordDataForChase::CleanUpCarsForChaseScene(void)
{
}

void CRecordDataForChase::RemoveCarFromChase(int32 i)
{
}

CVehicle* CRecordDataForChase::TurnChaseCarIntoScriptCar(int32 i)
{
	return nil;
}

