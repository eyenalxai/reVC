// Candidate-list checker for the VICE-121 per-colmodel triangle broad phase.
//
// For every collision model it compares the pruned candidate list of
// CCollision::CollectSphereTriangles (which walks the per-colmodel
// acceleration tree when the model has one) against the plain ascending scan
// over the same model with the same TestSphereTriangle predicate. The lists
// must be equal element for element, and the pruned list must be ascending.
// This is the property the collision result depends on: ProcessColModels
// keeps the strict minimum distance and keeps the first candidate on ties.
//
// The checker reads the same files the game does and applies the same
// layout, then drops the models whose declared size or index data is broken
// (the game's loader has no bounds checks and misparses them; their runtime
// behaviour is not a function of the file).
//
// Build (from the repository root, after a cmake configure so that
// build/compile_commands.json exists):
//     tools/collision_check/build.sh
// Run:
//     build/collision_check <gameRoot> [queriesPerModel] [seed]
// where <gameRoot> is the GTA Vice City directory (it contains data/ and
// models/). Exit code is non-zero when any candidate list differs.

#include "common.h"
#include "Collision.h"
#include "ColModel.h"
#include "Game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>
#include <algorithm>
#include <dirent.h>

// ---------------------------------------------------------------- stubs
// The collision files need the allocator and the assert hook from the game.
// The matrices here never attach an RwMatrix, so those two are unreachable.

void *RwMalloc(size_t size) { return malloc(size); }
void RwFree(void *mem) { free(mem); }
RwBool RwMatrixDestroy(RwMatrix*) { return true; }
RwMatrix *RwMatrixUpdate(RwMatrix*) { return nil; }

void
re3_assert(const char *expr, const char *filename, unsigned int lineno, const char *func)
{
	fprintf(stderr, "assertion failed: %s at %s:%u (%s)\n", expr, filename, lineno, func);
	abort();
}

// ---------------------------------------------------------------- rng

struct Rng
{
	uint32 state;
	void Seed(uint32 s) { state = s ? s : 0x9e3779b9u; }
	uint32 Next(void)
	{
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		return state;
	}
	float Float(void) { return (Next() >> 8) * (1.0f/16777216.0f); }
	float Range(float lo, float hi) { return lo + (hi-lo)*Float(); }
	int Int(int n) { return (int)(Next() % (uint32)n); }
};

// ---------------------------------------------------------------- models

struct TestModel
{
	std::string name;
	CColModel col;
	std::vector<CompressedVector> vertices;
	std::vector<CColTriangle> triangles;
};

static void
CommitModel(TestModel &m)
{
	m.col.vertices = m.vertices.empty() ? nil : &m.vertices[0];
	m.col.triangles = m.triangles.empty() ? nil : &m.triangles[0];
	m.col.numTriangles = m.triangles.size();
	m.col.ownsCollisionVolumes = false;
	m.col.level = LEVEL_GENERIC;
}

// layouts of CFileLoader::LoadCollisionModel; the sections the checker does
// not need are skipped
static bool
ParseColBuffer(const uint8 *buf, uint32 size, TestModel &m)
{
	if(size < 44)
		return false;
	int numSpheres = *(int16*)(buf+40);
	uint32 off = 44;
	if(numSpheres < 0 || off + 20*(uint32)numSpheres > size) return false;
	off += 20*numSpheres;

	if(off + 4 > size) return false;
	int numLines = *(int16*)(buf+off); off += 4;
	if(numLines < 0 || off + 24*(uint32)numLines > size) return false;
	off += 24*numLines;

	if(off + 4 > size) return false;
	int numBoxes = *(int16*)(buf+off); off += 4;
	if(numBoxes < 0 || off + 28*(uint32)numBoxes > size) return false;
	off += 28*numBoxes;

	if(off + 4 > size) return false;
	int numVertices = *(int16*)(buf+off); off += 4;
	if(numVertices < 0 || off + 12*(uint32)numVertices > size) return false;
	for(int i = 0; i < numVertices; i++){
		CompressedVector v;
		v.Set(*(float*)(buf+off), *(float*)(buf+off+4), *(float*)(buf+off+8));
		m.vertices.push_back(v);
		off += 12;
	}

	if(off + 4 > size) return false;
	int numTriangles = *(int16*)(buf+off); off += 4;
	if(numTriangles < 0 || off + 16*(uint32)numTriangles > size) return false;
	for(int i = 0; i < numTriangles; i++){
		CColTriangle t;
		t.a = *(int32*)(buf+off);
		t.b = *(int32*)(buf+off+4);
		t.c = *(int32*)(buf+off+8);
		t.surface = buf[off+12];
		if(t.a >= m.vertices.size() || t.b >= m.vertices.size() || t.c >= m.vertices.size())
			return false;
		m.triangles.push_back(t);
		off += 16;
	}
	return true;
}

static void
ScanColFiles(const std::string &dir, std::vector<std::string> &files)
{
	DIR *d = opendir(dir.c_str());
	if(d == nil)
		return;
	struct dirent *e;
	while((e = readdir(d)) != nil){
		if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		std::string p = dir + "/" + e->d_name;
		if(e->d_type == DT_DIR)
			ScanColFiles(p, files);
		else if(e->d_type == DT_REG && p.size() > 4 && p.compare(p.size()-4, 4, ".col") == 0)
			files.push_back(p);
	}
	closedir(d);
}

static bool
LoadColFile(const char *path, std::vector<TestModel> &models, int &dropped)
{
	FILE *f = fopen(path, "rb");
	if(f == nil)
		return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8> data(size);
	if(fread(&data[0], 1, size, f) != (size_t)size){ fclose(f); return false; }
	fclose(f);

	uint32 pos = 0;
	while(pos + 8 <= (uint32)size){
		if(memcmp(&data[pos], "COLL", 4) != 0)
			break;
		uint32 blocksize = *(uint32*)&data[pos+4];
		if(blocksize < 24 || pos + 8 + blocksize > (uint32)size)
			break;
		TestModel m;
		m.name.assign((const char*)&data[pos+8], strnlen((const char*)&data[pos+8], 24));
		if(ParseColBuffer(&data[pos+32], blocksize-24, m))
			models.push_back(m);
		else{
			fprintf(stderr, "dropped malformed colmodel %s from %s\n", m.name.c_str(), path);
			dropped++;
		}
		pos += 8 + blocksize;
	}
	return true;
}

static bool
LoadModels(const std::string &root, std::vector<TestModel> &models, int &dropped)
{
	std::vector<std::string> files;
	ScanColFiles(root + "/data/maps", files);
	std::sort(files.begin(), files.end());
	std::vector<std::string> coll;
	ScanColFiles(root + "/models/coll", coll);
	std::sort(coll.begin(), coll.end());
	files.insert(files.end(), coll.begin(), coll.end());
	for(size_t i = 0; i < files.size(); i++)
		LoadColFile(files[i].c_str(), models, dropped);
	for(size_t i = 0; i < models.size(); i++)
		CommitModel(models[i]);
	return true;
}

// ---------------------------------------------------------------- check

static const int MAX_CANDIDATES = 512;	// ProcessColModels asserts at 600

static bool
ListsEqual(const int32 *a, int32 an, const int32 *b, int32 bn)
{
	if(an != bn)
		return false;
	for(int32 i = 0; i < an; i++)
		if(a[i] != b[i])
			return false;
	return true;
}

int
main(int argc, char **argv)
{
	if(argc < 2){
		fprintf(stderr, "usage: %s <gameRoot> [queriesPerModel] [seed]\n", argv[0]);
		return 1;
	}
	std::string root = argv[1];
	int queriesPerModel = argc > 2 ? atoi(argv[2]) : 200;
	uint32 seed = argc > 3 ? (uint32)strtoul(argv[3], nil, 0) : 0xC0FFEE;

	std::vector<TestModel> models;
	int dropped = 0;
	if(!LoadModels(root, models, dropped)){
		fprintf(stderr, "cannot read models under %s\n", root.c_str());
		return 1;
	}
	fprintf(stderr, "loaded %d models (%d dropped, malformed)\n", (int)models.size(), dropped);

	Rng rng;
	rng.Seed(seed);

	// the tree can only append triangles the same predicate accepts on a
	// subset of the model, so its list never exceeds the plain one, and the
	// plain one is bounded here
	std::vector<int32> treeList(MAX_CANDIDATES);
	std::vector<int32> plainList(MAX_CANDIDATES);
	long long queries = 0, compared = 0, skipped = 0, mismatches = 0, treeCandidates = 0, plainCandidates = 0;
	int swept = 0, withTree = 0;

	for(size_t mi = 0; mi < models.size(); mi++){
		TestModel &m = models[mi];
		if(m.col.numTriangles == 0)
			continue;
		swept++;
		CCollision::CalculateTrianglePlanes(&m.col);
		m.col.CalculateTriangleAccel();
		if(m.col.triangleAccel)
			withTree++;
		CVector lo = m.col.boundingBox.min;
		CVector hi = m.col.boundingBox.max;

		for(int k = 0; k < queriesPerModel; k++){
			queries++;
			CColSphere sphere;
			static const float radii[] = { 0.25f, 0.5f, 1.25f, 3.0f, 8.0f };
			sphere.radius = radii[rng.Int(ARRAY_SIZE(radii))];
			sphere.surface = 0;
			sphere.piece = 0;
			if(rng.Int(2) == 0){
				// anywhere around the box
				sphere.center.x = rng.Range(lo.x - 3*sphere.radius, hi.x + 3*sphere.radius);
				sphere.center.y = rng.Range(lo.y - 3*sphere.radius, hi.y + 3*sphere.radius);
				sphere.center.z = rng.Range(lo.z - 3*sphere.radius, hi.z + 3*sphere.radius);
			}else{
				// on a triangle, around the distance where the answer flips
				int ti = rng.Int(m.col.numTriangles);
				const CColTriangle &t = m.triangles[ti];
				CVector a = m.col.vertices[t.a].Get();
				CVector b = m.col.vertices[t.b].Get();
				CVector c = m.col.vertices[t.c].Get();
				float u = rng.Float(), v = rng.Float();
				if(u + v > 1.0f){ u = 1.0f - u; v = 1.0f - v; }
				CVector p = a + (b-a)*u + (c-a)*v;
				CVector n = m.col.trianglePlanes[ti].normal;
				float d = sphere.radius * rng.Range(0.9f, 1.1f);
				sphere.center = p + n*d;
			}

			// the plain scan first, with the same predicate; queries whose
			// list would not fit are out of the game's domain (ProcessColModels
			// asserts above 600) and are skipped before the tree runs
			int32 plainNum = 0;
			for(int i = 0; i < m.col.numTriangles; i++){
				const CColTriangleBasis *basis = m.col.triangleBasis ? &m.col.triangleBasis[i] : nil;
				if(CCollision::TestSphereTriangle(sphere, m.col.vertices, m.col.triangles[i], m.col.trianglePlanes[i], basis)){
					if(plainNum < (int32)plainList.size())
						plainList[plainNum] = i;
					plainNum++;
				}
			}
			if(plainNum > MAX_CANDIDATES){
				skipped++;
				continue;
			}

			int32 treeNum = CCollision::CollectSphereTriangles(sphere, m.col, &treeList[0]);
			bool ascending = true;
			for(int32 i = 1; i < treeNum; i++)
				if(treeList[i-1] >= treeList[i])
					ascending = false;
			if(!ascending || !ListsEqual(&treeList[0], treeNum, &plainList[0], plainNum)){
				mismatches++;
				fprintf(stderr, "MISMATCH model %s query %d: tree %d plain %d%s\n",
					m.name.c_str(), k, treeNum, plainNum, ascending ? "" : " (not ascending)");
				if(mismatches < 5){
					fprintf(stderr, "  tree :");
					for(int32 i = 0; i < treeNum && i < 32; i++) fprintf(stderr, " %d", treeList[i]);
					fprintf(stderr, "\n  plain:");
					for(int32 i = 0; i < plainNum && i < 32; i++) fprintf(stderr, " %d", plainList[i]);
					fprintf(stderr, "\n");
				}
			}
			compared++;
			treeCandidates += treeNum;
			plainCandidates += plainNum;
		}
	}

	fprintf(stderr, "models swept %d (with acceleration tree %d)\n", swept, withTree);
	fprintf(stderr, "queries %lld, compared %lld (%lld skipped: more than %d candidates)\n",
		queries, compared, skipped, MAX_CANDIDATES);
	fprintf(stderr, "candidates: tree %lld, plain %lld\n", treeCandidates, plainCandidates);
	fprintf(stderr, "mismatches: %lld\n", mismatches);
	return mismatches != 0;
}
