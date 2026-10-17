// The IFP validator (tools/ifp/ifp.h). Every rule here is a way the file can
// break the loader or the movement content that reads it; the parser catches
// the structural defects and this file catches the content ones.

#include "ifp.h"

#include <math.h>

namespace ifp {

// The movement slots in file order, the engine's AnimationId 0..4 of
// aStdAnimDescs (AnimManager.cpp): walk, run, sprint, idle, startwalk. A
// movement block must keep its animations in that order; the name array and the
// description array in the engine's table are index-aligned by construction,
// so an out-of-file-order block silently gives a slot the wrong behaviour.
const MovementSlot MovementSlots[] = {
	{ "walk",      true,  true  },
	{ "run",       true,  true  },
	{ "sprint",    true,  true  },
	{ "idle",      false, false },
	{ "startwalk", true,  false },
};
const int NumMovementSlots = 5;

// The loader's name fields, from CAnimManager (strncpy limits) and
// CAnimBlendSequence::SetName.
static const size_t SequenceNameLimit = 24;
static const size_t BlockNameLimit = 20;	// MAX_ANIMBLOCK_NAME

// How far sideways or vertically a repeating cycle may drift over one loop,
// against the length of the stride it covers. The shipped walk cycles stay
// within a fraction of a percent; five percent catches a real drift and leaves
// room for an authored cycle that sways a little.
static const float CycleDriftTolerance = 0.05f;

static void
Problem(std::vector<std::string> *problems, const std::string &what)
{
	problems->push_back(what);
}

static bool
IsFinite(float v)
{
	return isfinite(v) != 0;
}

static void
CheckKeys(const Hierarchy &hier, const Sequence &seq, std::vector<std::string> *problems)
{
	std::string where = "animation \"" + hier.name + "\" sequence \"" + seq.name + "\"";

	if(seq.name.size() >= SequenceNameLimit)
		Problem(problems, Format("%s: the name is %d bytes; the loader copies at most %d with strncpy "
			"and needs a NUL", where.c_str(), (int)seq.name.size(), (int)SequenceNameLimit));
	if(seq.tagged && (seq.boneTag < -32768 || seq.boneTag > 32767))
		Problem(problems, Format("%s: the bone tag %d does not fit the loader's int16", where.c_str(),
			seq.boneTag));
	if((int32)seq.keys.size() != seq.numFrames)
		Problem(problems, Format("%s: the ANIM says %d frames and the keyframe chunk holds %d keys",
			where.c_str(), (int)seq.numFrames, (int)seq.keys.size()));
	if(seq.keys.empty() && seq.ident != KEYS_NONE)
		Problem(problems, Format("%s: a %s chunk on a sequence with 0 frames; the loader skips the "
			"sequence before the header read and misreads the file", where.c_str(),
			KeyIdentName(seq.ident)));
	if(!seq.keys.empty() && seq.ident == KEYS_NONE)
		Problem(problems, Format("%s: %d keys and no keyframe chunk identity", where.c_str(),
			(int)seq.keys.size()));

	for(size_t k = 0; k < seq.keys.size(); k++){
		const Key &key = seq.keys[k];
		if(!IsFinite(key.time) || !IsFinite(key.rotation[0]) || !IsFinite(key.rotation[1]) ||
		   !IsFinite(key.rotation[2]) || !IsFinite(key.rotation[3]))
			Problem(problems, Format("%s key %d: a value is not a number", where.c_str(), (int)k));
		if(k > 0 && !(key.time > seq.keys[k-1].time))
			Problem(problems, Format("%s key %d: the time %.6g does not increase from %.6g; the loader "
				"turns the times into deltas and counts time forwards", where.c_str(), (int)k,
				key.time, seq.keys[k-1].time));
		if(k == 0 && key.time < 0.0f)
			Problem(problems, Format("%s key 0: the time is %.6g", where.c_str(), key.time));
		float norm = sqrtf(key.rotation[0]*key.rotation[0] + key.rotation[1]*key.rotation[1] +
			key.rotation[2]*key.rotation[2] + key.rotation[3]*key.rotation[3]);
		if(!IsFinite(norm) || fabsf(norm - 1.0f) > 1e-3f)
			Problem(problems, Format("%s key %d: the rotation has length %.6g, not a unit quaternion "
				"(the file stores its conjugate and the loader negates x, y, z)", where.c_str(),
				(int)k, norm));
		if(seq.HasTranslation()){
			for(int i = 0; i < 3; i++)
				if(!IsFinite(key.translation[i]))
					Problem(problems, Format("%s key %d: the translation is not a number",
						where.c_str(), (int)k));
		}
		if(seq.ident == KEYS_KRTS){
			for(int i = 0; i < 3; i++)
				if(!IsFinite(key.scale[i]))
					Problem(problems, Format("%s key %d: the scale is not a number", where.c_str(), (int)k));
		}
	}
}

// The root sequence carries ground motion. The loader finds it by tag 0 or, for
// an untagged sequence, by the exact name "Root"; anything else is invisible
// ground motion.
static const Sequence *
FindRoot(const Hierarchy &hier, int *index)
{
	for(size_t i = 0; i < hier.sequences.size(); i++){
		if(IsRootSequence(hier.sequences[i])){
			if(index)
				*index = (int)i;
			return &hier.sequences[i];
		}
	}
	if(index)
		*index = -1;
	return 0;
}

static void
CheckMovement(const Block &block, std::vector<std::string> *problems)
{
	if((int)block.hierarchies.size() < NumMovementSlots){
		Problem(problems, Format("movement set: the block has %d animations, the movement slots need %d "
			"(walk, run, sprint, idle, startwalk in that order)", (int)block.hierarchies.size(),
			NumMovementSlots));
		return;
	}

	float orderedSpeeds[3] = { 0.0f, 0.0f, 0.0f };
	bool ordered = true;
	for(int i = 0; i < NumMovementSlots; i++){
		const MovementSlot &slot = MovementSlots[i];
		const Hierarchy &hier = block.hierarchies[i];
		std::string where = Format("movement slot %d (%s) \"%s\"", i, slot.role, hier.name.c_str());
		int rootIndex = -1;
		const Sequence *root = FindRoot(hier, &rootIndex);
		if(root == 0){
			Problem(problems, where + ": no root sequence: no sequence is tagged 0 and none is an "
				"untagged \"Root\", so the slot has no ground motion");
			if(i < 3)
				ordered = false;
			continue;
		}
		if(slot.groundMotion && !root->HasTranslation()){
			Problem(problems, Format("%s: the root \"%s\" has no translation keys (%s), so the cycle "
				"cannot move the ped", where.c_str(), root->name.c_str(), KeyIdentName(root->ident)));
			if(i < 3)
				ordered = false;
			continue;
		}
		if(slot.groundMotion && root->keys.size() < 2){
			Problem(problems, Format("%s: the root \"%s\" has %d frames, a cycle needs at least 2",
				where.c_str(), root->name.c_str(), (int)root->keys.size()));
			if(i < 3)
				ordered = false;
			continue;
		}
		float duration = root->keys.back().time - root->keys.front().time;
		if(duration <= 0.0f){
			Problem(problems, Format("%s: the root cycle lasts %.6g seconds", where.c_str(), duration));
			if(i < 3)
				ordered = false;
			continue;
		}
		if(root->keys.front().time != 0.0f)
			Problem(problems, Format("%s: the first root key is at %.6g, a cycle starts at 0",
				where.c_str(), root->keys.front().time));
		float speed = RootCycleSpeed(*root);
		if(slot.groundMotion && speed <= 0.0f){
			Problem(problems, Format("%s: rootSpeed is %s: the root cycle does not move the ped "
				"anywhere", where.c_str(), speed == 0.0f ? "0" : "negative"));
			if(i < 3)
				ordered = false;
			continue;
		}
		if(!slot.groundMotion && speed != 0.0f){
			Problem(problems, Format("%s: rootSpeed is %.6g; the idle is a pose and must not "
				"translate", where.c_str(), speed));
			continue;
		}
		if(slot.ordered){
			const Key &first = root->keys.front();
			const Key &last = root->keys.back();
			float dx = last.translation[0] - first.translation[0];
			float dy = last.translation[1] - first.translation[1];
			float dz = last.translation[2] - first.translation[2];
			float stride = sqrtf(dx*dx + dy*dy);
			float sideways = fabsf(dx) < fabsf(dy) ? fabsf(dx) : fabsf(dy);
			if(stride <= 0.0f || sideways > CycleDriftTolerance * stride)
				Problem(problems, Format("%s: the root cycle does not close: one loop drifts (%.4g, "
					"%.4g) sideways and %.4g vertically over a %.4g stride", where.c_str(), dx, dy, dz,
					stride));
			else if(fabsf(dz) > CycleDriftTolerance * stride)
				Problem(problems, Format("%s: the root cycle does not close: one loop drifts %.4g "
					"vertically over a %.4g stride", where.c_str(), dz, stride));
		}
		if(i < 3)
			orderedSpeeds[i] = speed;
	}

	if(ordered && !(orderedSpeeds[0] < orderedSpeeds[1] && orderedSpeeds[1] < orderedSpeeds[2]))
		Problem(problems, Format("movement set: the walk / run / sprint slots are out of order: "
			"rootSpeed is %.4g, %.4g, %.4g and the ladder must increase", orderedSpeeds[0],
			orderedSpeeds[1], orderedSpeeds[2]));
}

void
CheckBlock(const Block &block, const std::string &stem, const CheckOptions &options,
	std::vector<std::string> *problems)
{
	if(!stem.empty() && block.name != stem)
		Problem(problems, Format("block name: the INFO body says \"%s\" and the file is \"%s.ifp\"; "
			"the engine loads and streams the block by the size of this field",
			block.name.c_str(), stem.c_str()));
	if(block.name.size() >= BlockNameLimit)
		Problem(problems, Format("block name: the name is %d bytes; the loader copies at most %d with "
			"strncpy and needs a NUL", (int)block.name.size(), (int)BlockNameLimit));

	for(size_t i = 0; i < block.hierarchies.size(); i++){
		const Hierarchy &hier = block.hierarchies[i];
		std::string where = Format("animation %d \"%s\"", (int)i, hier.name.c_str());
		if(hier.name.size() >= SequenceNameLimit)
			Problem(problems, Format("%s: the name is %d bytes; the loader copies at most %d with "
				"strncpy and needs a NUL", where.c_str(), (int)hier.name.size(),
				(int)SequenceNameLimit));
		for(size_t j = 0; j < i; j++)
			if(block.hierarchies[j].name == hier.name)
				Problem(problems, Format("%s: another animation carries this name; the engine finds "
					"the first and this one is unreachable", where.c_str()));
		if(hier.sequences.empty())
			Problem(problems, where + ": the animation has no sequences");

		for(size_t s = 0; s < hier.sequences.size(); s++){
			const Sequence &seq = hier.sequences[s];
			CheckKeys(hier, seq, problems);
			if(seq.name == "Root" && seq.EngineBoneTag() != 0 && seq.EngineBoneTag() != -1)
				Problem(problems, Format("%s sequence \"Root\": the bone tag is %d; the loader binds "
					"the sequence by tag and never sees the root", where.c_str(), seq.EngineBoneTag()));
		}

		if(options.requireRoot){
			int rootIndex = -1;
			if(FindRoot(hier, &rootIndex) == 0)
				Problem(problems, where + ": no root sequence: no sequence is tagged 0 and none is an "
					"untagged \"Root\", so the loader sees no ground motion");
		}
	}

	if(options.movement)
		CheckMovement(block, problems);
}

} // namespace ifp
