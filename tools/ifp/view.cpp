// The loader's view of a hierarchy and the key-for-key comparison. This is the
// other half of the specification: what CAnimBlendHierarchy::CalcTotalTime and
// CAnimBlendSequence::RemoveQuaternionFlips make of the keys the reader stored.

#include "ifp.h"


namespace ifp {

static float
Dot(const float *a, const float *b)
{
	return a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
}

HierarchyView
ComputeView(const Hierarchy &h)
{
	HierarchyView view;
	view.name = h.name;
	view.totalLength = 0.0f;
	view.rootSpeed = 0.0f;
	view.rootSequence = -1;

	for(size_t i = 0; i < h.sequences.size(); i++){
		const Sequence &seq = h.sequences[i];
		SequenceView sv;
		sv.name = seq.name;
		sv.engineBoneTag = seq.EngineBoneTag();
		sv.hasTranslation = seq.HasTranslation();

		// CAnimBlendSequence::RemoveQuaternionFlips
		for(size_t k = 0; k < seq.keys.size(); k++){
			KeyView kv;
			for(int c = 0; c < 4; c++)
				kv.rotation[c] = seq.keys[k].rotation[c];
			if(k > 0 && Dot(seq.keys[k-1].rotation, seq.keys[k].rotation) < 0.0f)
				for(int c = 0; c < 4; c++)
					kv.rotation[c] = -kv.rotation[c];
			for(int c = 0; c < 3; c++)
				kv.translation[c] = seq.keys[k].translation[c];
			// CAnimBlendHierarchy::CalcTotalTime turns the absolute times into
			// deltas; the first key keeps its absolute time.
			kv.deltaTime = k == 0 ? seq.keys[0].time : seq.keys[k].time - seq.keys[k-1].time;
			sv.keys.push_back(kv);
		}
		view.sequences.push_back(sv);

		// CAnimBlendHierarchy::CalcTotalTime, with the FIX_BUGS guard for a
		// sequence with no frames.
		if(seq.keys.empty())
			continue;
		if(IsRootSequence(seq)){
			if(view.rootSequence < 0)
				view.rootSequence = (int)i;
			if(seq.HasTranslation())
				view.rootSpeed = RootCycleSpeed(seq);
		}
		float lastTime = seq.keys.back().time;
		if(lastTime > view.totalLength)
			view.totalLength = lastTime;
	}
	return view;
}

namespace {

void
Check(bool equal, const std::string &what, CompareResult *result)
{
	result->checks++;
	if(equal)
		return;
	result->failures++;
	if(result->mismatches.size() < 20)
		result->mismatches.push_back(what);
}

void
CheckFloats(const float *a, const float *b, int count, const std::string &what, CompareResult *result)
{
	bool equal = true;
	for(int i = 0; i < count; i++)
		if(!(a[i] == b[i]))
			equal = false;
	if(equal){
		result->checks++;
		return;
	}
	std::string line = what + ":";
	for(int i = 0; i < count; i++)
		line += Format(" %.9g", a[i]);
	line += " vs";
	for(int i = 0; i < count; i++)
		line += Format(" %.9g", b[i]);
	Check(false, line, result);
}

} // anonymous

void
CompareBlocks(const Block &a, const Block &b, CompareResult *result)
{
	Check(a.name == b.name, "block name '" + a.name + "' vs '" + b.name + "'", result);
	Check(a.hierarchies.size() == b.hierarchies.size(),
		Format("animation count %d vs %d", (int)a.hierarchies.size(), (int)b.hierarchies.size()), result);

	size_t count = a.hierarchies.size() < b.hierarchies.size() ? a.hierarchies.size() : b.hierarchies.size();
	for(size_t i = 0; i < count; i++){
		const Hierarchy &ha = a.hierarchies[i];
		const Hierarchy &hb = b.hierarchies[i];
		HierarchyView va = ComputeView(ha);
		HierarchyView vb = ComputeView(hb);
		std::string where = Format("animation %d (\"%s\")", (int)i, ha.name.c_str());

		Check(va.name == vb.name, where + ": name '" + va.name + "' vs '" + vb.name + "'", result);
		Check(va.sequences.size() == vb.sequences.size(),
			Format("%s: sequence count %d vs %d", where.c_str(), (int)va.sequences.size(),
				(int)vb.sequences.size()), result);
		CheckFloats(&va.totalLength, &vb.totalLength, 1, where + ": totalLength", result);
		CheckFloats(&va.rootSpeed, &vb.rootSpeed, 1, where + ": rootSpeed", result);
		Check(va.rootSequence == vb.rootSequence,
			Format("%s: root sequence %d vs %d", where.c_str(), va.rootSequence, vb.rootSequence), result);

		size_t seqs = va.sequences.size() < vb.sequences.size() ? va.sequences.size() : vb.sequences.size();
		for(size_t s = 0; s < seqs; s++){
			const SequenceView &sa = va.sequences[s];
			const SequenceView &sb = vb.sequences[s];
			std::string swhere = Format("%s sequence %d (\"%s\")", where.c_str(), (int)s, sa.name.c_str());
			Check(sa.name == sb.name, swhere + ": name '" + sa.name + "' vs '" + sb.name + "'", result);
			Check(sa.engineBoneTag == sb.engineBoneTag,
				Format("%s: bone tag %d vs %d", swhere.c_str(), sa.engineBoneTag, sb.engineBoneTag), result);
			Check(sa.hasTranslation == sb.hasTranslation,
				Format("%s: translation keys %d vs %d", swhere.c_str(), (int)sa.hasTranslation,
					(int)sb.hasTranslation), result);
			Check(sa.keys.size() == sb.keys.size(),
				Format("%s: frame count %d vs %d", swhere.c_str(), (int)sa.keys.size(),
					(int)sb.keys.size()), result);

			size_t keys = sa.keys.size() < sb.keys.size() ? sa.keys.size() : sb.keys.size();
			for(size_t k = 0; k < keys; k++){
				std::string kwhere = Format("%s key %d", swhere.c_str(), (int)k);
				CheckFloats(sa.keys[k].rotation, sb.keys[k].rotation, 4, kwhere + ": rotation", result);
				CheckFloats(sa.keys[k].translation, sb.keys[k].translation, 3, kwhere + ": translation", result);
				CheckFloats(&sa.keys[k].deltaTime, &sb.keys[k].deltaTime, 1, kwhere + ": delta time", result);
			}
		}
	}
}

} // namespace ifp
