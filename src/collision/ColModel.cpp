#include "common.h"
#include "ColModel.h"
#include "Collision.h"
#include "Game.h"
#include "MemoryHeap.h"
#include "Pools.h"

// Triangle broad phase, see VICE-121.
// The tree covers contiguous, ascending triangle index ranges, so a query can
// walk it in ascending index order and still skip whole subtrees the sphere
// cannot reach. A leaf holds between TRIANGLE_ACCEL_MIN_SPLIT and
// TRIANGLE_ACCEL_LEAF_SIZE triangles and every split stays inside the middle
// half of its range, which bounds the tree depth and therefore the query
// stack.
static const int32 TRIANGLE_ACCEL_LEAF_SIZE = 8;
static const int32 TRIANGLE_ACCEL_MIN_SPLIT = 4;
static const int32 TRIANGLE_ACCEL_MIN_TRIANGLES = 24;

static int32
TriangleCacheBytes(const CColModel *model)
{
	int32 bytes = 0;
	if(model->trianglePlanes)
		bytes += model->numTriangles * (int32)sizeof(CColTrianglePlane);
	if(model->triangleBasis)
		bytes += model->numTriangles * (int32)sizeof(CColTriangleBasis);
	if(model->triangleAccel)
		bytes += model->numTriangleAccelNodes * (int32)sizeof(CColTriangleAccelNode);
	return bytes;
}

static bool
TriangleAccelIsFinite(float f)
{
	return f == f && f > -1.0e38f && f < 1.0e38f;
}

// Bounds of one triangle. Returns false when the triangle cannot be bounded:
// the sphere tests compute distances in float and a triangle with a
// meaningless normal (degenerate, or a normal that is not near unit) can
// report a collision anywhere near its plane, which no box can bound.
static bool
TriangleAccelTriangleBounds(CColModel &model, int32 index, float *min, float *max, float &maxEdge)
{
	const CColTriangle &tri = model.triangles[index];
	const CColTrianglePlane &plane = model.trianglePlanes[index];
	CVector va = model.vertices[tri.a].Get();
	CVector vb = model.vertices[tri.b].Get();
	CVector vc = model.vertices[tri.c].Get();

	if(!TriangleAccelIsFinite(va.x) || !TriangleAccelIsFinite(va.y) || !TriangleAccelIsFinite(va.z) ||
	   !TriangleAccelIsFinite(vb.x) || !TriangleAccelIsFinite(vb.y) || !TriangleAccelIsFinite(vb.z) ||
	   !TriangleAccelIsFinite(vc.x) || !TriangleAccelIsFinite(vc.y) || !TriangleAccelIsFinite(vc.z))
		return false;

	float m2 = plane.normal.MagnitudeSqr();
	if(!(m2 > 0.25f && m2 < 4.0f))
		return false;

	// The cross product must be well above the rounding noise of its own
	// computation, otherwise the normal direction is noise.
	float crossSq = CrossProduct(vc-va, vb-va).MagnitudeSqr();
	float e0 = (vb-va).MagnitudeSqr();
	float e1 = (vc-va).MagnitudeSqr();
	float e2 = (vc-vb).MagnitudeSqr();
	float edgeSq = Max(e0, Max(e1, e2));
	if(!TriangleAccelIsFinite(crossSq) || !TriangleAccelIsFinite(edgeSq))
		return false;
	if(!(crossSq > 1.0e-12f * edgeSq * edgeSq))
		return false;

	min[0] = Min(va.x, Min(vb.x, vc.x));
	min[1] = Min(va.y, Min(vb.y, vc.y));
	min[2] = Min(va.z, Min(vb.z, vc.z));
	max[0] = Max(va.x, Max(vb.x, vc.x));
	max[1] = Max(va.y, Max(vb.y, vc.y));
	max[2] = Max(va.z, Max(vb.z, vc.z));
	maxEdge = Sqrt(edgeSq);
	return true;
}

static float
TriangleAccelSurfaceArea(const float *min, const float *max)
{
	float dx = max[0] - min[0];
	float dy = max[1] - min[1];
	float dz = max[2] - min[2];
	return 2.0f * (dx*dy + dy*dz + dz*dx);
}

CColModel::CColModel(void)
{
	numSpheres = 0;
	spheres = nil;
	numLines = 0;
	lines = nil;
	numBoxes = 0;
	boxes = nil;
	numTriangles = 0;
	vertices = nil;
	triangles = nil;
	trianglePlanes = nil;
	triangleBasis = nil;
	triangleAccel = nil;
	numTriangleAccelNodes = 0;
	triangleAccelDisabled = false;
	level = LEVEL_GENERIC;	// generic col slot
	ownsCollisionVolumes = true;
}

CColModel::~CColModel(void)
{
	RemoveCollisionVolumes();
}

void*
CColModel::operator new(size_t) throw()
{
	CColModel* node = CPools::GetColModelPool()->New();
	assert(node);
	return node;
}

void
CColModel::operator delete(void *p, size_t) throw()
{
	CPools::GetColModelPool()->Delete((CColModel*)p);
}

void
CColModel::RemoveCollisionVolumes(void)
{
	if(ownsCollisionVolumes){
		RwFree(spheres);
		RwFree(lines);
		RwFree(boxes);
		RwFree(vertices);
		RwFree(triangles);
		RemoveTrianglePlanes();
	}
	numSpheres = 0;
	numLines = 0;
	numBoxes = 0;
	numTriangles = 0;
	spheres = nil;
	lines = nil;
	boxes = nil;
	vertices = nil;
	triangles = nil;
}

// The planes and the basis describe the vertices and triangles they were
// built from and belong to the model until the geometry goes away (see
// VICE-121). The basis is optional: past COLTRI_CACHE_MAX_BYTES it is left
// out and the tests compute it themselves, which gives the same results at
// the speed of the old code.
void
CColModel::CalculateTrianglePlanes(void)
{
	PUSH_MEMID(MEMID_COLLISION);

	trianglePlanes = (CColTrianglePlane*)RwMalloc(sizeof(CColTrianglePlane) * numTriangles);
	REGISTER_MEMPTR(&trianglePlanes);
	for(int i = 0; i < numTriangles; i++)
		trianglePlanes[i].Set(vertices, triangles[i]);

	CCollision::ms_numCachedTriangles += numTriangles;

	// The basis is optional. Its check counts the planes that were just
	// allocated, so the cap really covers planes + basis + trees. The VU path
	// has its own compressed plane format and does not use the basis.
#ifndef VU_COLLISION
	if(CCollision::ms_numCachedBytes + numTriangles*(int32)(sizeof(CColTrianglePlane) + sizeof(CColTriangleBasis))
			<= COLTRI_CACHE_MAX_BYTES){
		triangleBasis = (CColTriangleBasis*)RwMalloc(sizeof(CColTriangleBasis) * numTriangles);
		REGISTER_MEMPTR(&triangleBasis);
		for(int i = 0; i < numTriangles; i++)
			triangleBasis[i].Set(vertices, triangles[i], trianglePlanes[i]);
	}
#endif

	CCollision::ms_numCachedBytes += TriangleCacheBytes(this);

	POP_MEMID();
}

void
CColModel::CalculateTriangleAccel(void)
{
	if(triangleAccel || triangleAccelDisabled)
		return;
	// Below this a full scan is cheaper than walking the tree.
	if(numTriangles < TRIANGLE_ACCEL_MIN_TRIANGLES || trianglePlanes == nil){
		triangleAccelDisabled = true;
		return;
	}
	// Every leaf holds at least TRIANGLE_ACCEL_MIN_SPLIT triangles, so half
	// as many nodes as triangles is an upper bound.
	int32 maxNodes = numTriangles/2;
	if(CCollision::ms_numCachedBytes + maxNodes*(int32)sizeof(CColTriangleAccelNode) > COLTRI_CACHE_MAX_BYTES){
		triangleAccelDisabled = true;
		return;
	}

	PUSH_MEMID(MEMID_COLLISION);
	CColTriangleAccelNode *nodes = (CColTriangleAccelNode*)RwMalloc(maxNodes * sizeof(CColTriangleAccelNode));

	struct ScratchTri
	{
		float min[3];	// prefix bounds over triangles [range.lo, i]
		float max[3];
		float maxEdge;
	};
	ScratchTri *scratch = (ScratchTri*)RwMalloc(numTriangles * sizeof(ScratchTri));

	if(nodes == nil || scratch == nil){
		RwFree(nodes);
		RwFree(scratch);
		POP_MEMID();
		triangleAccelDisabled = true;
		return;
	}

	struct Range
	{
		int32 lo, hi, node;
	};
	Range stack[TRIANGLE_ACCEL_MAX_DEPTH];
	int32 sp = 0;
	int32 numNodes = 1;
	stack[sp++] = Range{0, numTriangles, 0};

	bool ok = true;
	while(sp > 0){
		Range range = stack[--sp];

		// prefix bounds and the node bounds in one left-to-right pass
		float bmin[3] = {1.0e38f, 1.0e38f, 1.0e38f};
		float bmax[3] = {-1.0e38f, -1.0e38f, -1.0e38f};
		float nodeMaxEdge = 0.0f;
		for(int32 i = range.lo; i < range.hi; i++){
			float tmin[3], tmax[3], maxEdge;
			if(!TriangleAccelTriangleBounds(*this, i, tmin, tmax, maxEdge)){
				ok = false;
				break;
			}
			for(int a = 0; a < 3; a++){
				bmin[a] = Min(bmin[a], tmin[a]);
				bmax[a] = Max(bmax[a], tmax[a]);
				scratch[i].min[a] = bmin[a];
				scratch[i].max[a] = bmax[a];
			}
			nodeMaxEdge = Max(nodeMaxEdge, maxEdge);
			scratch[i].maxEdge = nodeMaxEdge;
		}
		if(!ok)
			break;

		CColTriangleAccelNode &node = nodes[range.node];
		node.min[0] = bmin[0]; node.min[1] = bmin[1]; node.min[2] = bmin[2];
		node.max[0] = bmax[0]; node.max[1] = bmax[1]; node.max[2] = bmax[2];
		node.maxEdge = nodeMaxEdge;

		if(range.hi - range.lo <= TRIANGLE_ACCEL_LEAF_SIZE){
			node.child = range.lo;
			node.count = range.hi - range.lo;
			continue;
		}

		// pick the split that minimizes the surface area heuristic cost.
		// Both sides keep at least a quarter of the range so the tree stays
		// shallow; on equal cost the middle split wins.
		int32 side = Max(TRIANGLE_ACCEL_MIN_SPLIT, (range.hi - range.lo)/4);
		int32 best = (range.lo + range.hi)/2;
		float bestCost = 1.0e38f;
		float smin[3] = {1.0e38f, 1.0e38f, 1.0e38f};
		float smax[3] = {-1.0e38f, -1.0e38f, -1.0e38f};
		for(int32 mid = range.hi - side; mid >= range.lo + side; mid--){
			float tmin[3], tmax[3], maxEdge;
			TriangleAccelTriangleBounds(*this, mid, tmin, tmax, maxEdge);
			for(int a = 0; a < 3; a++){
				smin[a] = Min(smin[a], tmin[a]);
				smax[a] = Max(smax[a], tmax[a]);
			}
			float cost = TriangleAccelSurfaceArea(scratch[mid-1].min, scratch[mid-1].max) * (mid - range.lo)
			           + TriangleAccelSurfaceArea(smin, smax) * (range.hi - mid);
			if(cost < bestCost){
				bestCost = cost;
				best = mid;
			}
		}

		assert(best > range.lo && best < range.hi);
		assert(sp + 2 <= TRIANGLE_ACCEL_MAX_DEPTH);
		assert(numNodes + 2 <= maxNodes);
		int32 child = numNodes;
		node.child = child;
		node.count = 0;
		numNodes += 2;
		stack[sp++] = Range{best, range.hi, child + 1};
		stack[sp++] = Range{range.lo, best, child};
	}

	RwFree(scratch);

	if(!ok){
		RwFree(nodes);
		POP_MEMID();
		triangleAccelDisabled = true;
		return;
	}

	triangleAccel = nodes;
	numTriangleAccelNodes = numNodes;
	CCollision::ms_numCachedBytes += numNodes * (int32)sizeof(CColTriangleAccelNode);
	POP_MEMID();
}

void
CColModel::RemoveTrianglePlanes(void)
{
	if(trianglePlanes)
		CCollision::ms_numCachedTriangles -= numTriangles;
	CCollision::ms_numCachedBytes -= TriangleCacheBytes(this);

	RwFree(trianglePlanes);
	RwFree(triangleBasis);
	RwFree(triangleAccel);
	trianglePlanes = nil;
	triangleBasis = nil;
	triangleAccel = nil;
	numTriangleAccelNodes = 0;
	triangleAccelDisabled = false;
}

void
CColModel::GetTrianglePoint(CVector &v, int i) const
{
	v = vertices[i].Get();
}

CColModel&
CColModel::operator=(const CColModel &other)
{
	int i;
	int numVerts;

	// The per-triangle caches describe the geometry they were built from,
	// which this assignment replaces. The old code kept stale planes here.
	RemoveTrianglePlanes();

	boundingSphere = other.boundingSphere;
	boundingBox = other.boundingBox;

	// copy spheres
	if(other.numSpheres){
		if(numSpheres != other.numSpheres){
			numSpheres = other.numSpheres;
			if(spheres)
				RwFree(spheres);
			spheres = (CColSphere*)RwMalloc(numSpheres*sizeof(CColSphere));
		}
		for(i = 0; i < numSpheres; i++)
			spheres[i] = other.spheres[i];
	}else{
		numSpheres = 0;
		if(spheres)
			RwFree(spheres);
		spheres = nil;
	}

	// copy lines
	if(other.numLines){
		if(numLines != other.numLines){
			numLines = other.numLines;
			if(lines)
				RwFree(lines);
			lines = (CColLine*)RwMalloc(numLines*sizeof(CColLine));
		}
		for(i = 0; i < numLines; i++)
			lines[i] = other.lines[i];
	}else{
		numLines = 0;
		if(lines)
			RwFree(lines);
		lines = nil;
	}

	// copy boxes
	if(other.numBoxes){
		if(numBoxes != other.numBoxes){
			numBoxes = other.numBoxes;
			if(boxes)
				RwFree(boxes);
			boxes = (CColBox*)RwMalloc(numBoxes*sizeof(CColBox));
		}
		for(i = 0; i < numBoxes; i++)
			boxes[i] = other.boxes[i];
	}else{
		numBoxes = 0;
		if(boxes)
			RwFree(boxes);
		boxes = nil;
	}

	// copy mesh
	if(other.numTriangles){
		// copy vertices
		numVerts = 0;
		for(i = 0; i < other.numTriangles; i++){
			if(other.triangles[i].a > numVerts)
				numVerts = other.triangles[i].a;
			if(other.triangles[i].b > numVerts)
				numVerts = other.triangles[i].b;
			if(other.triangles[i].c > numVerts)
				numVerts = other.triangles[i].c;
		}
		numVerts++;
		if(vertices)
			RwFree(vertices);
		if(numVerts){
			vertices = (CompressedVector*)RwMalloc(numVerts*sizeof(CompressedVector));
			for(i = 0; i < numVerts; i++)
				vertices[i] = other.vertices[i];
		}

		// copy triangles
		if(numTriangles != other.numTriangles){
			numTriangles = other.numTriangles;
			if(triangles)
				RwFree(triangles);
			triangles = (CColTriangle*)RwMalloc(numTriangles*sizeof(CColTriangle));
		}
		for(i = 0; i < numTriangles; i++)
			triangles[i] = other.triangles[i];
	}else{
		numTriangles = 0;
		if(triangles)
			RwFree(triangles);
		triangles = nil;
		if(vertices)
			RwFree(vertices);
		vertices = nil;
	}
	return *this;
}
