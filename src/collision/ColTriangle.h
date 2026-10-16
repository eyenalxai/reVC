#ifndef __GTA_COLTRIANGLE_H__
#define __GTA_COLTRIANGLE_H__

#include "CompressedVector.h"

enum Direction {
	DIR_X_POS,
	DIR_X_NEG,
	DIR_Y_POS,
	DIR_Y_NEG,
	DIR_Z_POS,
	DIR_Z_NEG,
};

struct CColTriangle
{
	uint16 a;
	uint16 b;
	uint16 c;
	uint8 surface;

	void Set(int a, int b, int c, uint8 surf)
	{
		this->a = a;
		this->b = b;
		this->c = c;
		this->surface = surf;
	}
};

struct CColTrianglePlane
{
#ifdef VU_COLLISION
	CompressedVector normal;
	int16 dist;

	void Set(const CVector &va, const CVector &vb, const CVector &vc);
	void Set(const CompressedVector *v, CColTriangle &tri) { Set(v[tri.a].Get(), v[tri.b].Get(), v[tri.c].Get()); }
	void GetNormal(CVector &n) const { n.x = normal.x/4096.0f; n.y = normal.y/4096.0f; n.z = normal.z/4096.0f; }
	float CalcPoint(const CVector &v) const { CVector n; GetNormal(n); return DotProduct(n, v) - dist/128.0f; };
#ifdef GTA_PS2
	void Unpack(uint128 &qword) const {
		__asm__ volatile (
			"lh      $8, 0(%1)\n"
			"lh      $9, 2(%1)\n"
			"lh      $10, 4(%1)\n"
			"lh      $11, 6(%1)\n"
			"pextlw  $10, $8\n"
			"pextlw  $11, $9\n"
			"pextlw  $2, $11, $10\n"
			"sq      $2, %0\n"
			: "=m" (qword)
			: "r" (this)
			: "$8", "$9", "$10", "$11", "$2"
		);
	}
#else
	void Unpack(int32 *qword) const {
		qword[0] = normal.x;
		qword[1] = normal.y;
		qword[2] = normal.z;
		qword[3] = dist;
	}
#endif
#else
	CVector normal;
	float dist;
	uint8 dir;

	void Set(const CVector &va, const CVector &vb, const CVector &vc);
	void Set(const CompressedVector *v, CColTriangle &tri) { Set(v[tri.a].Get(), v[tri.b].Get(), v[tri.c].Get()); }
	void GetNormal(CVector &n) const { n = normal; }
	float GetNormalX() const { return normal.x; }
	float GetNormalY() const { return normal.y; }
	float GetNormalZ() const { return normal.z; }
	float CalcPoint(const CVector &v) const { return DotProduct(normal, v) - dist; };
#endif
};

// Per-triangle data for the sphere tests, see VICE-121. It depends only on
// the vertices and the plane normal, so a colmodel computes it once and
// TestSphereTriangle and ProcessSphereTriangle both reuse it. The values are
// computed with the same expressions in the same order those tests used to
// compute by themselves, so the results are bit-identical.
// vec2 is b-a normalized, vec1 is the in-plane vector orthogonal to vec2 that
// forms the (vec1, vec2, normal) basis, len is |b-a| and c holds the
// coordinates of the third vertex in the (vec1, vec2) basis. The second
// vertex has the coordinates (0, len).
struct CColTriangleBasis
{
	CVector vec1;
	CVector vec2;
	float len;
	CVector2D c;

	// These are the very expressions TestSphereTriangle and
	// ProcessSphereTriangle used to compute for every sphere they were called
	// with. The order matters for the bit-identical result, so do not reorder
	// them. Inline so the fallback (no cache) path compiles to the same code
	// as before.
	void Set(const CompressedVector *verts, const CColTriangle &tri, const CColTrianglePlane &plane)
	{
		const CVector &va = verts[tri.a].Get();
		const CVector &vb = verts[tri.b].Get();
		const CVector &vc = verts[tri.c].Get();

		vec2 = vb - va;
		len = vec2.Magnitude();
		vec2 = vec2 * (1.0f/len);

		CVector normal;
		plane.GetNormal(normal);
		vec1 = CrossProduct(vec2, normal);

		CVector vac = vc - va;
		c = CVector2D(DotProduct(vec1, vac), DotProduct(vec2, vac));
	}
};

// One node of the per-colmodel triangle broad phase. A node bounds a
// contiguous, ascending run of triangles, either directly (leaf) or through
// its two children. The two children of an internal node are adjacent, at
// `child` and `child+1`. Leaves are enumerated in ascending triangle index
// order, which keeps the candidate order of the unaccelerated scan.
struct CColTriangleAccelNode
{
	float min[3];
	float max[3];
	float maxEdge;	// longest edge of any triangle in the subtree
	int32 child;	// internal: first child; leaf: first triangle index
	int32 count;	// internal: 0; leaf: number of triangles in the leaf
};

#endif // __GTA_COLTRIANGLE_H__
