#ifndef __GTA_POPULATIONCAPS_H__
#define __GTA_POPULATIONCAPS_H__

/*
 * The one table of the population and traffic budgets (VICE-123).
 *
 * Every cap between the density sliders and the pools lives here, with the pool
 * it protects, so the numbers cannot drift apart and the two invariants of the
 * table are checked at compile time:
 *
 *	on foot + the drivers + the passengers + the frame reserve <= the ped pool
 *	the cars (random, parked and the rest) < the vehicle pool
 *
 * The budgets
 * -----------
 *
 * The density sliders move two budgets (CIniFile::ApplyPopulationCaps and
 * CCarCtrl::GetRandomCarTarget). The ring area grows with the square of the
 * ring scale, so both grow with it (ADR 0009) and both stop at a hard top:
 *
 *	on-foot budget	= min(DEFAULT_MAX_NUMBER_OF_PEDS x the ped density x scale^2, MAX_PEDS_IN_USE)
 *	car budget	= min(DEFAULT_MAX_NUMBER_OF_CARS x the car density x scale^2, MAX_CARS_IN_USE)
 *	car target	= min(RANDOM_CARS_PER_INI_UNIT x the car density x scale^2, MAX_RANDOM_CARS)
 *
 * The pedestrian cap is the count ON FOOT. The drivers and the passengers of
 * the cars are pedestrians in the same pool, so they are added ON TOP of the
 * cap (CPopulation::AddToPopulation): a full street does not eat the crowd on
 * the sidewalk. The car cap counts every car the traffic budgets count: the
 * random ones, the parked ones, the mission and script cars, the law enforcers
 * and the emergency services (CCarCtrl::GenerateOneRandomCar).
 *
 * The pools behind the caps
 * -------------------------
 *
 * The ped pool, NUMPEDS (8192) slots:
 *
 *	on foot		<= MAX_PEDS_IN_USE				5100
 *	drivers		<= MAX_CARS_IN_USE (one per car)		2400
 *	passengers	<= MAX_PED_PASSENGERS (the measured rate)	 400
 *	reserve		>= PED_POOL_RESERVE				 292
 *									----
 *									8192
 *
 * The passenger allowance covers the measured rate of the shipped game: a
 * passenger is added per seat with p = 0.125 (PROBABILITY_OF_PASSENGER_IN_VEHICLE)
 * and the fleet gives about 0.11 passengers per car (measured live), so the full
 * car budget asks for about 270. The reserve is the free space a frame must
 * leave before its creation paths run: the burst of a frame is bounded by the
 * placements it makes, about 120 on-foot pedestrians (the ped attempts per frame
 * at the ring top times the widest group, 3 x 7.88 x 5) and about 92 car crew
 * members (a driver and up to three passengers of each of the cars a frame can
 * create), so 292 holds the burst with room for the one-off paths.
 *
 * The creation paths that the budgets drive refuse to create when the free
 * spaces fall below the reserve, so the pool cannot be exhausted: the on-foot
 * loop (CPopulation::Update and GeneratePedsAtStartOfGame), the car loop
 * (CCarCtrl::GenerateRandomCars, which also covers the drivers) and the
 * passenger loop (CCarCtrl::CreateCarFromPlacement). The one-off paths that the
 * budgets do not drive (a script car, the helicopter's swat, the rope) stay
 * outside the check; they are a handful per mission and the reserve is theirs.
 *
 * The vehicle pool, NUMVEHICLES (2560) slots, holds every car of the car budget
 * plus the cars that are outside it: the permanent ones (the planes and the
 * police helicopter), the set pieces and the shortcut car. The margin is 160:
 *
 *	cars		<= MAX_CARS_IN_USE				2400
 *	outside the budget						 160
 *									----
 *									2560
 *
 * The parked cars of the generator sit inside the same car budget: a random car
 * is not created when the sum reaches MAX_CARS_IN_USE, so the parked cars trade
 * with the random ones, not with the pool. A generator holds one car, so the
 * map's generator count (NUM_CARGENS) is the hard top of the parked count.
 *
 * The streaming budget
 * --------------------
 *
 * The vehicle models the population asks for are held resident by
 * CStreaming::ms_vehiclesLoaded (MAXVEHICLESLOADED slots; the soft target is
 * desiredNumVehiclesLoaded, the memory formula of CStreaming::Init2). The
 * population chooses from the 11 class arrays (56 models in the shipped data,
 * at most 25 per class) plus the police and gang sets, about 70 in total, so
 * the 90 slots hold every model a full street asks for. The shipped data has
 * 107 vehicle models; 51 of them are the "ignore" class (planes, RC, trains,
 * the unique vehicles) which the population never chooses. Measured live with
 * a full street: 63 resident.
 */

#include "config.h"

/* --- The density slider units ------------------------------------------- */

/* The pedestrians per unit of the Ped Density slider, at the stock ring, on
 * foot (the slider is 1.0 at stock: 100 pedestrians; the interior constant is a
 * flat budget, the interior has no ring). */
#define DEFAULT_MAX_NUMBER_OF_PEDS 100.0f
#define DEFAULT_MAX_NUMBER_OF_PEDS_INTERIOR 60.0f
/* The cars per unit of the Car Density slider at the stock ring (the slider is
 * 2.0 at stock: 90 cars). */
#define DEFAULT_MAX_NUMBER_OF_CARS 45.0f
/* The random cars per unit of the Car Density slider at the stock ring (60
 * random cars at stock, of the 90 cars). */
#define RANDOM_CARS_PER_INI_UNIT 30.0f

/* --- The hard tops ------------------------------------------------------ */

/* The on-foot pedestrians. The drivers and the passengers of the cars come on
 * top of this cap (the pool arithmetic above). */
#define MAX_PEDS_IN_USE 5100
/* Every car the car budgets count. Below the pool by the cars that are outside
 * the budgets (the permanent planes and the police helicopter, the set pieces). */
#define MAX_CARS_IN_USE 2400
/* The random cars alone. The total car budget is the real cap: the sum check in
 * GenerateOneRandomCar counts the parked, mission, law and emergency cars too,
 * so this top can hold the whole budget without blocking the random target. */
#define MAX_RANDOM_CARS MAX_CARS_IN_USE
/* The parked cars one generator set holds at the stock ring. They follow the
 * ring area like the traffic (ADR 0009) and stop at the generator count, one
 * car per generator (the map has 176: NUM_CARGENS is 185). */
#define PARKED_CARS_AT_STOCK_RING 20.0f

/* The passengers the ped pool must hold at the full car budget (measured rate
 * 0.125 per seat, about 0.11 per car, so about 270 at 2400 cars). */
#define MAX_PED_PASSENGERS 400
/* The free ped pool slots a frame leaves to its creation paths: the burst of
 * one frame (about 120 on-foot pedestrians and 92 car crew members) and the
 * one-off paths. */
#define PED_POOL_RESERVE 292

/* --- The fill rate ------------------------------------------------------ */

/* The placements a random car call tries before it gives up. */
#define CAR_SPAWN_ATTEMPTS 4
/* The random car calls per frame, times the ring scale: the ring fills from
 * the target, so the rate follows the ring like the target does. */
#define RANDOM_CAR_ATTEMPTS_PER_FRAME 3
/* The pedestrian calls per frame, times the ring scale. */
#define PED_ATTEMPTS_PER_FRAME 3

/* The parked cars a generator set holds at this ring scale: the stock count
 * over the ring area, capped by the generator count. */
static inline int ParkedCarTarget(float ringScale)
{
	float t = PARKED_CARS_AT_STOCK_RING * ringScale * ringScale;
	return t >= (float)NUM_CARGENS ? NUM_CARGENS : (int)t;
}

/*
 * The invariants. The first one is the pool arithmetic above; the second one
 * keeps the traffic below the vehicle pool. A wrong number in the table fails
 * the build here, not the player at the maximum sliders.
 */
static_assert(MAX_PEDS_IN_USE + MAX_CARS_IN_USE + MAX_PED_PASSENGERS + PED_POOL_RESERVE <= NUMPEDS,
	"the ped budget, its drivers, its passengers and its reserve must fit NUMPEDS");
static_assert(MAX_CARS_IN_USE < NUMVEHICLES, "the car budget must stay below NUMVEHICLES");
static_assert(MAX_RANDOM_CARS <= MAX_CARS_IN_USE, "the random car target cannot pass the total car budget");
static_assert((float)MAX_PEDS_IN_USE >= DEFAULT_MAX_NUMBER_OF_PEDS, "the on-foot cap must hold the stock budget");
static_assert((float)MAX_CARS_IN_USE >= DEFAULT_MAX_NUMBER_OF_CARS * 2.0f, "the car cap must hold the stock budget");
/* The vehicle residency tracker must fit the vehicle model slots. */
static_assert(MAXVEHICLESLOADED <= VEHICLEMODELSIZE, "the vehicle residency tracker must fit the vehicle model slots");

#endif // __GTA_POPULATIONCAPS_H__
