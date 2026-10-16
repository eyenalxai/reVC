#ifndef __GTA_POPULATIONBUDGET_H__
#define __GTA_POPULATIONBUDGET_H__

/*
 * The one table of the population and traffic budget (VICE-123).
 *
 * A budget here is a number between the density sliders and a pool: the caps
 * (the hard tops of the counts), the fill rates (how fast the counts reach
 * their tops) and the streaming model budget. They live in one table because
 * they are read together (the audit of utils/caps-audit prints them side by
 * side) and because a cap that a rate cannot fill, or a top that a pool cannot
 * hold, is a bug in the combination, not in one number.
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
 * the sidewalk. The car budget counts every car of the traffic: the random
 * ones, the parked ones, the mission and script cars, the law enforcers and
 * the emergency services (CCarCtrl::NumCarsInUse). One quirk of the count,
 * documented rather than changed: a law-enforcer random car increments BOTH
 * NumLawEnforcerCars and NumRandomCars (CarCtrl.cpp, UpdateCarCount), so the
 * sum counts it twice and the car budget blocks one car early per live law car
 * (the spawner keeps m_MaximumLawEnforcerVehicles, 3 at the top, and other
 * paths add a few more; measured 5..6 live). The double count is on the safe
 * side: the random traffic stops a little earlier, never later, and the pool
 * margin is untouched.
 *
 * The pools behind the caps
 * -------------------------
 *
 * The ped pool, NUMPEDS (8192) slots. The driver line is the whole vehicle
 * pool: any car of the pool can hold a driver, because a parked car can be
 * stolen; the car budget below it is the traffic's own line.
 *
 *	on foot		<= MAX_PEDS_IN_USE				4900
 *	drivers		<= NUMVEHICLES					2560
 *	passengers	<= MAX_PED_PASSENGERS (the counted ones)	 400
 *	reserve		>= PED_POOL_RESERVE				 332
 *									----
 *									8192
 *
 * The passenger allowance covers the passengers the traffic counts: a passenger
 * is added per seat with p = 0.125 (PROBABILITY_OF_PASSENGER_IN_VEHICLE) and
 * the fleet gives about 0.11 passengers per car (measured live), so the full
 * car budget asks for about 270, and MAX_PED_PASSENGERS stops the counted ones
 * at 400. Every other pedestrian of a car is already in the lines above: the
 * police crews count as cops in ms_nTotalPeds (only the passengers of the
 * traffic are subtracted from it, CarCtrl.cpp), and a script passenger is an
 * ordinary counted pedestrian. The reserve holds the burst of one frame: the
 * placements a frame makes, about 120 on-foot pedestrians (the ped attempts at
 * the ring top times the widest group, 3 x 7.88 x 5) and about 92 car crew
 * members (a driver and up to three passengers of each of the cars the car loop
 * can create), plus room for the one-off paths.
 *
 * The line is held by the creation paths, not by the arithmetic alone:
 *
 *	CPopulation::Update		one ped pool read a frame; the frame then
 *					adds at most the reserve's burst
 *	GeneratePedsAtStartOfGame	one read for the burst; the budget inside
 *					the loop is recomputed every call, so the
 *					burst of one call is one group (<= 5)
 *	CCarCtrl::GenerateRandomCars	one read a frame, before any car
 *					(and the drivers) is created
 *	CreateCarFromPlacement		one read per car with passengers, and
 *					MAX_PED_PASSENGERS caps the counted ones
 *
 * A creation path that finds the free spaces below the reserve adds nothing
 * that frame, so the pool cannot be exhausted even if a budget is passed;
 * CPopulation::RemovePedsIfThePoolGetsFull (one ped every 8 frames while
 * fewer than 8 slots are free) is the last valve behind them.
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
 * The parked cars of the generator sit inside the same car budget: the random
 * cars and the generator both stop at NumCarsInUse, so a full curb trades with
 * the random cars, not with the pool. A generator holds one car, so the map's
 * generator count (NUM_CARGENS) is the hard top of the parked count.
 *
 * The fill rate
 * -------------
 *
 * The rates are not caps: they bound the time a count takes to reach its top
 * (ADR 0013, the traffic rate follows the ring). The ring top is scale 7.88, so
 * a frame makes 3 x 7.88 = 23 car calls and 23 ped calls.
 *
 * The streaming model budget
 * --------------------------
 *
 * The vehicle models the population asks for are held resident by
 * CStreaming::ms_vehiclesLoaded (MAXVEHICLESLOADED slots; the soft target is
 * desiredNumVehiclesLoaded, the memory formula of CStreaming::Init2). The
 * population chooses the models of the class arrays plus the police and gang
 * sets; utils/caps-audit reads the shipped data and checks that the set fits
 * the tracker (it does with room: the classes hold 56 models and the tracker
 * 90, of the 110 model slots).
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
#define MAX_PEDS_IN_USE 4900
/* Every car the car budgets count. Below the pool by the cars that are outside
 * the budgets (the permanent planes and the police helicopter, the set pieces). */
#define MAX_CARS_IN_USE 2400
/* The random cars alone: the whole car budget. The sum check (NumCarsInUse in
 * GenerateOneRandomCar) is the real cap; it counts the parked, mission, law and
 * emergency cars too, so this top cannot block them. */
#define MAX_RANDOM_CARS MAX_CARS_IN_USE
/* The parked cars one generator set holds at the stock ring. They follow the
 * ring area like the traffic (ADR 0009) and stop at the generator count, one
 * car per generator (the map has 176: NUM_CARGENS is 185). */
#define PARKED_CARS_AT_STOCK_RING 20.0f

/* The passengers of the traffic (the ones ms_nTotalPeds subtracts, CarCtrl.cpp);
 * this is their true cap. The police crews and the script peds are not
 * passengers in that sense and count in the lines above. */
#define MAX_PED_PASSENGERS 400
/* The free ped pool slots a frame leaves to its creation paths: the burst of
 * one frame (about 120 on-foot pedestrians and 92 car crew members) plus room
 * for the one-off paths (scripts, the ropes). */
#define PED_POOL_RESERVE 332

/* The models one class array holds (CCarCtrl::CarArrays): the shipped data
 * stays below it, so AddToCarArray cannot assert (utils/caps-audit reads the
 * data and checks it). */
#define MAX_CAR_MODELS_IN_ARRAY 25

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
static inline int
ParkedCarTarget(float ringScale)
{
	float t = PARKED_CARS_AT_STOCK_RING * ringScale * ringScale;

	return t >= (float)NUM_CARGENS ? NUM_CARGENS : (int)t;
}

/*
 * The invariants. A wrong number in the table fails the build here, not the
 * player at the maximum sliders.
 */
static_assert(MAX_PEDS_IN_USE + NUMVEHICLES + MAX_PED_PASSENGERS + PED_POOL_RESERVE <= NUMPEDS,
	"the on-foot budget, the cars, the counted passengers and the reserve must fit NUMPEDS");
static_assert(MAX_CARS_IN_USE < NUMVEHICLES, "the car budget must stay below the vehicle pool");
static_assert(MAX_PEDS_IN_USE >= (int)(DEFAULT_MAX_NUMBER_OF_PEDS * 3.4f), "the on-foot cap cannot clamp the slider top at the stock ring");
static_assert(MAX_CARS_IN_USE >= (int)(DEFAULT_MAX_NUMBER_OF_CARS * 3.4f), "the car cap cannot clamp the slider top at the stock ring");
static_assert(NUM_CARGENS < MAX_CARS_IN_USE, "the parked top must leave room for the random cars in the car budget");
/* The vehicle residency tracker must fit the vehicle model slots. */
static_assert(MAXVEHICLESLOADED <= VEHICLEMODELSIZE, "the vehicle residency tracker must fit the vehicle model slots");

#endif // __GTA_POPULATIONBUDGET_H__
