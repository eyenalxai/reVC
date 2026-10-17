// The crouch pose step (tools/ifp, VICE-186): the movement block of the crouch
// stance, built from the shipped standing cycles.
//
//   build/ifp crouch "<gameRoot>/anim/ped.ifp" gamefiles/anim/crouch.ifp
//
// The shipped crouch content is stationary (WEAPON_crouch, DUCK_low): it folds
// the legs and drops the body for the duck, and carries no stride, so it cannot
// drive a moving stance. This step turns the shipped standing cycles into crouch
// cycles in three moves, all of them driven by the tables below:
//
//   1. The crouch pose: one rotation is composed into every key of a leg or
//      spine bone, q' = q_offset * q. q_offset lives in the bone's parent frame,
//      the frame the key's quaternion rotates in, so an axis of (0, 0, -1) is
//      the parent's own Z. The rows are the shipped crouch pose (WEAPON_crouch:
//      its standing first key to its settled last key), averaged over the two
//      legs, so a crouch cycle carries the leg angles the game already shows
//      while the player ducks. The arms keep their own source keys.
//   2. The body height: the root's z of every key becomes the crouch body
//      height. The engine reads the root key's z as the body drop
//      (FrameUpdate::xform->t.z = the root translation), so one height for every
//      crouch cycle is one crouch body height, whatever the source stood at.
//   3. The stride: the root's cycle translation is scaled about its first key.
//      The pose folds the legs, and a folded leg covers less ground per swing,
//      so the stride must shrink with it or the feet slide. The scale is a
//      per-cycle table column, because a shallower crouch keeps a longer
//      planted stride.
//
// The block holds four directional movement sets, in the engine's movement slot
// order (walk, run, runfast, idle, startwalk):
//
//   crouch       forward  walk_crouch, run_crouch, sprint_crouch, ...
//   crouchback   back     walk_back_crouch, run_back_crouch, ...
//   crouchleft   left     walk_left_crouch, run_left_crouch, ...
//   crouchright  right    walk_right_crouch, run_right_crouch, ...
//
// The idle is one cycle, IDLE_crouch, named by all four groups: a crouch does
// not distinguish the direction it stands still in. The group names and their
// block and description arrays are the engine's table
// (CAnimManager::ms_aAnimAssocDefinitions); this file is the content they read.
//
// The names of the block and its cycles are the shipped names with a _crouch
// suffix (WALK_player -> walk_crouch), so the shipped block and the crouch block
// never collide and the groups pick theirs by name.

#include "ifp.h"

#include <math.h>
#include <ostream>

namespace ifp {

// --------------------------------------------------------------- the tables

// The crouch pose: one row per bone the pose rotates. "degrees" is the whole
// joint rotation at the full crouch depth (depth 1.0 of a cycle row below); a
// shallower tier scales it. The axis is normalized on use, so only its
// direction matters. Tune the pose here: the angles are the shipped crouch's,
// and the comment on each row says what it does.
struct CrouchPoseRow {
	const char *bone;
	float axis[3];
	float degrees;
	const char *what;
};

static const CrouchPoseRow CrouchPose[] = {
	{ "L Thigh", {  0.250f,  0.392f, -0.885f },  79.0f, "hip flexion: the thigh swings forward under the pelvis" },
	{ "R Thigh", {  0.250f,  0.392f, -0.885f },  79.0f, "hip flexion: the thigh swings forward under the pelvis" },
	{ "L Calf",  {  0.0f,    0.0f,   -1.0f   }, 118.3f, "knee flexion: the calf folds under the thigh" },
	{ "R Calf",  {  0.0f,    0.0f,   -1.0f   }, 118.3f, "knee flexion: the calf folds under the thigh" },
	{ "L Foot",  {  0.184f, -0.005f,  0.983f },  59.6f, "ankle pitch: the sole stays flat on the ground" },
	{ "R Foot",  {  0.184f, -0.005f,  0.983f },  59.6f, "ankle pitch: the sole stays flat on the ground" },
	{ "Spine",   { -0.342f, -0.149f,  0.928f },  20.1f, "lean: the lower spine comes forward over the knees" },
	{ "Spine1",  { -0.381f,  0.387f,  0.840f },  34.3f, "lean: the upper spine rounds the shoulders forward" },
};
static const int NumCrouchPoseRows = (int)(sizeof(CrouchPose)/sizeof(CrouchPose[0]));

// The crouch body height: the root key z of the shipped crouch pose's settled
// key (WEAPON_crouch, root z -0.587; its standing first key is -0.026, so the
// body drops 0.561 units, about the shortening the folded legs owe the ground).
// The engine reads the root key's z as the body drop, so this is the height
// every crouch cycle sits at, and a shallower tier sits at depth times it.
// Tune it here, and keep the pose rows that fold the legs in step with it: the
// drop and the knee flexion are one setting, or the feet float or sink.
static const float CrouchRootHeight = -0.587f;

// The cycles of the block, in file order. The first five are the forward set,
// in the engine's slot order, so the block itself checks as a movement set; the
// other three directions follow with their four cycles each.
//   name   the animation name in the new block
//   source the shipped cycle to transform
//   depth  the share of the crouch pose this cycle carries, 1.0 = the whole
//          pose, 0.0 = standing
//   stride the scale of the root cycle translation about the root's first key.
//          It must follow the depth: the pose folds the legs, and a folded leg
//          covers less ground per swing, so the stride shrinks with it (the
//          half of the full pose) or the planted foot slides. A shallower tier
//          folds the legs less and keeps the longer stride.
struct CrouchCycle {
	const char *name;
	const char *source;
	float depth;
	float stride;
};

static const CrouchCycle CrouchCycles[] = {
	// forward: the hero's walk and run, the civilian sprint
	{ "walk_crouch",	"WALK_player",	1.0f,	0.50f },
	{ "run_crouch",		"run_player",	1.0f,	0.50f },
	{ "sprint_crouch",	"sprint_civi",	1.0f,	0.50f },
	// the stance idle: one cycle, shared by every direction
	{ "IDLE_crouch",	"IDLE_stance",	1.0f,	1.00f },
	{ "walk_start_crouch",	"WALK_start",	1.0f,	0.50f },
	// back
	{ "walk_back_crouch",		"walk_back",		1.0f,	0.50f },
	{ "run_back_crouch",		"run_back",		1.0f,	0.50f },
	// the shipped back cycles hold no faster variant, so the sprint tier is the
	// run's cycle carried shallower: straighter legs reach a longer stride, and
	// the movement ladder (walk < run < sprint) needs the faster cycle
	{ "sprint_back_crouch",		"run_back",		0.75f,	0.62f },
	{ "walk_start_back_crouch",	"walk_start_back",	1.0f,	0.50f },
	// left
	{ "walk_left_crouch",		"walk_left",		1.0f,	0.50f },
	{ "run_left_crouch",		"run_left",		1.0f,	0.50f },
	{ "sprint_left_crouch",		"run_left",		0.75f,	0.62f },
	{ "walk_start_left_crouch",	"walk_start_left",	1.0f,	0.50f },
	// right
	{ "walk_right_crouch",		"walk_right",		1.0f,	0.50f },
	{ "run_right_crouch",		"run_right",		1.0f,	0.50f },
	{ "sprint_right_crouch",	"run_right",		0.75f,	0.62f },
	{ "walk_start_right_crouch",	"walk_start_right",	1.0f,	0.50f },
};
static const int NumCrouchCycles = (int)(sizeof(CrouchCycles)/sizeof(CrouchCycles[0]));

// The four movement sets, in the group order of the engine's table
// (ASSOCGRP_CROUCH, CROUCHBACK, CROUCHLEFT, CROUCHRIGHT). The five names of a
// set are the five slots in order, and the group array in AnimManager.cpp must
// name them in the same order.
static const char *const CrouchSetNames[] = {
	"crouch", "crouchback", "crouchleft", "crouchright",
};
static const int NumCrouchSets = (int)(sizeof(CrouchSetNames)/sizeof(CrouchSetNames[0]));
static const char *const CrouchSets[][5] = {
	{ "walk_crouch", "run_crouch", "sprint_crouch", "IDLE_crouch", "walk_start_crouch" },
	{ "walk_back_crouch", "run_back_crouch", "sprint_back_crouch", "IDLE_crouch", "walk_start_back_crouch" },
	{ "walk_left_crouch", "run_left_crouch", "sprint_left_crouch", "IDLE_crouch", "walk_start_left_crouch" },
	{ "walk_right_crouch", "run_right_crouch", "sprint_right_crouch", "IDLE_crouch", "walk_start_right_crouch" },
};

// --------------------------------------------------------------- the transform

// One rotation. The tool stores the pose rotation the loader ends up with, so
// the composition lives in that space and the writer conjugates it again.
struct Quat {
	float x, y, z, w;
};

static Quat
FromAxisAngle(const float *axis, float degrees)
{
	float length = sqrtf(axis[0]*axis[0] + axis[1]*axis[1] + axis[2]*axis[2]);
	float half = degrees * (float)M_PI / 360.0f;
	float s = sinf(half);
	Quat q;
	if(length > 0.0f)
		s /= length;
	q.x = axis[0] * s;
	q.y = axis[1] * s;
	q.z = axis[2] * s;
	q.w = cosf(half);
	return q;
}

// a then b: the rotation b applied first, then a, both in the same frame. The
// pose offset pre-multiplies a key (q' = offset * q) to rotate the bone in its
// parent's frame, the frame the key's quaternion is expressed in.
static Quat
Multiply(const Quat &a, const Quat &b)
{
	Quat q;
	q.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
	q.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
	q.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
	q.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
	return q;
}

static void
Normalise(Quat *q)
{
	float length = sqrtf(q->x*q->x + q->y*q->y + q->z*q->z + q->w*q->w);
	if(length > 0.0f){
		q->x /= length;
		q->y /= length;
		q->z /= length;
		q->w /= length;
	}
}

static const CrouchPoseRow *
FindPoseRow(const std::string &bone)
{
	for(int i = 0; i < NumCrouchPoseRows; i++)
		if(bone == CrouchPose[i].bone)
			return &CrouchPose[i];
	return 0;
}

// The crouch pose on every key of the bones of the table. A row the hierarchy
// has no sequence for is a defect: the pose would be half applied, and the
// table and the shipped skeleton would have drifted apart.
static void
ApplyPose(Hierarchy *hier, float depth, std::vector<std::string> *problems)
{
	for(size_t s = 0; s < hier->sequences.size(); s++){
		Sequence &seq = hier->sequences[s];
		const CrouchPoseRow *row = FindPoseRow(seq.name);
		if(row == 0)
			continue;
		Quat offset = FromAxisAngle(row->axis, row->degrees * depth);
		for(size_t k = 0; k < seq.keys.size(); k++){
			Key &key = seq.keys[k];
			Quat q;
			q.x = key.rotation[0];
			q.y = key.rotation[1];
			q.z = key.rotation[2];
			q.w = key.rotation[3];
			q = Multiply(offset, q);
			Normalise(&q);
			key.rotation[0] = q.x;
			key.rotation[1] = q.y;
			key.rotation[2] = q.z;
			key.rotation[3] = q.w;
		}
	}

	for(int i = 0; i < NumCrouchPoseRows; i++){
		bool found = false;
		for(size_t s = 0; s < hier->sequences.size(); s++)
			if(hier->sequences[s].name == CrouchPose[i].bone)
				found = true;
		if(!found)
			problems->push_back(Format("animation \"%s\": the pose bone \"%s\" (%s) has no sequence",
				hier->name.c_str(), CrouchPose[i].bone, CrouchPose[i].what));
	}
}

// The crouch body height and the crouch stride on the root cycle. The engine
// reads the root key z as the body drop, so the cycle is put at the tier's
// height (the source's own height leaves no trace, which is what keeps the
// crouch body one height); the x and y cycle translation is scaled about the
// root's first key, so the cycle still starts where the source starts.
static void
ApplyRoot(Hierarchy *hier, float depth, float stride, std::vector<std::string> *problems)
{
	Sequence *root = 0;
	for(size_t s = 0; s < hier->sequences.size(); s++)
		if(IsRootSequence(hier->sequences[s])){
			root = &hier->sequences[s];
			break;
		}
	if(root == 0){
		problems->push_back(Format("animation \"%s\": no root sequence: the loader finds the ground "
			"motion by bone tag 0 or the exact name \"Root\"", hier->name.c_str()));
		return;
	}
	if(!root->HasTranslation()){
		problems->push_back(Format("animation \"%s\": the root \"%s\" is a %s sequence with no translation "
			"keys, so the cycle cannot carry the stride", hier->name.c_str(), root->name.c_str(),
			KeyIdentName(root->ident)));
		return;
	}

	float height = CrouchRootHeight * depth;
	// The first key's translation is the cycle's own reference: read it before
	// the loop, because the first key is one of the keys written below.
	float x0 = root->keys.front().translation[0];
	float y0 = root->keys.front().translation[1];
	float z0 = root->keys.front().translation[2];
	for(size_t k = 0; k < root->keys.size(); k++){
		Key &key = root->keys[k];
		key.translation[0] = x0 + stride * (key.translation[0] - x0);
		key.translation[1] = y0 + stride * (key.translation[1] - y0);
		key.translation[2] = height + (key.translation[2] - z0);
	}
}

// One cycle of the block: the source animation with the pose, the body height
// and the stride applied.
static bool
BuildCycle(const Block &source, const CrouchCycle &cycle, Hierarchy *out,
	std::vector<std::string> *problems)
{
	int index = FindHierarchy(source, cycle.source);
	if(index < 0){
		problems->push_back(Format("the source block has no animation \"%s\" for \"%s\"",
			cycle.source, cycle.name));
		return false;
	}
	*out = source.hierarchies[index];
	out->name = cycle.name;
	ApplyPose(out, cycle.depth, problems);
	ApplyRoot(out, cycle.depth, cycle.stride, problems);
	return true;
}

// --------------------------------------------------------------- the block

bool
BuildCrouchBlock(const Block &source, Block *out, std::vector<std::string> *problems)
{
	size_t first = problems->size();
	out->name = "crouch";
	for(int i = 0; i < NumCrouchCycles; i++){
		Hierarchy hier;
		if(BuildCycle(source, CrouchCycles[i], &hier, problems))
			out->hierarchies.push_back(hier);
	}
	// The set table and the cycle table must agree: a name the block does not
	// hold is a group that would load an empty association.
	for(int s = 0; s < NumCrouchSets; s++)
		for(int i = 0; i < NumMovementSlots; i++)
			if(FindHierarchy(*out, CrouchSets[s][i]) < 0)
				problems->push_back(Format("movement set \"%s\": slot %d (%s) is \"%s\" and the "
					"block has no such animation", CrouchSetNames[s], i, MovementSlots[i].role,
					CrouchSets[s][i]));
	return problems->size() == first;
}

// Every set passes the movement checks on its own. The block's own movement
// check reads its first five animations only (the forward set); the back, left
// and right sets are what the other three groups play, so the four are checked
// here, one at a time, exactly as CheckBlock would check a block of five.
bool
CheckCrouchSets(const Block &block, std::vector<std::string> *problems)
{
	size_t first = problems->size();
	for(int s = 0; s < NumCrouchSets; s++){
		Block set;
		set.name = block.name;
		bool complete = true;
		for(int i = 0; i < NumMovementSlots; i++){
			int index = FindHierarchy(block, CrouchSets[s][i]);
			if(index < 0){
				problems->push_back(Format("movement set \"%s\": no animation \"%s\" in the block",
					CrouchSetNames[s], CrouchSets[s][i]));
				complete = false;
				continue;
			}
			set.hierarchies.push_back(block.hierarchies[index]);
		}
		if(!complete)
			continue;
		size_t before = problems->size();
		CheckOptions options;
		options.movement = true;
		CheckBlock(set, "", options, problems);
		for(size_t i = before; i < problems->size(); i++)
			(*problems)[i] = Format("movement set \"%s\": %s", CrouchSetNames[s], (*problems)[i].c_str());
	}
	return problems->size() == first;
}

void
CrouchSetSummary(const Block &block, std::ostream &out)
{
	for(int s = 0; s < NumCrouchSets; s++){
		out << Format("  %-12s", CrouchSetNames[s]);
		for(int i = 0; i < NumMovementSlots; i++){
			int index = FindHierarchy(block, CrouchSets[s][i]);
			if(index < 0){
				out << " " << CrouchSets[s][i] << " missing";
				continue;
			}
			HierarchyView view = ComputeView(block.hierarchies[index]);
			out << Format(" %s %.3g", CrouchSets[s][i], view.rootSpeed);
		}
		out << "\n";
	}
}

} // namespace ifp
