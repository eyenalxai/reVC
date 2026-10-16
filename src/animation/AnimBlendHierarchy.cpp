#include "common.h"

#include "AnimBlendSequence.h"
#include "AnimBlendHierarchy.h"
#include "AnimManager.h"

CAnimBlendHierarchy::CAnimBlendHierarchy(void)
{
	sequences = nil;
	numSequences = 0;
	compressed = 0;
	totalLength = 0.0f;
	rootSpeed = 0.0f;
	linkPtr = nil;
}

void
CAnimBlendHierarchy::Shutdown(void)
{
	CAnimManager::RemoveFromUncompressedCache(this);
	RemoveAnimSequences();
	totalLength = 0.0f;
	rootSpeed = 0.0f;
	compressed = 0;
}

void
CAnimBlendHierarchy::SetName(char *name)
{
	strncpy(this->name, name, 24);
}

// The engine binds a sequence to the root bone by its bone tag; when the file
// has no tags (the KRTS animations) it binds by the name. The root sequence
// carries the ground motion of the animation.
static bool
IsRootSequence(CAnimBlendSequence *seq)
{
	if(seq->boneTag == 0)
		return true;
	return seq->boneTag == -1 && strcmp(seq->name, "Root") == 0;
}

void
CAnimBlendHierarchy::CalcTotalTime(void)
{
	int i, j;

	totalLength = 0.0f;
	rootSpeed = 0.0f;

	for(i = 0; i < numSequences; i++){
#ifdef FIX_BUGS
		if(sequences[i].numFrames == 0)
			continue;
#endif

		// The ground speed of the animation: the net displacement of the root
		// over one cycle, in units per second. The engine moves the ped by this
		// much per second with association speed 1.0.
		if(IsRootSequence(&sequences[i]) && sequences[i].HasTranslation()){
			KeyFrameTrans *first = (KeyFrameTrans*)sequences[i].GetKeyFrame(0);
			KeyFrameTrans *last = (KeyFrameTrans*)sequences[i].GetKeyFrame(sequences[i].numFrames-1);
			float duration = last->deltaTime - first->deltaTime;
			if(duration > 0.0f)
				rootSpeed = (last->translation - first->translation).Magnitude2D() / duration;
		}

		totalLength = Max(totalLength, sequences[i].GetKeyFrame(sequences[i].numFrames-1)->deltaTime);
		for(j = sequences[i].numFrames-1; j >= 1; j--){
			KeyFrame *kf1 = sequences[i].GetKeyFrame(j);
			KeyFrame *kf2 = sequences[i].GetKeyFrame(j-1);
			kf1->deltaTime -= kf2->deltaTime;
		}
	}
}

void
CAnimBlendHierarchy::CalcTotalTimeCompressed(void)
{
	int i, j;

	totalLength = 0.0f;
	rootSpeed = 0.0f;

	for(i = 0; i < numSequences; i++){
#ifdef FIX_BUGS
		if(sequences[i].numFrames == 0)
			continue;
#endif

		if(IsRootSequence(&sequences[i]) && sequences[i].HasTranslation()){
			KeyFrameTransCompressed *first = (KeyFrameTransCompressed*)sequences[i].GetKeyFrameCompressed(0);
			KeyFrameTransCompressed *last = (KeyFrameTransCompressed*)sequences[i].GetKeyFrameCompressed(sequences[i].numFrames-1);
			CVector firstPos, lastPos;
			first->GetTranslation(&firstPos);
			last->GetTranslation(&lastPos);
			float duration = last->GetDeltaTime() - first->GetDeltaTime();
			if(duration > 0.0f)
				rootSpeed = (lastPos - firstPos).Magnitude2D() / duration;
		}

		totalLength = Max(totalLength, sequences[i].GetKeyFrameCompressed(sequences[i].numFrames-1)->GetDeltaTime());
		for(j = sequences[i].numFrames-1; j >= 1; j--){
			KeyFrameCompressed *kf1 = sequences[i].GetKeyFrameCompressed(j);
			KeyFrameCompressed *kf2 = sequences[i].GetKeyFrameCompressed(j-1);
			kf1->deltaTime -= kf2->deltaTime;
		}
	}
}

void
CAnimBlendHierarchy::RemoveQuaternionFlips(void)
{
	int i;

	for(i = 0; i < numSequences; i++)
		sequences[i].RemoveQuaternionFlips();
}

void
CAnimBlendHierarchy::RemoveAnimSequences(void)
{
	delete[] sequences;
	sequences = nil;
	numSequences = 0;
}

void
CAnimBlendHierarchy::Uncompress(void)
{
#ifdef ANIM_COMPRESSION
	int i;
	assert(compressed);
	for(i = 0; i < numSequences; i++)
		sequences[i].Uncompress();
#endif
	compressed = 0;
	if(totalLength == 0.0f){
		RemoveQuaternionFlips();
		CalcTotalTime();
	}
}

void
CAnimBlendHierarchy::RemoveUncompressedData(void)
{
#ifdef ANIM_COMPRESSION
	int i;
	assert(!compressed);
	for(i = 0; i < numSequences; i++)
		sequences[i].RemoveUncompressedData();
#endif
	compressed = 1;
}

#ifdef USE_CUSTOM_ALLOCATOR
void
CAnimBlendHierarchy::MoveMemory(bool onlyone)
{
	int i;
	for(i = 0; i < numSequences; i++)
		if(sequences[i].MoveMemory() && onlyone)
			return;
}
#endif
