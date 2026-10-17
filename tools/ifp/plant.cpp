// Planted defects (tools/ifp/ifp.h). The close-out evidence runs every one of
// these through the validator. Model defects edit the parsed block before it is
// written; byte defects edit the written image, the way a broken writer would.

#include "ifp.h"

#include <string.h>

namespace ifp {

const char *const PlantDefects[] = {
	"blockname",	// the INFO body name does not match the file stem
	"unpadded",	// a chunk body written without its 4-byte pad
	"zeroframe",	// a keyframe chunk on a sequence with 0 frames
	"ident",	// an unknown keyframe chunk identity
	"longname",	// a name at or over the loader's field width
	"noroot",	// the root sequence hidden behind a wrong bone tag
	"missingroot",	// no sequence the loader can see as the root
	"opencycle",	// a movement cycle that drifts sideways
	"nospeed",	// a movement cycle that moves the ped nowhere
	"slotorder",	// the movement slots in the wrong order
	"deltatimes",	// time deltas where the absolute times belong
};
const int NumPlantDefects = (int)(sizeof(PlantDefects)/sizeof(PlantDefects[0]));

static Sequence *
FirstSequence(Block *block)
{
	for(size_t i = 0; i < block->hierarchies.size(); i++)
		for(size_t s = 0; s < block->hierarchies[i].sequences.size(); s++)
			if(!block->hierarchies[i].sequences[s].keys.empty())
				return &block->hierarchies[i].sequences[s];
	return 0;
}

static Sequence *
RootOf(Hierarchy *hier)
{
	for(size_t s = 0; s < hier->sequences.size(); s++)
		if(IsRootSequence(hier->sequences[s]))
			return &hier->sequences[s];
	return 0;
}

static Sequence *
FirstRoot(Block *block)
{
	for(size_t i = 0; i < block->hierarchies.size(); i++){
		Sequence *root = RootOf(&block->hierarchies[i]);
		if(root)
			return root;
	}
	return 0;
}

static uint32
Round4(uint32 size)
{
	if(size & 3)
		size += 4 - (size & 3);
	return size;
}

// Walks the chunk tree in file order without a stack: a container holds its
// children right after its own header, so descending is one step of 8 bytes.
// Returns the offset of the first chunk the walker accepts, or the file size.
static size_t
WalkChunks(const std::vector<uint8> &bytes, bool wantUnpadded, uint32 *chunkSize)
{
	size_t at = 0;
	while(at + 8 <= bytes.size()){
		char ident[5] = { 0 };
		memcpy(ident, &bytes[at], 4);
		uint32 size;
		memcpy(&size, &bytes[at + 4], 4);
		if(wantUnpadded && size % 4 != 0){
			*chunkSize = size;
			return at;
		}
		if(!wantUnpadded && KeyIdentFromName(ident) != KEYS_NONE)
			return at;
		if(strcmp(ident, "ANPK") == 0 || strcmp(ident, "DGAN") == 0 || strcmp(ident, "CPAN") == 0)
			at += 8;
		else
			at += 8 + Round4(size);
	}
	return bytes.size();
}

bool
PlantDefect(const std::string &defect, const Block &input, const std::string &stem,
	const std::string &outPath, std::string *note)
{
	Block block = input;
	bool bytePlant = false;

	if(defect == "blockname"){
		block.name = "planted";
		*note = Format("the INFO body name is \"planted\" and the file is \"%s.ifp\"", stem.c_str());
	}else if(defect == "unpadded"){
		bytePlant = true;
		*note = "a chunk body is written without its 4-byte pad";
	}else if(defect == "ident"){
		bytePlant = true;
		*note = "a keyframe chunk carries the identity KRXY";
	}else if(defect == "zeroframe"){
		Sequence *seq = FirstSequence(&block);
		if(seq == 0)
			return false;
		seq->numFrames = 0;
		*note = Format("sequence \"%s\" keeps its %s chunk with 0 frames", seq->name.c_str(),
			KeyIdentName(seq->ident));
	}else if(defect == "longname"){
		Sequence *seq = FirstSequence(&block);
		if(seq == 0)
			return false;
		seq->name.assign(24, 'x');
		*note = "sequence \"xxxxxxxxxxxxxxxxxxxxxxxx\" is 24 bytes and fills the loader's strncpy";
	}else if(defect == "noroot"){
		Sequence *root = FirstRoot(&block);
		if(root == 0)
			return false;
		root->SetTagged(5);
		*note = Format("the root sequence \"%s\" is tagged 5 (Head)", root->name.c_str());
	}else if(defect == "missingroot"){
		Sequence *root = FirstRoot(&block);
		if(root == 0)
			return false;
		root->SetUntagged();
		root->name = "no-root";
		*note = "the root sequence is renamed \"no-root\" and untagged";
	}else if(defect == "opencycle"){
		if(block.hierarchies.empty())
			return false;
		Sequence *root = RootOf(&block.hierarchies[0]);
		if(root == 0 || root->keys.empty())
			return false;
		root->keys.back().translation[0] += 1.0f;
		*note = Format("movement slot 0 \"%s\": the root cycle drifts 1.0 sideways",
			block.hierarchies[0].name.c_str());
	}else if(defect == "nospeed"){
		if(block.hierarchies.empty())
			return false;
		Sequence *root = RootOf(&block.hierarchies[0]);
		if(root == 0 || root->keys.empty())
			return false;
		for(size_t k = 0; k < root->keys.size(); k++)
			memcpy(root->keys[k].translation, root->keys[0].translation, sizeof(root->keys[0].translation));
		*note = Format("movement slot 0 \"%s\": the root cycle stands still",
			block.hierarchies[0].name.c_str());
	}else if(defect == "slotorder"){
		if(block.hierarchies.size() < 2)
			return false;
		Hierarchy swap = block.hierarchies[0];
		block.hierarchies[0] = block.hierarchies[1];
		block.hierarchies[1] = swap;
		*note = "movement slots 0 and 1 are swapped";
	}else if(defect == "deltatimes"){
		int done = 0;
		for(size_t i = 0; i < block.hierarchies.size(); i++)
			for(size_t s = 0; s < block.hierarchies[i].sequences.size(); s++){
				Sequence &seq = block.hierarchies[i].sequences[s];
				if(seq.keys.size() < 3)
					continue;
				for(size_t k = seq.keys.size(); k-- > 1; )
					seq.keys[k].time -= seq.keys[k-1].time;
				done++;
			}
		if(done == 0)
			return false;
		*note = Format("%d sequences carry the time deltas where the absolute times belong", done);
	}else
		return false;

	std::vector<uint8> bytes = BuildBlock(block);
	if(bytePlant){
		uint32 size = 0;
		size_t at = WalkChunks(bytes, defect == "unpadded", &size);
		if(at >= bytes.size())
			return false;
		if(defect == "unpadded"){
			uint32 rounded = Round4(size);
			bytes.erase(bytes.begin() + at + 8 + size, bytes.begin() + at + 8 + rounded);
			*note = Format("the body at offset %d is %d bytes and its %d pad bytes are missing",
				(int)at, (int)size, (int)(rounded - size));
		}else{
			memcpy(&bytes[at], "KRXY", 4);
			*note = Format("the keyframe chunk at offset %d reads KRXY", (int)at);
		}
	}
	std::string error;
	if(!WriteBytes(outPath, bytes, &error)){
		*note = outPath + ": " + error;
		return false;
	}
	return true;
}

} // namespace ifp
