// The prone animation set (VICE-187): the crawl cycles, the aim poses, the
// dive and the landing pose, built from the shipped Vice City prone content.
//
// The shipped content already holds every prone pose this set needs. The
// settled keys of FALL_front (the same body attitude the knockdowns and
// getup_front start from) are the prone base: face down, head forward, the
// legs on the ground. getup_front's early keys hold a knee drawn up and the
// arms pulled in, CAR_crawloutRHS holds a crawl, and the push-up keys lift the
// torso while the legs stay down. Every rotation in the output is therefore
// either a shipped key or a per-bone slerp of two shipped keys; only a pose
// that needs an attitude of its own (the aim lift, the dive pitch, the crawl
// sway) adds a small rotation in the bone's own frame.
//
//		tools/ifp/build_prone.sh [<gameRoot>] [<out.ifp>]
//
// gameRoot defaults to the Steam Vice City install and the output to
// gamefiles/anim/prone.ifp. The game file is opened read only. The tool prints
// the parameter table, the animation list with each root speed, and the four
// movement sets checked slot by slot with the validator's own rules.
//
// The rotation machinery and the bone-name lookup are shared with the crouch
// generator: tools/ifp/pose.h. The parameter table below is this generator's
// own.

#include "pose.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

using ifp::Block;
using ifp::CheckOptions;
using ifp::CountOf;
using ifp::FindBone;
using ifp::FindHierarchy;
using ifp::FindSequence;
using ifp::Hierarchy;
using ifp::Key;
using ifp::KeyIdent;
using ifp::Quat;
using ifp::QuatFromKey;
using ifp::QuatMake;
using ifp::QuatMultiply;
using ifp::QuatNormalise;
using ifp::QuatSlerp;
using ifp::Sequence;
using ifp::SetKeyRotation;

// ---------------------------------------------------------------- parameters
// Everything the owner tunes lives here, in one table. The pose of the prone
// set is changed here and nowhere else; the code below only interpolates.

// The prone base: the settled pose of the shipped face-down fall (its last key
// is a stable prone pose, the same body attitude the knockdowns settle into).
static const char *BaseAnim = "FALL_front";
static const float BaseTime = 0.9f;	// past the last key: the settled pose

// The crawl cycle. The engine plays one crawl speed, so all four cycles use one
// duration; the strides differ only enough for the validator's ladder (it
// wants walk < run < runfast, and the four sets each point the three moving
// slots at one cycle). 0.44, 0.53 and 0.61 units a second stay in the band a
// crawl can honestly use.
static const float CycleLength = 1.4f;	// seconds
static const int CycleKeys = 8;		// key intervals in one cycle (9 keys)

// Per direction: the root ground motion over one cycle (the ped's frame, +Y
// forward; the engine's strafes move -X for left and +X for right) and the
// body's own motion: a few degrees of pitch at every step, and a lean into the
// direction for the sideways crawls.
struct Crawl {
	const char *name;
	float strideX;
	float strideY;
	float pitchDeg;		// the body's sway over one cycle
	float leanDeg;		// the static lean into the direction travelled
	bool reverse;		// sweep the limbs the other way (a crawl backwards)
};
static const Crawl Crawls[] = {
	{ "crawl_fwd",   0.00f,  0.62f,  3.0f,  0.0f, false },
	{ "crawl_back",  0.00f, -0.74f,  3.0f,  0.0f, true },
	{ "crawl_left", -0.86f,  0.00f,  3.0f, -4.0f, false },
	{ "crawl_right", 0.86f,  0.00f,  3.0f,  4.0f, false },
};
static const int NumCrawls = CountOf(Crawls);

// The limbs. Each crawl key is a per-bone slerp of the base pose and one of
// these donors; the donors are shipped keys, so a limb angle in the output is
// an angle a shipped animation holds. The donor's left leg drives either leg
// (and its arms drive their own side): the ped rig gives a limb pair one rest
// orientation, so the same angles draw the other side's knee up the same way
// instead of crossing it.
static const char *LegDrawAnim = "getup_front";
static const float LegDrawTime = 0.2f;	// the left knee 101 degrees, the foot off the ground
static const char *ArmPullAnim = "CAR_crawloutRHS";
static const float ArmPullTime = 2.0f;	// the hands drawn in under the body

// The pose set the presentation selects on: front, side and back. Each is the
// base pose with the torso brought up (the legs stay prone) and, for the side
// pose, the chest rolled. `lift` turns the chest up about the bone's lateral
// axis and `twist` rolls it about the body's long axis; both are spread over
// the chain so no joint bends hard. The lift goes above the Spine because the
// rig hangs the legs off it: a lift on the Spine would take the legs with it.
struct AimPose {
	const char *name;
	float liftDeg;		// the torso up, the legs stay prone
	float twistDeg;		// the chest rolled (the rotate variants)
};
static const AimPose AimPoses[] = {
	{ "prone_aim_front",  8.0f,  0.0f },
	{ "prone_aim_side",  20.0f, 14.0f },
	{ "prone_aim_back",  34.0f,  0.0f },
};
static const int NumAimPoses = CountOf(AimPoses);

// The dive and the landing are the base pose with the body pitched (the root
// carries it) and the torso lifted; presentation only, the solver owns the
// motion.
static const float DivePitchDeg = 22.0f;	// the head down, mid-dive
static const float DiveLiftDeg = 10.0f;
static const float LandPitchDeg = -3.0f;	// the body flat, the chest catching
static const float LandLiftDeg = 6.0f;

// The idle and the start. The idle is the base pose with a small breath; the
// start is the push-off into the cycle, shared by the four sets (the engine's
// startwalk slot carries the ground motion along the ped's forward).
static const float IdleLength = 3.0f;
static const float IdleBreathDeg = 1.5f;
static const float StartLength = 0.5f;
static const float StartPushY = 0.16f;	// the ground motion of the push-off

// The block and the file. The order is the one the engine's movement slots
// read: the first five animations are the set the validator checks, and the
// engine's four groups point their slots at these names.
static const char *BlockName = "prone";
static const char *const BoneNames[] = {
	"Root", "Pelvis", "Spine", "Spine1", "Neck", "Head",
	"Bip01 L Clavicle", "L UpperArm", "L Forearm", "L Hand", "L Finger",
	"Bip01 R Clavicle", "R UpperArm", "R Forearm", "R Hand", "R Finger",
	"L Thigh", "L Calf", "L Foot",
	"R Thigh", "R Calf", "R Foot",
};
static const int NumBones = CountOf(BoneNames);

// The four engine sets, in the slot order aStdAnimDescs uses: walk, run,
// sprint, idle, startwalk. The three moving slots reuse one cycle: a crawl is
// slow and has one speed.
struct MovementSet {
	const char *name;
	const char *cycle;
};
static const MovementSet MovementSets[] = {
	{ "forward", "crawl_fwd" },
	{ "back",    "crawl_back" },
	{ "left",    "crawl_left" },
	{ "right",   "crawl_right" },
};
static const int NumMovementSets = CountOf(MovementSets);

// ---------------------------------------------------------------- quaternions

// A rotation of the table, about an axis in the bone's own frame. It stays in
// this file rather than moving to pose.h: the two generators round the same
// rotation differently (this one normalises the axis and the result, the crouch
// generator's divides the axis length into the sine), and a single shared
// constructor changed the bytes of the committed gamefiles/anim/prone.ifp. The
// composition itself is shared (QuatMultiply).
static Quat
AxisAngle(float ax, float ay, float az, float deg)
{
	float r = deg*(float)M_PI/360.0f;
	float s = sinf(r);
	float n = sqrtf(ax*ax + ay*ay + az*az);
	if(n == 0.0f)
		return QuatMake(0, 0, 0, 1);
	return QuatNormalise(QuatMake(ax/n*s, ay/n*s, az/n*s, cosf(r)));
}

// ---------------------------------------------------------------- sampling

// A shipped hierarchy read as a pose at one time: the per-bone slerp of its
// keys, clamped at the ends, plus the root's translation.
struct Pose {
	Quat rot[NumBones];
	float rootTrans[3];
};

static bool
SampleSequence(const Sequence &seq, float time, Quat *rot, float *trans)
{
	if(seq.keys.empty())
		return false;

	size_t i = 0;
	for(i = 0; i < seq.keys.size(); i++)
		if(seq.keys[i].time >= time)
			break;
	if(i == 0){
		*rot = QuatFromKey(seq.keys[0]);
		if(trans && seq.HasTranslation())
			for(int c = 0; c < 3; c++)
				trans[c] = seq.keys[0].translation[c];
		return true;
	}
	if(i >= seq.keys.size())
		i = seq.keys.size() - 1;

	const Key &a = seq.keys[i-1];
	const Key &b = seq.keys[i];
	float f = b.time > a.time ? (time - a.time)/(b.time - a.time) : 0.0f;
	if(f < 0.0f)
		f = 0.0f;
	if(f > 1.0f)
		f = 1.0f;
	*rot = QuatSlerp(QuatFromKey(a), QuatFromKey(b), f);
	if(trans && seq.HasTranslation())
		for(int c = 0; c < 3; c++)
			trans[c] = a.translation[c] + f*(b.translation[c] - a.translation[c]);
	return true;
}

// The 22 bones of one shipped animation at one time.
static bool
SamplePose(const Block &ped, const char *anim, float time, Pose *pose)
{
	int index = FindHierarchy(ped, anim);
	if(index < 0)
		return false;
	const Hierarchy &hier = ped.hierarchies[index];
	for(int b = 0; b < NumBones; b++){
		int s = FindSequence(hier, BoneNames[b]);
		if(s < 0)
			return false;
		if(!SampleSequence(hier.sequences[s], time, &pose->rot[b], pose->rootTrans))
			return false;
	}
	return true;
}

// The base pose of the set: the settled prone pose every animation starts from.
static Pose BasePose;
static Pose LegDrawPose;
static Pose LegDrawMirror;
static Pose ArmPullPose;

// The mirror-by-bones rule: the ped rig gives a limb pair one rest orientation,
// so the donor's left limb drives the right limb of the same kind. This builds
// the swapped copy once.
static void
MakeMirror(const Pose &src, Pose *dst)
{
	*dst = src;
	static const struct { const char *left; const char *right; } Pairs[] = {
		{ "L Thigh", "R Thigh" }, { "L Calf", "R Calf" }, { "L Foot", "R Foot" },
		{ "L UpperArm", "R UpperArm" }, { "L Forearm", "R Forearm" },
		{ "L Hand", "R Hand" }, { "L Finger", "R Finger" },
		{ "Bip01 L Clavicle", "Bip01 R Clavicle" },
	};
	for(int p = 0; p < CountOf(Pairs); p++){
		int l = FindBone(BoneNames, NumBones, Pairs[p].left);
		int r = FindBone(BoneNames, NumBones, Pairs[p].right);
		if(l >= 0 && r >= 0){
			dst->rot[r] = src.rot[l];
			dst->rot[l] = src.rot[r];
		}
	}
}

// ---------------------------------------------------------------- building

struct RootKey {
	float time;
	float trans[3];
};

struct Build {
	std::vector<Quat> rot[NumBones];
	std::vector<RootKey> root;

	void Push(float time)
	{
		root.resize(root.size() + 1);
		root.back().time = time;
	}

	void Add(int bone, const Quat &q) { rot[bone].push_back(q); }
};

// The base sequence order and the tags come from the base pose, so a new
// hierarchy is keyed exactly like the shipped one the engine binds.
static Hierarchy
MakeHierarchy(const Block &ped, const char *name, const Build &build)
{
	int index = FindHierarchy(ped, BaseAnim);
	const Hierarchy &source = ped.hierarchies[index];

	Hierarchy hier;
	hier.name = name;
	hier.reserved = source.reserved;

	for(int b = 0; b < NumBones; b++){
		int s = FindSequence(source, BoneNames[b]);
		if(s < 0)
			continue;
		const Sequence &seq = source.sequences[s];
		Sequence out = seq;
		out.keys.clear();
		if(build.rot[b].empty())
			continue;	// a bone the set does not key: no frames, no chunk
		std::vector<Key> keys;
		for(size_t k = 0; k < build.rot[b].size(); k++){
			Key key;
			memset(&key, 0, sizeof(key));
			SetKeyRotation(&key, build.rot[b][k]);
			key.time = build.root[k].time;
			if(b == 0){
				key.translation[0] = build.root[k].trans[0];
				key.translation[1] = build.root[k].trans[1];
				key.translation[2] = build.root[k].trans[2];
			}
			keys.push_back(key);
		}
		out.SetTagged(seq.EngineBoneTag());
		out.SetKeys(b == 0 ? ifp::KEYS_KRT0 : ifp::KEYS_KR00, keys);
		hier.sequences.push_back(out);
	}
	return hier;
}

// Every animation takes the same 22 bones, so the build starts as a copy of the
// base pose at every key and the caller replaces what moves.
static void
StartBase(Build *build, const std::vector<float> &times, const Pose &base)
{
	for(size_t k = 0; k < times.size(); k++){
		build->Push(times[k]);
		for(int b = 0; b < NumBones; b++)
			build->Add(b, base.rot[b]);
	}
}

// The crawl cycle: the legs alternate a draw-up and an extend, the arms
// alternate a pull and a reach contralateral to the legs, and the body sways.
static void
BuildCrawl(const Crawl &crawl, const Block &ped, Hierarchy *out)
{
	Build build;
	std::vector<float> times;
	for(int k = 0; k <= CycleKeys; k++)
		times.push_back(CycleLength*k/CycleKeys);
	StartBase(&build, times, BasePose);

	for(int k = 0; k <= CycleKeys; k++){
		float u = (float)k/CycleKeys;
		if(crawl.reverse)
			u = 1.0f - u;
		// The left leg draws up at the half cycle, the right leg is opposite;
		// the arms pair with the opposite leg (a crawl moves the right arm
		// with the left leg).
		float legLeft = 0.5f*(1.0f - cosf(2.0f*(float)M_PI*u));
		float legRight = 1.0f - legLeft;
		float armLeft = legRight;
		float armRight = legLeft;
		float sway = crawl.pitchDeg*sinf(2.0f*(float)M_PI*u);

		for(int b = 0; b < NumBones; b++){
			const char *name = BoneNames[b];
			Quat q = build.rot[b][k];
			if(strncmp(name, "L Thigh", 7) == 0 || strncmp(name, "L Calf", 6) == 0 ||
			   strncmp(name, "L Foot", 6) == 0)
				q = QuatSlerp(BasePose.rot[b], LegDrawPose.rot[b], legLeft);
			else if(strncmp(name, "R Thigh", 7) == 0 || strncmp(name, "R Calf", 6) == 0 ||
			        strncmp(name, "R Foot", 6) == 0)
				q = QuatSlerp(BasePose.rot[b], LegDrawMirror.rot[b], legRight);
			else if(strncmp(name, "L UpperArm", 10) == 0 || strncmp(name, "L Forearm", 9) == 0 ||
			        strncmp(name, "L Hand", 6) == 0 || strncmp(name, "L Finger", 8) == 0 ||
			        strncmp(name, "Bip01 L Clavicle", 16) == 0)
				q = QuatSlerp(BasePose.rot[b], ArmPullPose.rot[b], armLeft);
			else if(strncmp(name, "R UpperArm", 10) == 0 || strncmp(name, "R Forearm", 9) == 0 ||
			        strncmp(name, "R Hand", 6) == 0 || strncmp(name, "R Finger", 8) == 0 ||
			        strncmp(name, "Bip01 R Clavicle", 16) == 0)
				q = QuatSlerp(BasePose.rot[b], ArmPullPose.rot[b], armRight);
			// The body sways: the spine bends a little and the hips carry
			// the roll into a sideways crawl.
			if(strcmp(name, "Spine") == 0)
				q = QuatMultiply(q, AxisAngle(0, 0, 1, -0.6f*sway));
			else if(strcmp(name, "Spine1") == 0)
				q = QuatMultiply(q, AxisAngle(0, 0, 1, -0.4f*sway));
			else if(strcmp(name, "Pelvis") == 0)
				q = QuatMultiply(q, AxisAngle(1, 0, 0, -crawl.leanDeg));
			build.rot[b][k] = q;
		}

		float f = (float)k/CycleKeys;
		build.root[k].trans[0] = BasePose.rootTrans[0] + crawl.strideX*f;
		build.root[k].trans[1] = BasePose.rootTrans[1] + crawl.strideY*f;
		build.root[k].trans[2] = BasePose.rootTrans[2];
	}

	*out = MakeHierarchy(ped, crawl.name, build);
}

// The aim poses: the base pose with the torso lifted (the legs stay prone) and,
// for the side pose, the chest rolled.
static void
BuildAim(const AimPose &aim, const Block &ped, Hierarchy *out)
{
	Build build;
	std::vector<float> times;
	times.push_back(0.0f);
	times.push_back(1.0f);
	StartBase(&build, times, BasePose);

	for(int k = 0; k < (int)times.size(); k++){
		for(int b = 0; b < NumBones; b++){
			const char *name = BoneNames[b];
			Quat q = build.rot[b][k];
			float lift = 0.0f, twist = 0.0f;
			// The rig hangs the legs off the Spine, so the lift goes above
			// it (Spine1 and Neck) and the legs stay on the floor; the twist
			// rolls the hips through the Spine and the chest above it.
			if(strcmp(name, "Spine") == 0)
				twist = aim.twistDeg*0.35f;
			else if(strcmp(name, "Spine1") == 0){
				lift = aim.liftDeg*0.6f;
				twist = aim.twistDeg*0.4f;
			}else if(strcmp(name, "Neck") == 0){
				lift = aim.liftDeg*0.4f;
				twist = aim.twistDeg*0.25f;
			}
			// A negative bend about the bone's own lateral axis lifts the
			// chest; the twist is about the body's long axis.
			if(lift != 0.0f)
				q = QuatMultiply(q, AxisAngle(0, 0, 1, -lift));
			if(twist != 0.0f)
				q = QuatMultiply(q, AxisAngle(1, 0, 0, twist));
			build.rot[b][k] = q;
		}
		build.root[k].trans[0] = BasePose.rootTrans[0];
		build.root[k].trans[1] = BasePose.rootTrans[1];
		build.root[k].trans[2] = BasePose.rootTrans[2];
	}

	*out = MakeHierarchy(ped, aim.name, build);
}

// The dive and the landing: the base pose, the torso lifted, and the whole
// body pitched by the root.
static void
BuildPitched(const char *name, float pitchDeg, float liftDeg, const Block &ped, Hierarchy *out)
{
	Build build;
	std::vector<float> times;
	times.push_back(0.0f);
	times.push_back(0.75f);
	StartBase(&build, times, BasePose);

	for(int k = 0; k < (int)times.size(); k++){
		for(int b = 0; b < NumBones; b++){
			const char *bone = BoneNames[b];
			Quat q = build.rot[b][k];
			if(strcmp(bone, "Root") == 0)
				// The root's own lateral axis: a rotation about it pitches
				// the body (the head down for a positive angle).
				q = QuatMultiply(q, AxisAngle(0, 1, 0, pitchDeg));
			else if(strcmp(bone, "Spine1") == 0)
				q = QuatMultiply(q, AxisAngle(0, 0, 1, -liftDeg*0.5f));
			else if(strcmp(bone, "Neck") == 0)
				q = QuatMultiply(q, AxisAngle(0, 0, 1, -liftDeg*0.5f));
			build.rot[b][k] = q;
		}
		build.root[k].trans[0] = BasePose.rootTrans[0];
		build.root[k].trans[1] = BasePose.rootTrans[1];
		build.root[k].trans[2] = BasePose.rootTrans[2];
	}

	*out = MakeHierarchy(ped, name, build);
}

// The idle: the base pose with a small breath in the spine. The root does not
// translate, so the slot's reference speed is zero.
static void
BuildIdle(const Block &ped, Hierarchy *out)
{
	Build build;
	std::vector<float> times;
	times.push_back(0.0f);
	times.push_back(IdleLength*0.5f);
	times.push_back(IdleLength);
	StartBase(&build, times, BasePose);

	for(int k = 0; k < (int)times.size(); k++){
		float f = times[k]/IdleLength;
		float breath = IdleBreathDeg*(0.5f - 0.5f*cosf(2.0f*(float)M_PI*f));
		for(int b = 0; b < NumBones; b++){
			const char *name = BoneNames[b];
			Quat q = build.rot[b][k];
			if(strcmp(name, "Spine") == 0)
				q = QuatMultiply(q, AxisAngle(0, 0, 1, -breath));
			else if(strcmp(name, "Neck") == 0)
				q = QuatMultiply(q, AxisAngle(0, 0, 1, -0.5f*breath));
			build.rot[b][k] = q;
		}
		build.root[k].trans[0] = BasePose.rootTrans[0];
		build.root[k].trans[1] = BasePose.rootTrans[1];
		build.root[k].trans[2] = BasePose.rootTrans[2];
	}

	*out = MakeHierarchy(ped, "prone_idle", build);
}

// The start: the idle pose, then the crawl's gathered phase, pushing the body
// forward. The four sets share it, so its ground motion is along the ped's
// forward, the way the engine's startwalk slot reads it.
static void
BuildStart(const Block &ped, Hierarchy *out)
{
	Build build;
	std::vector<float> times;
	times.push_back(0.0f);
	times.push_back(StartLength*0.5f);
	times.push_back(StartLength);
	StartBase(&build, times, BasePose);

	for(int k = 0; k < (int)times.size(); k++){
		float f = times[k]/StartLength;
		// At the end the pose is the crawl's quarter-cycle phase: the left
		// leg and the right arm gathered for the first pull.
		float legLeft = 0.5f*(1.0f - cosf((float)M_PI*f));
		float armRight = legLeft;
		for(int b = 0; b < NumBones; b++){
			const char *name = BoneNames[b];
			Quat q = build.rot[b][k];
			if(strncmp(name, "L Thigh", 7) == 0 || strncmp(name, "L Calf", 6) == 0 ||
			   strncmp(name, "L Foot", 6) == 0)
				q = QuatSlerp(BasePose.rot[b], LegDrawPose.rot[b], legLeft);
			else if(strncmp(name, "R UpperArm", 10) == 0 || strncmp(name, "R Forearm", 9) == 0 ||
			        strncmp(name, "R Hand", 6) == 0 || strncmp(name, "R Finger", 8) == 0 ||
			        strncmp(name, "Bip01 R Clavicle", 16) == 0)
				q = QuatSlerp(BasePose.rot[b], ArmPullPose.rot[b], armRight);
			build.rot[b][k] = q;
		}
		build.root[k].trans[0] = BasePose.rootTrans[0];
		build.root[k].trans[1] = BasePose.rootTrans[1] + StartPushY*f;
		build.root[k].trans[2] = BasePose.rootTrans[2];
	}

	*out = MakeHierarchy(ped, "crawl_start", build);
}

// ---------------------------------------------------------------- evidence

static void
PrintSets(const Block &block)
{
	printf("\nthe engine's four sets (walk, run, runfast, idle, startwalk):\n");
	for(int s = 0; s < NumMovementSets; s++){
		const char *slots[5];
		slots[0] = slots[1] = slots[2] = MovementSets[s].cycle;
		slots[3] = "prone_idle";
		slots[4] = "crawl_start";
		printf("  %-8s", MovementSets[s].name);
		for(int i = 0; i < 5; i++){
			int index = FindHierarchy(block, slots[i]);
			if(index < 0){
				printf(" [%s MISSING]", slots[i]);
				continue;
			}
			ifp::HierarchyView view = ifp::ComputeView(block.hierarchies[index]);
			if(i < 3)
				printf(" %s(%.3f u/s)", slots[i], view.rootSpeed);
			else
				printf(" %s(speed %.3f)", slots[i], view.rootSpeed);
		}
		printf("\n");
	}

	// The same rules the validator applies to a movement block, on the first
	// five animations (the block's movement set).
	CheckOptions options;
	options.movement = true;
	std::vector<std::string> problems;
	ifp::CheckBlock(block, BlockName, options, &problems);
	printf("\nthe validator on the built block: %s\n",
		problems.empty() ? "OK" : problems[0].c_str());
}

int
main(int argc, char **argv)
{
	const char *game = argc > 1 ? argv[1] : 0;
	const char *out = argc > 2 ? argv[2] : "gamefiles/anim/prone.ifp";

	std::string input = game ? std::string(game) : std::string("");
	if(input.empty()){
		const char *home = getenv("HOME");
		input = home ? std::string(home) : std::string(".");
		input += "/Games/Steam/steamapps/common/Grand Theft Auto Vice City";
	}
	// A game root holds anim/ped.ifp; anything else is the file itself.
	std::string path = input;
	struct stat st;
	if(stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
		path = input + "/anim/ped.ifp";
	ifp::ReadResult read = ifp::ReadBlock(path);
	if(read.fatal || !read.problems.empty()){
		fprintf(stderr, "cannot read the shipped ped.ifp under \"%s\"\n", input.c_str());
		return 1;
	}
	const Block &ped = read.block;

	if(!SamplePose(ped, BaseAnim, BaseTime, &BasePose)){
		fprintf(stderr, "no %s in the shipped file\n", BaseAnim);
		return 1;
	}
	if(!SamplePose(ped, LegDrawAnim, LegDrawTime, &LegDrawPose)){
		fprintf(stderr, "no %s in the shipped file\n", LegDrawAnim);
		return 1;
	}
	if(!SamplePose(ped, ArmPullAnim, ArmPullTime, &ArmPullPose)){
		fprintf(stderr, "no %s in the shipped file\n", ArmPullAnim);
		return 1;
	}
	MakeMirror(LegDrawPose, &LegDrawMirror);

	printf("prone, from %s\n", BaseAnim);
	printf("  base: %s at %.2f s\n", BaseAnim, BaseTime);
	printf("  leg draw-up: %s at %.2f s, arm pull: %s at %.2f s\n",
		LegDrawAnim, LegDrawTime, ArmPullAnim, ArmPullTime);
	printf("  cycle: %.2f s, %d intervals; idle %.2f s, start %.2f s\n",
		CycleLength, CycleKeys, IdleLength, StartLength);
	for(int i = 0; i < NumCrawls; i++)
		printf("  %-11s stride (%+.2f %+.2f) pitch %.1f lean %.1f\n", Crawls[i].name,
			Crawls[i].strideX, Crawls[i].strideY, Crawls[i].pitchDeg, Crawls[i].leanDeg);

	Block block;
	block.name = BlockName;

	// The block order: the four cycles, the idle, the start, then the poses.
	// The first five are the movement set the validator checks, so the idle
	// sits in the idle slot and the start in the startwalk slot.
	Hierarchy hier;
	BuildCrawl(Crawls[0], ped, &hier); block.hierarchies.push_back(hier);	// walk
	BuildCrawl(Crawls[1], ped, &hier); block.hierarchies.push_back(hier);	// run
	BuildCrawl(Crawls[2], ped, &hier); block.hierarchies.push_back(hier);	// runfast
	BuildIdle(ped, &hier); block.hierarchies.push_back(hier);		// idle
	BuildStart(ped, &hier); block.hierarchies.push_back(hier);		// startwalk
	BuildCrawl(Crawls[3], ped, &hier); block.hierarchies.push_back(hier);	// the fourth cycle
	for(int i = 0; i < NumAimPoses; i++){
		BuildAim(AimPoses[i], ped, &hier);
		block.hierarchies.push_back(hier);
	}
	BuildPitched("prone_dive", DivePitchDeg, DiveLiftDeg, ped, &hier);
	block.hierarchies.push_back(hier);
	BuildPitched("prone_land", LandPitchDeg, LandLiftDeg, ped, &hier);
	block.hierarchies.push_back(hier);

	long keys = 0;
	printf("\nthe animations:\n");
	for(size_t i = 0; i < block.hierarchies.size(); i++){
		ifp::HierarchyView view = ifp::ComputeView(block.hierarchies[i]);
		long n = 0;
		for(size_t s = 0; s < block.hierarchies[i].sequences.size(); s++)
			n += (long)block.hierarchies[i].sequences[s].keys.size();
		keys += n;
		printf("  %d %-16s %2d sequences %4ld keys  %.3f s  rootSpeed %.3f\n",
			(int)i, block.hierarchies[i].name.c_str(),
			(int)block.hierarchies[i].sequences.size(), n, view.totalLength,
			view.rootSpeed);
	}

	std::string error;
	if(!ifp::WriteBlock(out, block, &error)){
		fprintf(stderr, "%s: %s\n", out, error.c_str());
		return 1;
	}
	printf("\nwrote %s (block \"%s\", %d animations, %ld keys)\n", out, block.name.c_str(),
		(int)block.hierarchies.size(), keys);

	PrintSets(block);
	return 0;
}
