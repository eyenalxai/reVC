#include "common.h"

#include "Bones.h"
#include "AnimBlendHierarchy.h"
#include "AnimBlendClumpData.h"
#include "RpAnimBlend.h"
#include "AnimManager.h"
#include "AnimBlendAssociation.h"
#include "MemoryMgr.h"

CAnimBlendAssociation::CAnimBlendAssociation(void)
{
	groupId = -1;
	nodes = nil;
	blendAmount = 1.0f;
	blendDelta = 0.0f;
	currentTime = 0.0f;
	speed = 1.0f;
	timeStep = 0.0f;
	animId = -1;
	flags = 0;
	callbackType = CB_NONE;
	link.Init();
}

CAnimBlendAssociation::CAnimBlendAssociation(CAnimBlendAssociation &other)
{
	nodes = nil;
	blendAmount = 1.0f;
	blendDelta = 0.0f;
	currentTime = 0.0f;
	speed = 1.0f;
	timeStep = 0.0f;
	callbackType = CB_NONE;
	link.Init();
	Init(other);
}

CAnimBlendAssociation::~CAnimBlendAssociation(void)
{
	FreeAnimBlendNodeArray();
	link.Remove();
}


void
CAnimBlendAssociation::AllocateAnimBlendNodeArray(int n)
{
	int i;

	nodes = (CAnimBlendNode*)RwMallocAlign(n*sizeof(CAnimBlendNode), 64);
	for(i = 0; i < n; i++)
		nodes[i].Init();
}

void
CAnimBlendAssociation::FreeAnimBlendNodeArray(void)
{
	if(nodes)
		RwFreeAlign(nodes);
}

void
CAnimBlendAssociation::Init(RpClump *clump, CAnimBlendHierarchy *hier)
{
	int i;
	AnimBlendFrameData *frame;

	CAnimBlendClumpData *clumpData = *RPANIMBLENDCLUMPDATA(clump);
	numNodes = clumpData->numFrames;
	AllocateAnimBlendNodeArray(numNodes);
	for(i = 0; i < numNodes; i++)
		nodes[i].association = this;
	hierarchy = hier;

	// Init every node from a sequence and a Clump frame
	// NB: This is where the order of nodes is defined
	for(i = 0; i < hier->numSequences; i++){
		CAnimBlendSequence *seq = &hier->sequences[i];
		if(seq->boneTag == -1)
			frame = RpAnimBlendClumpFindFrame(clump, seq->name);
		else
			frame = RpAnimBlendClumpFindBone(clump, seq->boneTag);
		if(frame && seq->numFrames > 0)
			nodes[frame - clumpData->frames].sequence = seq;
	}
}

void
CAnimBlendAssociation::Init(CAnimBlendAssociation &assoc)
{
	int i;

	hierarchy = assoc.hierarchy;
	numNodes = assoc.numNodes;
	flags = assoc.flags;
	animId = assoc.animId;
	groupId = assoc.groupId;
	AllocateAnimBlendNodeArray(numNodes);
	for(i = 0; i < numNodes; i++){
		nodes[i] = assoc.nodes[i];
		nodes[i].association = this;
	}
}

void
CAnimBlendAssociation::SetBlend(float amount, float delta)
{
	blendAmount = amount;
	blendDelta = delta;
}

// Whether a sequence drives one of the 8 lower body bones. Everything else,
// a name-matched sequence (boneTag < 0) included, stays: the default keeps
// the bones this list does not know, which is the safe side for the removal.
static bool
IsLowerBodyBone(int16 boneTag)
{
	if(boneTag < 0)
		return false;
	switch(boneTag){
	case BONE_root:
	case BONE_pelvis:
	case BONE_l_thigh:
	case BONE_l_calf:
	case BONE_l_foot:
	case BONE_r_thigh:
	case BONE_r_calf:
	case BONE_r_foot:
		return true;
	default:
		return false;
	}
}

// Keep only the sequences that drive the upper body. A partial association
// whose data covers the whole body otherwise overrides the legs of the
// movement animation it layers over; dropping the lower body keys leaves the
// legs to that animation (VICE-141). A seated rider also keeps the spine of
// the seat pose: the standing aim pose bends the spine away from the bike seat
// (VICE-141 follow-up).
void
CAnimBlendAssociation::MakeUpperBodyOnly(bool seated)
{
	int i;
	for(i = 0; i < numNodes; i++)
		if(nodes[i].sequence && (IsLowerBodyBone(nodes[i].sequence->boneTag) || (seated && nodes[i].sequence->boneTag == BONE_spine)))
			nodes[i].sequence = nil;
}

void
CAnimBlendAssociation::SetFinishCallback(void (*cb)(CAnimBlendAssociation*, void*), void *arg)
{
	callbackType = CB_FINISH;
	callback = cb;
	callbackArg = arg;
}

void
CAnimBlendAssociation::SetDeleteCallback(void (*cb)(CAnimBlendAssociation*, void*), void *arg)
{
	callbackType = CB_DELETE;
	callback = cb;
	callbackArg = arg;
}

void
CAnimBlendAssociation::SetCurrentTime(float time)
{
	int i;

	for(currentTime = time; currentTime >= hierarchy->totalLength; currentTime -= hierarchy->totalLength)
		if (!IsRepeating()) {
			currentTime = hierarchy->totalLength;
			break;
		}

	CAnimManager::UncompressAnimation(hierarchy);
#ifdef ANIM_COMPRESSION
	// strangely PC has this but android doesn't
	if(hierarchy->keepCompressed){
		for(i = 0; i < numNodes; i++)
			if(nodes[i].sequence)
				nodes[i].SetupKeyFrameCompressed();
	}else
#endif
	{
		for(i = 0; i < numNodes; i++)
			if(nodes[i].sequence)
				nodes[i].FindKeyFrame(currentTime);
	}
}

void
CAnimBlendAssociation::SyncAnimation(CAnimBlendAssociation *other)
{
	SetCurrentTime(other->currentTime/other->hierarchy->totalLength * hierarchy->totalLength);
}

void
CAnimBlendAssociation::Start(float time)
{
	flags |= ASSOC_RUNNING;
	SetCurrentTime(time);
}

void
CAnimBlendAssociation::UpdateTimeStep(float timeDelta, float relSpeed)
{
	if(IsRunning())
		timeStep = (flags & ASSOC_MOVEMENT ? relSpeed*hierarchy->totalLength : speed) * timeDelta;
}

bool
CAnimBlendAssociation::UpdateTime(float timeDelta, float relSpeed)
{
	if(!IsRunning())
		return true;
	if(currentTime >= hierarchy->totalLength){
		flags &= ~ASSOC_RUNNING;
		return true;
	}

	currentTime += timeStep;

	if(currentTime >= hierarchy->totalLength){
		// Ran past end

		if(IsRepeating())
			currentTime -= hierarchy->totalLength;
		else{
			currentTime = hierarchy->totalLength;
			if(flags & ASSOC_FADEOUTWHENDONE){
				flags |= ASSOC_DELETEFADEDOUT;
				blendDelta = -4.0f;
			}
			if(callbackType == CB_FINISH){
				callbackType = CB_NONE;
				callback(this, callbackArg);
			}
		}
	}
	return true;
}

// return whether we still exist after this function
bool
CAnimBlendAssociation::UpdateBlend(float timeDelta)
{
	blendAmount += blendDelta * timeDelta;

	if(blendAmount <= 0.0f && blendDelta < 0.0f){
		// We're faded out and are not fading in
		blendAmount = 0.0f;
		blendDelta = Max(0.0f, blendDelta);
		if(flags & ASSOC_DELETEFADEDOUT){
			if(callbackType == CB_FINISH || callbackType == CB_DELETE)
				callback(this, callbackArg);
			delete this;
			return false;
		}
	}

	if(blendAmount > 1.0f){
		// Maximally faded in, clamp values
		blendAmount = 1.0f;
		blendDelta = Min(0.0f, blendDelta);
	}

	return true;
}
