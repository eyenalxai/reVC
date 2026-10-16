#ifndef __GTA_VISIBILITYPLUGINS_H__
#define __GTA_VISIBILITYPLUGINS_H__

#include "templates.h"

class CEntity;
class CSimpleModelInfo;
class CClumpModelInfo;

class CVisibilityPlugins
{
public:
	struct AlphaObjectInfo
	{
		union {
			CEntity *entity;
			RpAtomic *atomic;
		};
		float sort;
	};

	static CLinkList<AlphaObjectInfo> m_alphaList;
	static CLinkList<AlphaObjectInfo> m_alphaBoatAtomicList;
	static CLinkList<AlphaObjectInfo> m_alphaEntityList;
	static CLinkList<AlphaObjectInfo> m_alphaUnderwaterEntityList;
#ifdef NEW_RENDERER
	static CLinkList<AlphaObjectInfo> m_alphaBuildingList;
#endif
	static RwV3d *ms_pCameraPosn;
	static float ms_cullCompsDist;
	static float ms_vehicleLod0Dist;
	static float ms_vehicleLod1Dist;
	static float ms_vehicleFadeDist;
	static float ms_bigVehicleLod0Dist;
	static float ms_bigVehicleLod1Dist;
	static float ms_pedLod1Dist;
	static float ms_pedFadeDist;

	static void Initialise(void);
	static void Shutdown(void);
	static void InitAlphaEntityList(void);
	static bool InsertEntityIntoSortedList(CEntity *e, float dist);
	static void InitAlphaAtomicList(void);
	static bool InsertAtomicIntoSortedList(RpAtomic *a, float dist);
	static bool InsertAtomicIntoBoatSortedList(RpAtomic *a, float dist);

	static void SetRenderWareCamera(RwCamera *camera);
	static void SetupVehicleVariables(RpClump *vehicle);

	static RpAtomic *RenderWheelAtomicCB(RpAtomic *atomic);
	static RpAtomic *RenderAlphaAtomic(RpAtomic *atomic, int alpha);
	static RpAtomic *RenderWeaponCB(RpAtomic *atomic);
	static RpAtomic *RenderFadingAtomic(RpAtomic *atm, float dist);

	// The vehicle and train detail atomics are one rule: a LOD distance bounds
	// the atomic, an optional camera-facing cull hides it, and the alpha
	// atomics enter a sorted list instead of drawing straight away.
	enum eVehicleAtomicCull
	{
		VEHICLEATOMIC_CULL_NONE,	// no cull test
		VEHICLEATOMIC_CULL_ANGLE,	// cull on dot > 0 && (ANGLECULL || 0.1*distSq < dot*dot)
		VEHICLEATOMIC_CULL_DOT		// cull on dot > 0 only
	};
	enum eVehicleAtomicSort
	{
		VEHICLEATOMIC_SORT_NONE,		// draw now
		VEHICLEATOMIC_SORT_DRAWLAST_EPS,	// DRAWLAST key distSq - 0.0001f (before the clump); else distSq + dot
		VEHICLEATOMIC_SORT_DOT,			// key distSq + dot
		VEHICLEATOMIC_SORT_BOATLIST,		// DRAWLAST key distSq into the boat list; else draw
		VEHICLEATOMIC_SORT_DRAWLAST		// DRAWLAST key distSq; else distSq + dot
	};
	static RpAtomic *RenderVehicleAtomicCB(RpAtomic *atomic, float maxDistSq,
		eVehicleAtomicCull cullTest, eVehicleAtomicSort sortRule);

	static RpAtomic *RenderVehicleHiDetailCB(RpAtomic *atomic);
	static RpAtomic *RenderVehicleHiDetailAlphaCB(RpAtomic *atomic);
	static RpAtomic *RenderVehicleHiDetailCB_BigVehicle(RpAtomic *atomic);
	static RpAtomic *RenderVehicleHiDetailAlphaCB_BigVehicle(RpAtomic *atomic);
	static RpAtomic *RenderVehicleHiDetailCB_Boat(RpAtomic *atomic);
	static RpAtomic *RenderVehicleHiDetailAlphaCB_Boat(RpAtomic *atomic);
	static RpAtomic *RenderVehicleLoDetailCB_Boat(RpAtomic *atomic);
	static RpAtomic *RenderVehicleLowDetailCB_BigVehicle(RpAtomic *atomic);
	static RpAtomic *RenderVehicleLowDetailAlphaCB_BigVehicle(RpAtomic *atomic);
	static RpAtomic *RenderVehicleReallyLowDetailCB(RpAtomic *atomic);
	static RpAtomic *RenderVehicleReallyLowDetailCB_BigVehicle(RpAtomic *atomic);
	static RpAtomic *RenderTrainHiDetailCB(RpAtomic *atomic);
	static RpAtomic *RenderTrainHiDetailAlphaCB(RpAtomic *atomic);
	static RpAtomic *RenderVehicleRotorAlphaCB(RpAtomic *atomic);
	static RpAtomic *RenderVehicleTailRotorAlphaCB(RpAtomic *atomic);

	static RpAtomic *RenderPlayerCB(RpAtomic *atomic);
	static RpAtomic *RenderPedCB(RpAtomic *atomic);	// for skinned models with only one clump

	static void RenderAtomicList(CLinkList<AlphaObjectInfo> &list);
	static void RenderAlphaAtomics(void);
	static void RenderBoatAlphaAtomics(void);
	static void RenderFadingEntities(CLinkList<AlphaObjectInfo> &list);
	static void RenderFadingEntities(void);
	static void RenderFadingUnderwaterEntities(void);

	static float GetDistanceSquaredFromCamera(RwV3d *pos);
	static float GetDistanceSquaredFromCamera(RwFrame *frame);
	static float GetDotProductWithCameraVector(RwMatrix *atomicMat, RwMatrix *clumpMat, uint32 flags);

	//
	// RW Plugins
	//

	struct AtomicExt
	{
		// The simple model the loader split the atomic into; nil for the
		// atomics that stay in their clump.
		CSimpleModelInfo *modelInfo;
		// The ATOMIC_FLAG_* bits of a vehicle part; zero until one is set.
		uint32 flags;
	};
	static AtomicExt *GetAtomicExt(RpAtomic *atomic);
	static void SetAtomicRenderCallback(RpAtomic*, RpAtomicCallBackRender);

	static void *AtomicConstructor(void *object, int32 offset, int32 len);
	static void *AtomicDestructor(void *object, int32 offset, int32 len);
	static void *AtomicCopyConstructor(void *dst, const void *src,
	                                   int32 offset, int32 len);
	static int32 ms_atomicPluginOffset;

	struct FrameExt
	{
		// BUG: this is abused to hold a pointer by SetClumpModelInfo
		intptr id;
	};
	static void SetFrameHierarchyId(RwFrame *frame, intptr id);
	static intptr GetFrameHierarchyId(RwFrame *frame);

	static void *FrameConstructor(void *object, int32 offset, int32 len);
	static void *FrameDestructor(void *object, int32 offset, int32 len);
	static void *FrameCopyConstructor(void *dst, const void *src,
	                                  int32 offset, int32 len);
	static int32 ms_framePluginOffset;

	struct ClumpExt
	{
		int alpha;
	};
	static void SetClumpModelInfo(RpClump*, CClumpModelInfo*);
	static void SetClumpAlpha(RpClump*, int);
	static int GetClumpAlpha(RpClump*);

	static void *ClumpConstructor(void *object, int32 offset, int32 len);
	static void *ClumpDestructor(void *object, int32 offset, int32 len);
	static void *ClumpCopyConstructor(void *dst, const void *src,
	                                  int32 offset, int32 len);
	static int32 ms_clumpPluginOffset;

	static bool PluginAttach(void);
};

RpMaterial *SetAlphaCB(RpMaterial *material, void *data);

#endif // __GTA_VISIBILITYPLUGINS_H__
