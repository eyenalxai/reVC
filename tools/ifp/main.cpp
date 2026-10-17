// IFP reader, writer and validator for the movement content (VICE-185).
//
// Standalone: no engine header, no cmake configure, C++17 with c++.
//
//   tools/ifp/build.sh
//
//   build/ifp dump      "<gameRoot>/anim/ped.ifp" WALK_civi
//   build/ifp check     "<gameRoot>/anim/ped.ifp"
//   build/ifp check     /tmp/opencode/ifp/movement.ifp --movement
//   build/ifp write     "<gameRoot>/anim/ped.ifp" /tmp/opencode/ifp/ped.ifp
//   build/ifp roundtrip "<gameRoot>/anim/ped.ifp" --dir /tmp/opencode/ifp
//   build/ifp extract   "<gameRoot>/anim/ped.ifp" /tmp/opencode/ifp/crouch.ifp
//                       WALK_civi=walk_crouch run_civi=run_crouch sprint_civi=sprint_crouch
//                       IDLE_stance=idle_crouch WALK_start=walk_start_crouch
//   build/ifp plant     slotorder /tmp/opencode/ifp/crouch.ifp /tmp/opencode/ifp/crouch.bad.ifp
//
// A file argument may be a game root (a directory holding anim/ped.ifp) or an
// IFP path. Nothing is ever written into the game folder: the writer only
// writes the output path it is given and the round trip uses /tmp/opencode.
//
// The movement slots, in file order (the engine's AnimationId 0..4):
//   walk, run, sprint, idle, startwalk
// and the movement rules a block must keep:
//   * every slot carries a root sequence the loader can find (bone tag 0, or an
//     untagged sequence named exactly "Root"),
//   * walk, run and sprint are cycles: the root has translation keys, at least
//     two frames, the first key at 0, a positive rootSpeed, and a stride that
//     comes back to its start in every direction but the one it walks,
//   * the idle does not translate, and
//   * the walk / run / sprint rootSpeeds increase, the way the engine's
//     aStdAnimDescs ladder expects them.

#include "ifp.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <iostream>
#include <sys/stat.h>
#include <sys/types.h>

using ifp::Block;
using ifp::CheckOptions;

static void
Usage(void)
{
	fprintf(stderr,
		"usage: ifp <command> [arguments]\n"
		"  dump      <file|gameroot> [animation...] [--keys]\n"
		"  check     <file|gameroot> [--movement] [--require-root]\n"
		"  write     <file|gameroot> <out.ifp>\n"
		"  roundtrip <file|gameroot> [--dir <dir>]\n"
		"  extract   <file|gameroot> <out.ifp> <animation>[=<newname>]...\n"
		"  plant     <defect> <file|gameroot> <out.ifp>\n"
		"\nplanted defects:");
	for(int i = 0; i < ifp::NumPlantDefects; i++)
		fprintf(stderr, " %s", ifp::PlantDefects[i]);
	fprintf(stderr, "\n");
}

// A game root holds anim/ped.ifp; anything else is the file path.
static std::string
ResolveInput(const std::string &argument)
{
	struct stat st;
	if(stat(argument.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
		return argument + "/anim/ped.ifp";
	return argument;
}

static std::string
Stem(const std::string &path)
{
	size_t slash = path.find_last_of("/\\");
	std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
	if(base.size() > 4){
		std::string tail = base.substr(base.size() - 4);
		for(size_t i = 0; i < tail.size(); i++)
			tail[i] = (char)tolower((unsigned char)tail[i]);
		if(tail == ".ifp")
			base = base.substr(0, base.size() - 4);
	}
	return base;
}

static std::string
JoinDir(const std::string &dir, const std::string &file)
{
	if(!dir.empty() && dir[dir.size()-1] == '/')
		return dir + file;
	return dir + "/" + file;
}

static void
MakeDirs(const std::string &path)
{
	// Creates every component before the file name. A relative path without a
	// slash, or a path that already exists, needs nothing.
	std::string dir;
	for(size_t i = 0; i < path.size(); i++){
		dir += path[i];
		if(path[i] == '/' && i + 1 < path.size())
			mkdir(dir.c_str(), 0777);
	}
}

static void
PrintProblems(const std::string &path, const std::vector<std::string> &problems)
{
	// A mass defect (times written as deltas, say) must not bury the report.
	size_t shown = problems.size() < 20 ? problems.size() : 20;
	for(size_t i = 0; i < shown; i++)
		fprintf(stderr, "%s: %s\n", path.c_str(), problems[i].c_str());
	if(problems.size() > shown)
		fprintf(stderr, "%s: ... and %d more defects\n", path.c_str(), (int)(problems.size() - shown));
}

struct Counts {
	Counts(void) : animations(0), sequences(0), keys(0) {}
	int animations;
	long sequences;
	long keys;
};

static Counts
Count(const Block &block)
{
	Counts c;
	c.animations = (int)block.hierarchies.size();
	for(size_t i = 0; i < block.hierarchies.size(); i++){
		c.sequences += (long)block.hierarchies[i].sequences.size();
		for(size_t s = 0; s < block.hierarchies[i].sequences.size(); s++)
			c.keys += (long)block.hierarchies[i].sequences[s].keys.size();
	}
	return c;
}

static int
CommandDump(int argc, char **argv)
{
	std::string path = ResolveInput(argv[0]);
	std::vector<std::string> only;
	bool withKeys = false;
	for(int i = 1; i < argc; i++){
		if(strcmp(argv[i], "--keys") == 0){
			withKeys = true;
			continue;
		}
		only.push_back(argv[i]);
	}

	ifp::ReadResult read = ifp::ReadBlock(path);
	if(read.fatal){
		PrintProblems(path, read.problems);
		return 1;
	}
	for(size_t i = 0; i < only.size(); i++)
		if(ifp::FindHierarchy(read.block, only[i]) < 0){
			fprintf(stderr, "%s: no animation \"%s\"\n", path.c_str(), only[i].c_str());
			return 1;
		}
	printf("%s: %d bytes\n", path.c_str(), (int)read.fileSize);
	ifp::DumpBlock(read.block, &only, withKeys, std::cout);
	if(!read.problems.empty()){
		PrintProblems(path, read.problems);
		return 1;
	}
	return 0;
}

static int
CommandCheck(int argc, char **argv)
{
	std::string path = ResolveInput(argv[0]);
	CheckOptions options;
	for(int i = 1; i < argc; i++){
		if(strcmp(argv[i], "--movement") == 0)
			options.movement = true;
		else if(strcmp(argv[i], "--require-root") == 0)
			options.requireRoot = true;
		else{
			fprintf(stderr, "ifp check: unknown argument %s\n", argv[i]);
			return 1;
		}
	}

	ifp::ReadResult read = ifp::ReadBlock(path);
	std::vector<std::string> problems = read.problems;
	if(!read.fatal)
		ifp::CheckBlock(read.block, Stem(path), options, &problems);
	if(!problems.empty()){
		PrintProblems(path, problems);
		fprintf(stderr, "%s: %d defect%s\n", path.c_str(), (int)problems.size(),
			problems.size() == 1 ? "" : "s");
		return 1;
	}
	Counts c = Count(read.block);
	printf("OK: %s: block \"%s\", %d animations, %ld sequences, %ld keys\n", path.c_str(),
		read.block.name.c_str(), c.animations, c.sequences, c.keys);
	if(options.movement)
		printf("OK: %s: the movement slots are walk, run, sprint, idle, startwalk\n", path.c_str());
	if(options.requireRoot)
		printf("OK: %s: every animation carries a root sequence\n", path.c_str());
	return 0;
}

static int
CommandWrite(int argc, char **argv)
{
	if(argc != 2){
		Usage();
		return 1;
	}
	std::string path = ResolveInput(argv[0]);
	std::string out = argv[1];
	ifp::ReadResult read = ifp::ReadBlock(path);
	if(!read.problems.empty()){
		PrintProblems(path, read.problems);
		return 1;
	}
	std::string error;
	if(!ifp::WriteBlock(out, read.block, &error)){
		fprintf(stderr, "%s: %s\n", out.c_str(), error.c_str());
		return 1;
	}
	Counts c = Count(read.block);
	printf("wrote %s (%d animations, %ld sequences, %ld keys)\n", out.c_str(), c.animations,
		c.sequences, c.keys);
	if(Stem(out) != read.block.name)
		fprintf(stderr, "note: the block name is \"%s\" and the file stem is \"%s\"; the engine "
			"keys the block by the body name\n", read.block.name.c_str(), Stem(out).c_str());
	return 0;
}

static int
CommandRoundtrip(int argc, char **argv)
{
	std::string path = ResolveInput(argv[0]);
	std::string dir = "/tmp/opencode/ifp";
	for(int i = 1; i < argc; i++){
		if(strcmp(argv[i], "--dir") == 0 && i + 1 < argc){
			dir = argv[++i];
			continue;
		}
		fprintf(stderr, "ifp roundtrip: unknown argument %s\n", argv[i]);
		return 1;
	}

	std::vector<uint8> original;
	std::string error;
	if(!ifp::ReadBytes(path, &original, &error)){
		fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
		return 1;
	}
	ifp::ReadResult read = ifp::ParseBlock(original);
	if(!read.problems.empty()){
		PrintProblems(path, read.problems);
		fprintf(stderr, "%s: the input does not parse, nothing to round trip\n", path.c_str());
		return 1;
	}

	MakeDirs(dir);
	std::string out = JoinDir(dir, Stem(path) + ".ifp");
	if(!ifp::WriteBytes(out, ifp::BuildBlock(read.block), &error)){
		fprintf(stderr, "%s: %s\n", out.c_str(), error.c_str());
		return 1;
	}
	printf("wrote %s (%d bytes)\n", out.c_str(), (int)original.size());

	std::vector<uint8> written;
	if(!ifp::ReadBytes(out, &written, &error)){
		fprintf(stderr, "%s: %s\n", out.c_str(), error.c_str());
		return 1;
	}
	ifp::ReadResult again = ifp::ParseBlock(written);
	if(!again.problems.empty()){
		PrintProblems(out, again.problems);
		fprintf(stderr, "%s: the written file does not parse\n", out.c_str());
		return 1;
	}

	ifp::CompareResult result;
	ifp::CompareBlocks(read.block, again.block, &result);
	bool byteStable = original == written;
	result.checks++;
	if(!byteStable){
		result.failures++;
		result.mismatches.push_back("the written file is not byte-stable");
	}
	for(size_t i = 0; i < result.mismatches.size(); i++)
		fprintf(stderr, "MISMATCH %s\n", result.mismatches[i].c_str());
	Counts c = Count(read.block);
	printf("RESULT: %s: %s, %d animations, %ld sequences, %ld keys (%ld checks, %ld failures)\n",
		Stem(path).c_str(), byteStable ? "byte-stable" : "the bytes differ", c.animations,
		c.sequences, c.keys, result.checks, result.failures);
	return result.failures == 0 ? 0 : 1;
}

static int
CommandExtract(int argc, char **argv)
{
	if(argc < 3){
		Usage();
		return 1;
	}
	std::string path = ResolveInput(argv[0]);
	std::string out = argv[1];
	ifp::ReadResult read = ifp::ReadBlock(path);
	if(!read.problems.empty()){
		PrintProblems(path, read.problems);
		return 1;
	}

	Block block;
	block.name = Stem(out);
	for(int i = 2; i < argc; i++){
		std::string argument = argv[i];
		std::string before = argument, after;
		size_t equals = argument.find('=');
		if(equals != std::string::npos){
			before = argument.substr(0, equals);
			after = argument.substr(equals + 1);
		}
		int index = ifp::FindHierarchy(read.block, before);
		if(index < 0){
			fprintf(stderr, "%s: no animation \"%s\" of %d\n", path.c_str(), before.c_str(),
				(int)read.block.hierarchies.size());
			return 1;
		}
		ifp::Hierarchy hier = read.block.hierarchies[index];
		if(!after.empty())
			hier.name = after;
		block.hierarchies.push_back(hier);
	}

	std::string error;
	if(!ifp::WriteBlock(out, block, &error)){
		fprintf(stderr, "%s: %s\n", out.c_str(), error.c_str());
		return 1;
	}
	Counts c = Count(block);
	printf("wrote %s (block \"%s\", %d animations, %ld sequences, %ld keys)\n", out.c_str(),
		block.name.c_str(), c.animations, c.sequences, c.keys);
	return 0;
}

static int
CommandPlant(int argc, char **argv)
{
	if(argc != 3){
		Usage();
		return 1;
	}
	std::string defect = argv[0];
	std::string path = ResolveInput(argv[1]);
	std::string out = argv[2];

	bool known = false;
	for(int i = 0; i < ifp::NumPlantDefects; i++)
		if(defect == ifp::PlantDefects[i])
			known = true;
	if(!known){
		fprintf(stderr, "ifp plant: unknown defect \"%s\"; known defects:", defect.c_str());
		for(int i = 0; i < ifp::NumPlantDefects; i++)
			fprintf(stderr, " %s", ifp::PlantDefects[i]);
		fprintf(stderr, "\n");
		return 1;
	}

	ifp::ReadResult read = ifp::ReadBlock(path);
	if(!read.problems.empty()){
		PrintProblems(path, read.problems);
		return 1;
	}
	MakeDirs(out);
	std::string note;
	if(!ifp::PlantDefect(defect, read.block, Stem(out), out, &note)){
		fprintf(stderr, "%s: cannot plant %s: %s\n", path.c_str(), defect.c_str(), note.c_str());
		return 1;
	}
	printf("planted %s in %s: %s\n", defect.c_str(), out.c_str(), note.c_str());
	return 0;
}

int
main(int argc, char **argv)
{
	if(argc < 2){
		Usage();
		return 1;
	}
	const char *command = argv[1];
	if(strcmp(command, "dump") == 0 && argc >= 3)
		return CommandDump(argc - 2, argv + 2);
	if(strcmp(command, "check") == 0 && argc >= 3)
		return CommandCheck(argc - 2, argv + 2);
	if(strcmp(command, "write") == 0 && argc == 4)
		return CommandWrite(argc - 2, argv + 2);
	if(strcmp(command, "roundtrip") == 0 && argc >= 3)
		return CommandRoundtrip(argc - 2, argv + 2);
	if(strcmp(command, "extract") == 0 && argc >= 5)
		return CommandExtract(argc - 2, argv + 2);
	if(strcmp(command, "plant") == 0 && argc == 5)
		return CommandPlant(argc - 2, argv + 2);
	Usage();
	return 1;
}
