// The IFP reader and writer (tools/ifp/ifp.h). Every read mirrors
// CAnimManager::LoadAnimFile: the same order of chunks, the same rounded body
// sizes, the same type-punned fields, the same conjugation of the rotation and
// the same int16 truncation of the bone tag.

#include "ifp.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <fstream>

namespace ifp {

std::string
Format(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return std::string(buf);
}

static const char *const AnpkName[] = { "ANPK", 0 };
static const char *const InfoName[] = { "INFO", 0 };
static const char *const NameName[] = { "NAME", 0 };
static const char *const DganName[] = { "DGAN", 0 };
static const char *const CpanName[] = { "CPAN", 0 };
static const char *const AnimName[] = { "ANIM", 0 };
static const char *const KeyNames[] = { "KR00", "KRT0", "KRTS", 0 };

static uint32
Round4(uint32 size)
{
	if(size & 3)
		size += 4 - (size & 3);
	return size;
}

const char *
KeyIdentName(KeyIdent ident)
{
	switch(ident){
	case KEYS_NONE: break;
	case KEYS_KR00: return "KR00";
	case KEYS_KRT0: return "KRT0";
	case KEYS_KRTS: return "KRTS";
	}
	return "none";
}

KeyIdent
KeyIdentFromName(const char *name)
{
	if(memcmp(name, "KR00", 4) == 0) return KEYS_KR00;
	if(memcmp(name, "KRT0", 4) == 0) return KEYS_KRT0;
	if(memcmp(name, "KRTS", 4) == 0) return KEYS_KRTS;
	return KEYS_NONE;
}

int
KeySize(KeyIdent ident)
{
	switch(ident){
	case KEYS_NONE: break;
	case KEYS_KR00: return 20;
	case KEYS_KRT0: return 32;
	case KEYS_KRTS: return 44;
	}
	return 0;
}

Sequence::Sequence(void)
{
	numFrames = 0;
	tagged = false;
	boneTag = -1;
	tagSibling = -1;
	animSize = 48;
	ident = KEYS_NONE;
	hasKeyChunk = false;
}

int
Sequence::EngineBoneTag(void) const
{
	// CAnimBlendSequence::SetBoneTag takes an int and the field is an int16.
	return tagged ? (int16)boneTag : -1;
}

bool
Sequence::HasTranslation(void) const
{
	return ident == KEYS_KRT0 || ident == KEYS_KRTS;
}

void
Sequence::SetTagged(int32 tag)
{
	tagged = true;
	boneTag = tag;
	animSize = 44;
}

void
Sequence::SetUntagged(void)
{
	tagged = false;
	boneTag = -1;
	tagSibling = -1;
	animSize = 48;
}

void
Sequence::SetKeys(KeyIdent id, const std::vector<Key> &newKeys)
{
	ident = id;
	keys = newKeys;
	numFrames = (int32)keys.size();
	hasKeyChunk = !keys.empty();
}

Hierarchy::Hierarchy(void)
{
	reserved = 0;
}

Block::Block(void)
{
}

bool
IsRootSequence(const Sequence &seq)
{
	// CAnimBlendHierarchy::IsRootSequence
	if(seq.EngineBoneTag() == 0)
		return true;
	return seq.EngineBoneTag() == -1 && seq.name == "Root";
}

float
RootCycleSpeed(const Sequence &seq)
{
	// CAnimBlendHierarchy::RootCycleSpeed: the net ground displacement of the
	// whole cycle over its own duration, in units per second.
	if(!seq.HasTranslation() || seq.keys.size() < 2)
		return 0.0f;
	const Key &first = seq.keys.front();
	const Key &last = seq.keys.back();
	float duration = last.time - first.time;
	if(duration <= 0.0f)
		return 0.0f;
	float dx = last.translation[0] - first.translation[0];
	float dy = last.translation[1] - first.translation[1];
	return sqrtf(dx*dx + dy*dy) / duration;
}

// ---------------------------------------------------------------- reading

namespace {

// One cursor over the file image. A failure stops the walk: the loader skips
// chunks by the rounded size, so every later byte would be misread.
struct Reader {
	const uint8 *data;
	size_t size;
	size_t offset;		// the next chunk header
	size_t bodyOffset;	// the body of the last chunk header read
	std::string lastIdent;
	std::vector<std::string> *problems;
	bool fatal;

	void Problem(const std::string &what)
	{
		problems->push_back(what);
		fatal = true;
	}
	void Note(const std::string &what)
	{
		problems->push_back(what);
	}
	uint32 U32(size_t at) const
	{
		uint32 v;
		memcpy(&v, data + at, 4);
		return v;
	}
	int32 I32(size_t at) const
	{
		int32 v;
		memcpy(&v, data + at, 4);
		return v;
	}
	float F32(size_t at) const
	{
		float v;
		memcpy(&v, data + at, 4);
		return v;
	}
	const uint8 *Body(void) const { return data + bodyOffset; }

	// Is At(at) the start of a chunk header? Tells an unpadded body from a
	// stray byte.
	bool LooksLikeHeader(size_t at) const
	{
		static const char *const idents[] = {
			"ANPK", "INFO", "NAME", "DGAN", "CPAN", "ANIM", "KR00", "KRT0", "KRTS", 0
		};
		if(at + 4 > size)
			return false;
		for(int i = 0; idents[i]; i++)
			if(memcmp(data + at, idents[i], 4) == 0)
				return true;
		return false;
	}

	// Reads one chunk header and leaves the cursor at its body. wants is a
	// NUL-terminated list of accepted identities. The loader consumes the body
	// of INFO, NAME, ANIM and the keyframe chunks and descends into the body of
	// ANPK, DGAN and CPAN without ever reading their size.
	bool Header(const char *const *wants, uint32 *declared, const std::string &where)
	{
		if(offset + 8 > size){
			Problem(where + ": a chunk header runs past the end of the file");
			return false;
		}
		char ident[5] = { 0 };
		memcpy(ident, data + offset, 4);
		bool known = false;
		for(int i = 0; wants[i]; i++)
			if(strcmp(ident, wants[i]) == 0)
				known = true;
		if(!known){
			std::string expected = wants[0];
			for(int i = 1; wants[i]; i++)
				expected += ", " + std::string(wants[i]);
			Problem(where + ": expected the " + expected + " chunk, found '" + ident + "'");
			return false;
		}
		uint32 size4 = U32(offset + 4);
		lastIdent = ident;
		bodyOffset = offset + 8;
		offset = bodyOffset;
		*declared = size4;
		return true;
	}

	// Reads the rounded body the loader reads, checks its padding and steps over
	// it. The loader reads the rounded size, so the pad is part of the read and
	// a short body moves every following chunk.
	bool Body(uint32 declared, std::vector<uint8> *body, const std::string &where)
	{
		uint32 rounded = Round4(declared);
		if(bodyOffset + rounded > size){
			Problem(where + " " + lastIdent + ": the body is " + std::to_string(declared) +
				" bytes, " + std::to_string(rounded) + " rounded, and the file holds only " +
				std::to_string(size - bodyOffset) + " bytes after the header");
			return false;
		}
		for(uint32 i = declared; i < rounded; i++){
			if(data[bodyOffset + i] != 0){
				if(LooksLikeHeader(bodyOffset + declared))
					Problem(where + " " + lastIdent + ": the body is not padded to 4 bytes, the chunk header '" +
						std::string((const char*)data + bodyOffset + declared, 4) +
						"' starts where the pad belongs");
				else
					Problem(where + " " + lastIdent + ": the body is not padded to 4 bytes, pad byte " +
						std::to_string(i - declared) + " is " + std::to_string(data[bodyOffset + i]));
				return false;
			}
		}
		body->assign(data + bodyOffset, data + bodyOffset + rounded);
		offset = bodyOffset + rounded;
		return true;
	}

	// The name chunks are NUL-terminated strings. The loader reads them into a
	// 256-byte buffer and strncpy's 24 bytes out of it.
	bool Name(const uint8 *raw, uint32 length, const std::string &where, std::string *name)
	{
		if(length > 256){
			Problem(where + ": the name body is " + std::to_string(length) +
				" bytes and the loader reads it into a 256-byte buffer");
			return false;
		}
		size_t len = 0;
		while(len < length && raw[len] != 0)
			len++;
		if(len == length)
			Note(where + ": the name is not NUL-terminated inside its " +
				std::to_string(length) + "-byte body");
		name->assign((const char*)raw, len);
		return true;
	}

	KeyIdent PeekIdent(void) const
	{
		if(offset + 4 > size)
			return KEYS_NONE;
		return KeyIdentFromName((const char*)data + offset);
	}
};

// One sequence: CPAN { ANIM, KR* }, with no keyframe chunk for 0 frames.
bool
ParseSequence(Reader &r, const std::string &hierWhere, int32 index, Sequence *seq)
{
	std::string where = hierWhere + " sequence " + std::to_string(index);
	uint32 declared;
	std::vector<uint8> body;

	if(!r.Header(CpanName, &declared, where))
		return false;
	size_t cpanBody = r.bodyOffset;

	if(!r.Header(AnimName, &declared, where))
		return false;
	if(!r.Body(declared, &body, where))
		return false;
	if(declared < 40){
		r.Problem(where + " ANIM: the body is " + std::to_string(declared) +
			" bytes, the loader reads numFrames at +28 and the last frame at +36");
		return false;
	}
	seq->animSize = (int32)declared;
	// The loader reads the bone tag only from the 44-byte form and leaves it at
	// -1 otherwise, so that sequence binds by name.
	seq->tagged = declared == 44;
	seq->boneTag = declared >= 44 ? r.I32(r.bodyOffset + 40) : -1;
	seq->tagSibling = declared >= 48 ? r.I32(r.bodyOffset + 44) : -1;
	seq->numFrames = r.I32(r.bodyOffset + 28);
	{
		size_t animNameLen = declared < 28 ? declared : 28;
		size_t len = 0;
		while(len < animNameLen && r.Body()[len] != 0)
			len++;
		seq->name.assign((const char*)r.Body(), len);
	}
	where += " \"" + seq->name + "\"";
	if(seq->numFrames < 0){
		r.Problem(where + ": the frame count is " + std::to_string(seq->numFrames));
		return false;
	}

	if(seq->numFrames == 0){
		// The loader continues here without reading a keyframe header, so a
		// chunk at this point is read as the next sequence's CPAN.
		if(r.PeekIdent() != KEYS_NONE){
			r.Problem(where + ": the sequence has 0 frames and a " +
				std::string(KeyIdentName(r.PeekIdent())) +
				" chunk; the loader skips the sequence and reads the chunk as the next one");
			return false;
		}
		return true;
	}

	size_t keysAt = r.offset;
	if(!r.Header(KeyNames, &declared, where))
		return false;
	if(!r.Body(declared, &body, where))
		return false;
	seq->ident = KeyIdentFromName((const char*)r.data + keysAt);
	uint32 expected = (uint32)KeySize(seq->ident) * (uint32)seq->numFrames;
	if(declared != expected){
		r.Problem(where + " " + KeyIdentName(seq->ident) + ": the body is " +
			std::to_string(declared) + " bytes for " + std::to_string(seq->numFrames) +
			" frames, the loader reads " + std::to_string(expected));
		return false;
	}
	int per = KeySize(seq->ident);
	seq->keys.reserve(seq->numFrames);
	for(int32 k = 0; k < seq->numFrames; k++){
		Key key;
		memset(&key, 0, sizeof(key));
		for(int i = 0; i < 4; i++)
			key.rotation[i] = r.F32(r.bodyOffset + k * per + 4 * i);
		// The file stores the conjugate of the pose rotation.
		key.rotation[0] = -key.rotation[0];
		key.rotation[1] = -key.rotation[1];
		key.rotation[2] = -key.rotation[2];
		if(seq->HasTranslation())
			for(int i = 0; i < 3; i++)
				key.translation[i] = r.F32(r.bodyOffset + k * per + 16 + 4 * i);
		if(seq->ident == KEYS_KRTS)
			for(int i = 0; i < 3; i++)
				key.scale[i] = r.F32(r.bodyOffset + k * per + 28 + 4 * i);
		key.time = r.F32(r.bodyOffset + k * per + per - 4);
		for(int i = 0; i < 4; i++)
			if(!(key.rotation[i] == key.rotation[i]))
				r.Note(where + ": key " + std::to_string(k) + " has a non-finite rotation");
		seq->keys.push_back(key);
	}
	seq->hasKeyChunk = true;

	uint32 cpanDeclared = r.U32(cpanBody - 4);
	if(r.offset - cpanBody != cpanDeclared)
		r.Note(where + ": the CPAN size is " + std::to_string(cpanDeclared) +
			" and the sequence spans " + std::to_string(r.offset - cpanBody) + " bytes");
	return true;
}

// One animation: NAME and DGAN { INFO, CPAN... }.
bool
ParseHierarchy(Reader &r, int32 index, Hierarchy *hier)
{
	std::string where = "animation " + std::to_string(index);
	uint32 declared;
	std::vector<uint8> body;

	if(!r.Header(NameName, &declared, where))
		return false;
	if(!r.Body(declared, &body, where))
		return false;
	if(!r.Name(r.Body(), declared, where + " NAME", &hier->name))
		return false;
	where += " \"" + hier->name + "\"";

	size_t dganAt = r.offset;
	if(!r.Header(DganName, &declared, where))
		return false;
	if(!r.Header(InfoName, &declared, where + " DGAN"))
		return false;
	if(!r.Body(declared, &body, where + " DGAN"))
		return false;
	if(declared < 8){
		r.Problem(where + " DGAN INFO: the body is " + std::to_string(declared) +
			" bytes, too short for the sequence count");
		return false;
	}
	int32 numSequences = r.I32(r.bodyOffset);
	hier->reserved = r.I32(r.bodyOffset + 4);
	if(numSequences < 0 || numSequences > 65536){
		r.Problem(where + " DGAN INFO: the sequence count is " + std::to_string(numSequences));
		return false;
	}

	for(int32 s = 0; s < numSequences; s++){
		Sequence seq;
		if(!ParseSequence(r, where, s, &seq))
			return false;
		hier->sequences.push_back(seq);
	}
	uint32 dganDeclared = r.U32(dganAt + 4);
	if(r.offset - (dganAt + 8) != dganDeclared)
		r.Note(where + ": the DGAN size is " + std::to_string(dganDeclared) +
			" and the sequences span " + std::to_string(r.offset - dganAt - 8) + " bytes");
	return true;
}

bool
ParseAnpk(Reader &r, Block *block)
{
	uint32 declared;
	std::vector<uint8> body;

	if(!r.Header(AnpkName, &declared, "anpk"))
		return false;
	// The ANPK body is the whole file. The loader never looks at the size, but
	// a writer that disagrees with itself is worth a line.
	if(declared != r.size - 8)
		r.Note("anpk: the ANPK size is " + std::to_string(declared) + " and the body holds " +
			std::to_string(r.size - 8) + " bytes");

	if(!r.Header(InfoName, &declared, "block"))
		return false;
	if(!r.Body(declared, &body, "block"))
		return false;
	if(declared < 8){
		r.Problem("block INFO: the body is " + std::to_string(declared) +
			" bytes, too short for the animation count and the name");
		return false;
	}
	int32 count = r.I32(r.bodyOffset);
	if(!r.Name(r.Body() + 4, declared - 4, "block", &block->name))
		return false;
	if(count < 0 || count > 1000000){
		r.Problem("block INFO: the animation count is " + std::to_string(count));
		return false;
	}
	for(int32 i = 0; i < count; i++){
		Hierarchy hier;
		if(!ParseHierarchy(r, i, &hier))
			return false;
		block->hierarchies.push_back(hier);
	}
	if(r.offset != r.size)
		r.Note(std::to_string(r.size - r.offset) + " bytes after the last animation");
	return true;
}

} // anonymous

ReadResult
ParseBlock(const std::vector<uint8> &data)
{
	ReadResult result;
	result.fileSize = data.size();
	if(data.empty()){
		result.problems.push_back("the file is empty");
		result.fatal = true;
		return result;
	}
	Reader r;
	r.data = &data[0];
	r.size = data.size();
	r.offset = 0;
	r.bodyOffset = 0;
	r.problems = &result.problems;
	r.fatal = false;
	ParseAnpk(r, &result.block);
	result.fatal = r.fatal;
	return result;
}

bool
ReadBytes(const std::string &path, std::vector<uint8> *data, std::string *error)
{
	std::ifstream in(path.c_str(), std::ios::binary);
	if(!in){
		*error = "cannot open";
		return false;
	}
	in.seekg(0, std::ios::end);
	std::streamoff len = in.tellg();
	if(len < 0){
		*error = "cannot size";
		return false;
	}
	in.seekg(0, std::ios::beg);
	data->resize((size_t)len);
	if(len > 0 && !in.read((char*)&(*data)[0], len)){
		*error = "cannot read";
		return false;
	}
	return true;
}

ReadResult
ReadBlock(const std::string &path)
{
	std::vector<uint8> data;
	std::string error;
	if(!ReadBytes(path, &data, &error)){
		ReadResult result;
		result.problems.push_back(error);
		result.fatal = true;
		return result;
	}
	return ParseBlock(data);
}

// ---------------------------------------------------------------- writing

namespace {

void
PutU32(std::vector<uint8> &out, uint32 v)
{
	out.insert(out.end(), (const uint8*)&v, (const uint8*)&v + 4);
}

void
PutBytes(std::vector<uint8> &out, const void *bytes, size_t count)
{
	const uint8 *p = (const uint8*)bytes;
	out.insert(out.end(), p, p + count);
}

void
PutZeroes(std::vector<uint8> &out, size_t count)
{
	out.insert(out.end(), count, 0);
}

// ident + uint32 size + the body + the zero pad the loader reads past the size.
std::vector<uint8>
Chunk(const char *ident, const std::vector<uint8> &body, uint32 size)
{
	std::vector<uint8> out;
	PutBytes(out, ident, 4);
	PutU32(out, size);
	if(size > 0)
		PutBytes(out, &body[0], size);
	PutZeroes(out, Round4(size) - size);
	return out;
}

std::vector<uint8>
NameChunk(const char *ident, const std::string &name)
{
	std::vector<uint8> body;
	PutBytes(body, name.data(), name.size());
	PutZeroes(body, 1);
	return Chunk(ident, body, (uint32)body.size());
}

int
AnimBodySize(const Sequence &seq)
{
	if(seq.tagged)
		return 44;
	if(seq.animSize == 44)
		return 48;
	return seq.animSize >= 48 ? seq.animSize : 48;
}

std::vector<uint8>
AnimBody(const Sequence &seq)
{
	int size = AnimBodySize(seq);
	std::vector<uint8> body(size, 0);
	size_t nameLen = seq.name.size() < 28 ? seq.name.size() : 28;
	if(nameLen > 0)
		memcpy(&body[0], seq.name.data(), nameLen);
	int32 zero = 0;
	int32 last = seq.numFrames - 1;
	memcpy(&body[28], &seq.numFrames, 4);
	memcpy(&body[32], &zero, 4);
	memcpy(&body[36], &last, 4);
	memcpy(&body[40], &seq.boneTag, 4);
	if(size >= 48)
		memcpy(&body[44], &seq.tagSibling, 4);
	return body;
}

std::vector<uint8>
KeysBody(const Sequence &seq)
{
	int per = KeySize(seq.ident);
	std::vector<uint8> body(per * seq.keys.size(), 0);
	for(size_t k = 0; k < seq.keys.size(); k++){
		const Key &key = seq.keys[k];
		size_t at = k * per;
		// The file stores the conjugate of the pose rotation.
		float rot[4];
		rot[0] = -key.rotation[0];
		rot[1] = -key.rotation[1];
		rot[2] = -key.rotation[2];
		rot[3] = key.rotation[3];
		memcpy(&body[at], rot, 16);
		if(seq.HasTranslation())
			memcpy(&body[at + 16], key.translation, 12);
		if(seq.ident == KEYS_KRTS)
			memcpy(&body[at + 28], key.scale, 12);
		memcpy(&body[at + per - 4], &key.time, 4);
	}
	return body;
}

std::vector<uint8>
CpanChunk(const Sequence &seq)
{
	std::vector<uint8> anim = AnimBody(seq);
	std::vector<uint8> body = Chunk("ANIM", anim, (uint32)anim.size());
	// The loader skips a sequence with no frames before the keyframe header.
	if(seq.ident != KEYS_NONE && (seq.hasKeyChunk || !seq.keys.empty())){
		std::vector<uint8> keys = KeysBody(seq);
		std::vector<uint8> chunk = Chunk(KeyIdentName(seq.ident), keys, (uint32)keys.size());
		body.insert(body.end(), chunk.begin(), chunk.end());
	}
	return body;
}

} // anonymous

std::vector<uint8>
BuildBlock(const Block &block)
{
	std::vector<uint8> info;
	PutU32(info, (uint32)block.hierarchies.size());
	PutBytes(info, block.name.data(), block.name.size());
	PutZeroes(info, 1);
	std::vector<uint8> body = Chunk("INFO", info, (uint32)info.size());

	for(size_t i = 0; i < block.hierarchies.size(); i++){
		const Hierarchy &hier = block.hierarchies[i];
		std::vector<uint8> info2;
		PutU32(info2, (uint32)hier.sequences.size());
		PutU32(info2, (uint32)hier.reserved);
		std::vector<uint8> dgan = Chunk("INFO", info2, (uint32)info2.size());
		for(size_t s = 0; s < hier.sequences.size(); s++){
			std::vector<uint8> cpan = CpanChunk(hier.sequences[s]);
			std::vector<uint8> chunk = Chunk("CPAN", cpan, (uint32)cpan.size());
			dgan.insert(dgan.end(), chunk.begin(), chunk.end());
		}
		std::vector<uint8> name = NameChunk("NAME", hier.name);
		std::vector<uint8> dganChunk = Chunk("DGAN", dgan, (uint32)dgan.size());
		body.insert(body.end(), name.begin(), name.end());
		body.insert(body.end(), dganChunk.begin(), dganChunk.end());
	}

	std::vector<uint8> out;
	PutBytes(out, "ANPK", 4);
	PutU32(out, (uint32)body.size());
	out.insert(out.end(), body.begin(), body.end());
	return out;
}

bool
WriteBytes(const std::string &path, const std::vector<uint8> &data, std::string *error)
{
	std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
	if(!out){
		*error = "cannot open for writing";
		return false;
	}
	if(!data.empty())
		out.write((const char*)&data[0], data.size());
	if(!out){
		*error = "cannot write";
		return false;
	}
	return true;
}

bool
WriteBlock(const std::string &path, const Block &block, std::string *error)
{
	return WriteBytes(path, BuildBlock(block), error);
}

} // namespace ifp
