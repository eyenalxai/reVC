// The human-readable IFP dump (tools/ifp/ifp.h): the block, the animations, the
// sequences with their bone tags, frame counts, keyframe identities and
// absolute key times, and the computed totalLength and rootSpeed.

#include "ifp.h"

#include <ostream>

namespace ifp {

// ConvertBoneTag2BoneName (src/animation/Bones.cpp), for the dump only: the
// tool is standalone and must not include an engine header.
static const struct { int tag; const char *name; } BoneTags[] = {
	{ 0,  "Root" },
	{ 1,  "Pelvis" },
	{ 2,  "Spine" },
	{ 3,  "Spine1" },
	{ 4,  "Neck" },
	{ 5,  "Head" },
	{ 21, "Bip01 R Clavicle" },
	{ 22, "R UpperArm" },
	{ 23, "R Forearm" },
	{ 24, "R Hand" },
	{ 25, "R Fingers" },
	{ 31, "Bip01 L Clavicle" },
	{ 32, "L UpperArm" },
	{ 33, "L Forearm" },
	{ 34, "L Hand" },
	{ 35, "L Fingers" },
	{ 41, "L Thigh" },
	{ 42, "L Calf" },
	{ 43, "L Foot" },
	{ 51, "R Thigh" },
	{ 52, "R Calf" },
	{ 53, "R Foot" },
};

static const char *
BoneTagName(int tag)
{
	for(size_t i = 0; i < sizeof(BoneTags)/sizeof(BoneTags[0]); i++)
		if(BoneTags[i].tag == tag)
			return BoneTags[i].name;
	return 0;
}

static std::string
TagText(const Sequence &seq)
{
	int tag = seq.EngineBoneTag();
	if(tag < 0){
		if(seq.boneTag >= 0)
			return Format("-(file %d)", seq.boneTag);
		return "-";
	}
	const char *name = BoneTagName(tag);
	if(name)
		return Format("%d(%s)", tag, name);
	return Format("%d", tag);
}

void
DumpBlock(const Block &block, std::vector<std::string> *only, bool withKeys, std::ostream &out)
{
	int hierarchies = 0;
	long sequences = 0;
	long keys = 0;
	for(size_t i = 0; i < block.hierarchies.size(); i++){
		const Hierarchy &hier = block.hierarchies[i];
		if(only && !only->empty()){
			bool wanted = false;
			for(size_t n = 0; n < only->size(); n++)
				if((*only)[n] == hier.name)
					wanted = true;
			if(!wanted)
				continue;
		}
		hierarchies++;
		HierarchyView view = ComputeView(hier);
		out << Format("  %s: %d sequences, totalLength %.5g, rootSpeed %.5g, ",
			hier.name.c_str(), (int)hier.sequences.size(), view.totalLength, view.rootSpeed);
		if(view.rootSequence < 0)
			out << "root none\n";
		else
			out << Format("root %d \"%s\" (tag %s)\n", view.rootSequence,
				hier.sequences[view.rootSequence].name.c_str(),
				TagText(hier.sequences[view.rootSequence]).c_str());
		for(size_t s = 0; s < hier.sequences.size(); s++){
			const Sequence &seq = hier.sequences[s];
			sequences++;
			keys += (long)seq.keys.size();
			std::string times = "-";
			if(!seq.keys.empty())
				times = Format("%.5g..%.5g", seq.keys.front().time, seq.keys.back().time);
			out << Format("    %-20s tag=%-20s frames=%-4d keys=%-5s t=[%s]",
				seq.name.c_str(), TagText(seq).c_str(), (int)seq.numFrames,
				KeyIdentName(seq.ident), times.c_str());
			if(seq.hasKeyChunk && seq.keys.empty())
				out << " (chunk on 0 frames)";
			out << "\n";
			if(!withKeys)
				continue;
			for(size_t k = 0; k < seq.keys.size(); k++){
				const Key &key = seq.keys[k];
				out << Format("      [%d] rot=(%.6g %.6g %.6g %.6g) t=%.6g", (int)k, key.rotation[0],
					key.rotation[1], key.rotation[2], key.rotation[3], key.time);
				if(seq.HasTranslation())
					out << Format(" pos=(%.6g %.6g %.6g)", key.translation[0], key.translation[1],
						key.translation[2]);
				if(seq.ident == KEYS_KRTS)
					out << Format(" scale=(%.6g %.6g %.6g)", key.scale[0], key.scale[1], key.scale[2]);
				out << "\n";
			}
		}
	}
	if(!only || only->empty())
		out << Format("block \"%s\": %d animations, %ld sequences, %ld keys\n",
			block.name.c_str(), (int)block.hierarchies.size(), sequences, keys);
	else
		out << Format("block \"%s\": %d of %d animations shown, %ld sequences, %ld keys\n",
			block.name.c_str(), hierarchies, (int)block.hierarchies.size(), sequences, keys);
}

int
FindHierarchy(const Block &block, const std::string &name)
{
	for(size_t i = 0; i < block.hierarchies.size(); i++)
		if(block.hierarchies[i].name == name)
			return (int)i;
	return -1;
}

} // namespace ifp
