#include "common.h"

#include "SpawnOccupancy.h"

#include "Ped.h"
#include "Pools.h"
#include "Vehicle.h"
#include "World.h"

/*
 * The engine side of the occupancy store (VICE-149). The store itself is
 * SSpawnOccupancyStore in the header (engine-free, no allocation); this file
 * owns the game's one instance, the hooks the entity lifecycle calls and the
 * reads the planner and the director will use.
 *
 * The hooks and their call sites:
 *
 *	CSpawnOccupancy::VehicleRegistered	in CVehicle's constructor, next to
 *						CCarCtrl::UpdateCarCount;
 *	CSpawnOccupancy::VehicleRemoved	in CVehicle's destructor, next to the
 *						same counter;
 *	CSpawnOccupancy::PedRegistered	in CPed's constructor, next to
 *						CPopulation::UpdatePedCount;
 *	CSpawnOccupancy::PedRemoved	in CPed's destructor, next to the same
 *						counter;
 *	CSpawnOccupancy::EntityMoved	CWorld::Add (the placement, the
 *					teleport, the save load's restore),
 *					CPhysical::ApplyMoveSpeed (the physics
 *					step), the aircraft's own movement
 *					(CHeli::ProcessControl, CPlane::
 *					ProcessControl, SteerAIHeliTowardsTarget
 *					Coors), the far layer's re-pose, the
 *					crane's carried car, the garage's
 *					centring and CPed::SetPedPositionInCar;
 *	CSpawnOccupancy::Resync		the replay restore (CReplay::
 *					RestoreStuffFromMem) and the boot
 *					(CGame::Initialise);
 *	CSpawnOccupancy::Reset		CWorld::ClearForRestart.
 *
 * Every hook is O(1) and changes no game value; the store stays inert until
 * the planner and the director tickets read it.
 */

SSpawnOccupancyStore CSpawnOccupancy::ms_Store;

/* The grid must cover the positions the hooks read: the world's own bounds
 * (World.h). A position off the grid is not counted, so the edge cells are
 * the world's own and are never clipped away. */
static_assert(SPAWN_OCCUPANCY_GRID_MIN_X == WORLD_MIN_X && SPAWN_OCCUPANCY_GRID_MIN_Y == WORLD_MIN_Y,
	"the occupancy grid must start at the world's corner");
static_assert(SPAWN_OCCUPANCY_CELLS_X * SPAWN_OCCUPANCY_CELL_SIZE >= WORLD_SIZE_X &&
              SPAWN_OCCUPANCY_CELLS_Y * SPAWN_OCCUPANCY_CELL_SIZE >= WORLD_SIZE_Y,
	"the occupancy grid must cover the world");

/* The handle of an entity is its pool index: unique while it lives, reused
 * after the pool frees the slot, and the same value in the constructor, in
 * the destructor and in the move hook. The slot is held (not free) at all
 * three, the same guarantee the counters' radar reads rest on. */
static int32_t
VehicleHandle(CVehicle *pVehicle)
{
	return CPools::GetVehiclePool()->GetJustIndex(pVehicle);
}

static int32_t
PedHandle(CPed *pPed)
{
	return CPools::GetPedPool()->GetJustIndex(pPed);
}

void
CSpawnOccupancy::VehicleRegistered(CVehicle *pVehicle)
{
	ms_Store.Register(SPAWN_OCCUPANCY_VEHICLE, VehicleHandle(pVehicle));
}

void
CSpawnOccupancy::VehicleRemoved(CVehicle *pVehicle)
{
	ms_Store.Remove(SPAWN_OCCUPANCY_VEHICLE, VehicleHandle(pVehicle));
}

void
CSpawnOccupancy::PedRegistered(CPed *pPed)
{
	ms_Store.Register(SPAWN_OCCUPANCY_PED, PedHandle(pPed));
}

void
CSpawnOccupancy::PedRemoved(CPed *pPed)
{
	ms_Store.Remove(SPAWN_OCCUPANCY_PED, PedHandle(pPed));
}

void
CSpawnOccupancy::EntityMoved(CEntity *pEntity)
{
	if (pEntity->IsVehicle())
		ms_Store.Move(SPAWN_OCCUPANCY_VEHICLE, VehicleHandle((CVehicle*)pEntity),
			pEntity->GetPosition().x, pEntity->GetPosition().y);
	else if (pEntity->IsPed())
		ms_Store.Move(SPAWN_OCCUPANCY_PED, PedHandle((CPed*)pEntity),
			pEntity->GetPosition().x, pEntity->GetPosition().y);
}

void
CSpawnOccupancy::Resync(void)
{
	/* The pool images can come back without a constructor (the replay
	 * restore), and every record a load or a replay left can be wrong: the
	 * rebuild clears everything - the reservations included, the clock may
	 * have moved - and counts the live slots where they stand now. */
	ms_Store.Reset();
	CVehiclePool *pVehiclePool = CPools::GetVehiclePool();
	for (int32_t i = 0; i < pVehiclePool->GetSize(); i++) {
		CVehicle *pVehicle = pVehiclePool->GetSlot(i);
		if (pVehicle)
			ms_Store.Create(SPAWN_OCCUPANCY_VEHICLE, i,
				pVehicle->GetPosition().x, pVehicle->GetPosition().y);
	}
	CPedPool *pPedPool = CPools::GetPedPool();
	for (int32_t i = 0; i < pPedPool->GetSize(); i++) {
		CPed *pPed = pPedPool->GetSlot(i);
		if (pPed)
			ms_Store.Create(SPAWN_OCCUPANCY_PED, i,
				pPed->GetPosition().x, pPed->GetPosition().y);
	}
}

void
CSpawnOccupancy::Reset(void)
{
	ms_Store.Reset();
}

bool
CSpawnOccupancy::CellAt(float x, float y, int32_t &cellX, int32_t &cellY)
{
	int32_t cell;
	if (!SSpawnOccupancyStore::CellAt(x, y, cell))
		return false;
	cellX = cell % SPAWN_OCCUPANCY_CELLS_X;
	cellY = cell / SPAWN_OCCUPANCY_CELLS_X;
	return true;
}

int32_t
CSpawnOccupancy::CountAtPosition(ESpawnOccupancyClass type, float x, float y)
{
	return ms_Store.CountAtPosition(type, x, y);
}

int32_t
CSpawnOccupancy::CountAtCell(ESpawnOccupancyClass type, int32_t cellX, int32_t cellY)
{
	return ms_Store.CountAtCell(type, cellX, cellY);
}

int32_t
CSpawnOccupancy::TotalCount(ESpawnOccupancyClass type)
{
	return ms_Store.TotalCount(type);
}

SSpawnOccupancyStore &
CSpawnOccupancy::Store(void)
{
	return ms_Store;
}

bool
CSpawnOccupancy::ReserveSite(int32_t site, uint32_t now)
{
	return ms_Store.ReserveSite(site, now);
}

bool
CSpawnOccupancy::SiteReserved(int32_t site, uint32_t now)
{
	return ms_Store.SiteReserved(site, now);
}

void
CSpawnOccupancy::ReleaseSite(int32_t site)
{
	ms_Store.ReleaseSite(site);
}
