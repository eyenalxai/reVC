#ifndef __GTA_RECORD_H__
#define __GTA_RECORD_H__

class CVehicle;

extern char gString[256];

class CRecordDataForChase
{
	enum {
		STATE_NONE = 0,
		STATE_RECORD = 1
	};
	static uint8 Status;
public:

	static bool IsRecording(void) { return Status == STATE_RECORD; }

	static void Init(void);
	static void SaveOrRetrieveDataForThisFrame(void);
	static void SaveOrRetrieveCarPositions(void);
	static void ProcessControlCars(void);
	static bool ShouldThisPadBeLeftAlone(uint8 pad);
	static void StartChaseScene(float);
	static void CleanUpChaseScene(void);
	static void SetUpCarsForChaseScene(void);
	static void CleanUpCarsForChaseScene(void);
	static void RemoveCarFromChase(int32);
	static CVehicle* TurnChaseCarIntoScriptCar(int32);

};

class CRecordDataForGame
{
	enum {
		STATE_NONE = 0,
		STATE_RECORD = 1,
		STATE_PLAYBACK = 2,
	};
	static uint16 RecordingState;

public:
	static bool IsRecording() { return RecordingState == STATE_RECORD; }
	static bool IsPlayingBack() { return RecordingState == STATE_PLAYBACK; }

	static void SaveOrRetrieveDataForThisFrame(void);
	static void Init(void);
};

#endif // __GTA_RECORD_H__
