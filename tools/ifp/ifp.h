#ifndef __TOOLS_IFP_IFP_H__
#define __TOOLS_IFP_IFP_H__

// IFP (animation) reader, writer and validator for the movement content.
//
// The engine reads the ANPK form only. CAnimManager::LoadAnimFile
// (src/animation/AnimManager.cpp) is the specification: it skips every chunk by
// its size rounded up to 4, ignores all sizes otherwise, and type-puns each
// body. This tool therefore carries its own model of the format and no engine
// header.
//
// Build and run, from the repository root:
//
//   tools/ifp/build.sh
//   build/ifp dump      "<gameRoot>/anim/ped.ifp" WALK_civi
//   build/ifp check     "<gameRoot>/anim/ped.ifp"
//   build/ifp roundtrip "<gameRoot>/anim/ped.ifp" --dir /tmp/opencode/ifp
//
// The layout, with chunks of <ident> + uint32 <size> and bodies padded to a
// multiple of 4 bytes (the loader reads ROUNDSIZE(size)):
//
//   ANPK <size>
//     INFO <8>          int32 numAnimations, char blockName[]   (blockName = the file stem)
//     per animation:
//       NAME <size>     char name[]
//       DGAN <size>
//         INFO <8>      int32 numSequences, int32 reserved      (reserved is ignored by the loader)
//         per sequence:
//           CPAN <size>
//             ANIM <44>   char name[28], int32 numFrames, int32 0, int32 numFrames-1,
//                         int32 boneTag
//             ANIM <48>   the same, then int32 ignored           (the loader reads no tag: -1)
//             KRTS <size> numFrames * (float rotation[4], float position[3],
//                                      float scale[3], float time)
//             KRT0 <size> numFrames * (float rotation[4], float position[3], float time)
//             KR00 <size> numFrames * (float rotation[4], float time)
//           (no keyframe chunk at all when numFrames is 0: the loader continues)
//
// The file stores the conjugate of every rotation: the loader negates x, y and
// z as it reads. This model stores the rotation the loader ends up with, so a
// content transform works in pose space and the writer conjugates again. The
// key time is absolute, in seconds, and the first key of a cycle is at 0.
//
// The sequence and animation names must fit the loader's fields: it copies a
// name with strncpy(dst, src, 24), so a name is at most 23 bytes, and it reads
// the bone tag into an int16. The block name limit is MAX_ANIMBLOCK_NAME, 20.
//
// The root sequence carries the ground motion of the animation. The loader
// finds it by bone tag 0, or, when the sequence is untagged (the 48-byte ANIM
// form), by the exact name "Root" (CAnimBlendHierarchy::IsRootSequence). Its
// translation is ground motion with +Y forward, and a repeating cycle closes
// when one loop ends where the next begins in every direction but the stride:
// no sideways drift and no vertical drift.

#include <stdint.h>
#include <iosfwd>
#include <string>
#include <vector>

typedef int8_t int8;
typedef uint8_t uint8;
typedef int16_t int16;
typedef uint16_t uint16;
typedef int32_t int32;
typedef uint32_t uint32;

namespace ifp {

// The number of entries of a table, so a table and the count of it never drift
// apart. The one array count of the tool.
template <typename T, int N>
constexpr int CountOf(const T (&)[N])
{
	return N;
}

// printf into a std::string, for the plain one-line messages the tool prints.
std::string Format(const char *fmt, ...);

// The keyframe chunk identities the loader accepts. KEYS_NONE is a sequence
// with no chunk, which is the only form the loader reads for 0 frames.
enum KeyIdent {
	KEYS_NONE = 0,
	KEYS_KR00,	// rotation and time
	KEYS_KRT0,	// rotation, translation and time
	KEYS_KRTS,	// rotation, translation, scale and time
};

const char *KeyIdentName(KeyIdent ident);
KeyIdent KeyIdentFromName(const char *name);
int KeySize(KeyIdent ident);

// One keyframe. rotation is the pose rotation the loader ends up with, time is
// absolute in seconds, scale is the KRTS scaling the loader ignores.
struct Key {
	float rotation[4];
	float translation[3];
	float time;
	float scale[3];
};

// One animated node. numFrames is the count in the ANIM body and keys holds
// what the file stores after it; a valid file has keys.size() == numFrames.
struct Sequence {
	std::string name;
	int32 numFrames;
	// The ANIM body comes in two forms. The 44-byte form carries a bone tag
	// the loader reads (and the sequence binds by that tag); the 48-byte form
	// carries one dword the loader ignores and a sibling dword, so the loader
	// leaves the tag at -1 and the sequence binds by name.
	bool tagged;
	int32 boneTag;		// +40: the loader's tag when tagged, ignored otherwise
	int32 tagSibling;	// +44, 48-byte form only
	int32 animSize;		// the ANIM body size to write (44 or 48 for new content)
	KeyIdent ident;		// the keyframe chunk identity, KEYS_NONE when absent
	bool hasKeyChunk;	// false with KEYS_NONE is the valid zero-frame sequence
	std::vector<Key> keys;

	Sequence(void);

	// What CAnimBlendSequence::boneTag holds after the load (int16 there).
	int EngineBoneTag(void) const;
	bool HasTranslation(void) const;
	// New content: 44-byte ANIM with the tag the loader reads.
	void SetTagged(int32 tag);
	// New content: 48-byte ANIM, untagged, bound by name.
	void SetUntagged(void);
	// New content: replace the keys and the keyframe chunk identity.
	void SetKeys(KeyIdent id, const std::vector<Key> &newKeys);
};

// One animation. reserved is the DGAN INFO body's second dword: the loader
// ignores it, but a writer must preserve what it reads.
struct Hierarchy {
	std::string name;
	int32 reserved;
	std::vector<Sequence> sequences;

	Hierarchy(void);
};

// One block, which is one file. The engine keys the block by this name, so it
// must equal the file stem.
struct Block {
	std::string name;
	std::vector<Hierarchy> hierarchies;

	Block(void);
};

// The loader's per-key numbers: the rotation after the quaternion flip fix and
// the key times as deltas (CAnimBlendHierarchy::CalcTotalTime), plus the
// hierarchy's totalLength and rootSpeed.
struct KeyView {
	float rotation[4];
	float translation[3];
	float deltaTime;
};

struct SequenceView {
	std::string name;
	int engineBoneTag;
	bool hasTranslation;
	std::vector<KeyView> keys;
};

struct HierarchyView {
	std::string name;
	float totalLength;
	float rootSpeed;
	int rootSequence;	// index of the root sequence, -1 when the loader sees none
	std::vector<SequenceView> sequences;
};

HierarchyView ComputeView(const Hierarchy &h);
bool IsRootSequence(const Sequence &seq);
float RootCycleSpeed(const Sequence &seq);

// The index of an animation by name, -1 when the block has no such name. The
// tool's one block lookup: the commands, the validator and the generators name
// their animations through it (file.cpp, so every build links it).
int FindHierarchy(const Block &block, const std::string &name);

// ---------------------------------------------------------------- reading

// A parse result. problems holds one plain line per defect, most of them fatal
// because the loader would desynchronise on it; a fatal parse stops at the
// first defect and leaves the block partial.
struct ReadResult {
	Block block;
	std::vector<std::string> problems;
	size_t fileSize;
	bool fatal;

	ReadResult(void) : fileSize(0), fatal(false) {}
	bool Ok(void) const { return problems.empty(); }
};

ReadResult ParseBlock(const std::vector<uint8> &data);
ReadResult ReadBlock(const std::string &path);
bool ReadBytes(const std::string &path, std::vector<uint8> *data, std::string *error);

// ---------------------------------------------------------------- writing

std::vector<uint8> BuildBlock(const Block &block);
bool WriteBytes(const std::string &path, const std::vector<uint8> &data, std::string *error);
bool WriteBlock(const std::string &path, const Block &block, std::string *error);

// ---------------------------------------------------------------- checking

struct CheckOptions {
	CheckOptions(void) : movement(false), requireRoot(false) {}
	bool movement;		// the block is a movement set: check the five slots
	bool requireRoot;	// every animation must carry a root sequence
};

// Appends one plain line per defect. stem is the file stem the block name must
// equal; pass an empty stem to skip that rule.
void CheckBlock(const Block &block, const std::string &stem, const CheckOptions &options,
	std::vector<std::string> *problems);

// The movement slots in file order, the engine's AnimationId 0..4 of
// aStdAnimDescs (AnimManager.cpp): walk, run, sprint, idle, startwalk. A
// movement block must keep its animations in that order; the name array and the
// description array in the engine's table are index-aligned by construction,
// so an out-of-order file silently gives a slot the wrong behaviour.
struct MovementSlot {
	const char *role;
	bool groundMotion;	// the slot must move the ped
	bool ordered;		// the slot takes part in the walk < run < sprint order
};

extern const MovementSlot MovementSlots[];
extern const int NumMovementSlots;

// ---------------------------------------------------------------- dump

// Human-readable view. only, when non-empty, selects animations by name.
// withKeys adds every key of every sequence.
void DumpBlock(const Block &block, std::vector<std::string> *only, bool withKeys, std::ostream &out);

// ---------------------------------------------------------------- compare

struct CompareResult {
	CompareResult(void) : checks(0), failures(0) {}
	long checks;
	long failures;
	std::vector<std::string> mismatches;	// at most a few, for the report
};

// Compares two blocks key for key as the loader would see them.
void CompareBlocks(const Block &a, const Block &b, CompareResult *result);

// ---------------------------------------------------------------- crouch

// Builds the crouch movement block (VICE-186, tools/ifp/crouch.cpp) out of the
// shipped standing cycles: the crouch pose on the leg and spine bones, the
// crouch body height on the root and a crouch stride. The tables the transform
// reads live in crouch.cpp, the one place to tune the pose.
bool BuildCrouchBlock(const Block &source, Block *out, std::vector<std::string> *problems);

// The movement checks of the block's four directional sets, one set at a time:
// the block's own movement check reads its first five animations only.
bool CheckCrouchSets(const Block &block, std::vector<std::string> *problems);

// One line per set: the five slot names and their root speeds, the ladder the
// presentation picks the tier from.
void CrouchSetSummary(const Block &block, std::ostream &out);

// ---------------------------------------------------------------- planting

// Test facility for the close-out evidence: writes a copy of the input with one
// planted defect, so the validator has something to catch.
extern const char *const PlantDefects[];
extern const int NumPlantDefects;

// Returns false when the name is unknown. note explains what was planted.
bool PlantDefect(const std::string &defect, const Block &input, const std::string &stem,
	const std::string &outPath, std::string *note);

} // namespace ifp

#endif // __TOOLS_IFP_IFP_H__
