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
 * ordinary counted pedestrian. The reserve holds the frame of a fill: for the
 * trickle, the placements of the 6 ped calls at the capped ring (3 x 2 calls,
 * the widest one a group of 6) and the crew of the 6 car calls (a driver and up
 * to three passengers each); for a fill burst, the widest step of its loop
 * between two pool reads (FILL_BURST_POOL_CHECK_ATTEMPTS attempts, so at most
 * 4 x 6 on-foot pedestrians or 4 x 4 car crew members), plus room for the
 * one-off paths. The burst checks the pool and the stop line every step, so a
 * burst frame of 100 attempts stops at the pool instead of emptying it. The
 * wide groups are the mall groups of `PlaceMallPedsAsStationaryGroup`
 * (3..6 under FIX_BUGS; a gang group is 3..5).
 *
 * The line is held by the creation paths, not by the arithmetic alone:
 *
 *	CPopulation::Update		one ped pool read a frame; the frame then
 *					adds at most the reserve's frame
 *	CPopulation::SpendFillBurstFrame	one read before its loop, then one
 *					every FILL_BURST_POOL_CHECK_ATTEMPTS attempts
 *					(the stop line itself is re-read every
 *					attempt, so its overshoot is one placement)
 *	CCarCtrl::GenerateRandomCars	one read a frame, before any car
 *					(and the drivers) is created
 *	CCarCtrl::SpendFillBurstFrame	the same step inside the car burst, or a
 *					frame of 100 cars could take 400 crew members
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
 * (ADR 0013, the traffic rate follows the ring). The ring stops at scale 2
 * (VICE-145), so a frame makes 3 x 2 = 6 car calls and 6 ped calls.
 *
 * That trickle fills a full ring in tens of seconds, which is the owner's
 * complaint. The fill burst is the other rate: after boot, an interior exit and
 * a cutscene end the street fills at once, in a bounded work window, and each
 * frame of the window carries a bounded number of attempts. The rates decide
 * how fast a count reaches its top; the caps decide where it stops.
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
/* The free ped pool slots a frame leaves to its creation paths: the frame of a
 * fill plus room for the one-off paths (scripts, the ropes). The widest
 * placement is a group of 6 (a mall group; a gang group is 5), so the trickle
 * of the capped ring asks for 6 x 6 = 36 and the widest step of a burst loop
 * (FILL_BURST_POOL_CHECK_ATTEMPTS attempts between two pool reads) asks for
 * 4 x 6 = 24 on-foot pedestrians or 4 x 4 = 16 car crew members. */
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

/* --- The fill burst (VICE-126) ------------------------------------------ */

/* The burst is the fill of a transition: after boot, an interior exit and a
 * cutscene end the counts jump to their targets instead of trickling up. It is
 * a bounded work window, and the bound is one number for every run:
 *
 *	FILL_BURST_FRAMES x FILL_BURST_ATTEMPTS_PER_FRAME	the visible window
 *	FILL_BURST_FRAMES x FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME the hidden one
 *
 * A frame that spends its attempts spends a window frame, placing or not. A
 * frame whose zone models are not resident after the run has started does not
 * spend the window, but it counts against the give-up bound below, so the
 * streaming wait is bounded too and no run can ask for more attempts than the
 * hidden window above. A frame the callers spend before the run's first
 * attempt where the world is not ready yet - the local target is not known
 * (the boot frames before the zone and the range are live, a target of 0) or
 * the zone's models are not resident yet (the slow load of the first seconds)
 * - rides the boot wait (FILL_BURST_BOOT_WAIT_FRAMES): it does not spend the
 * window and does not burn the give-up bound, because a world that is still
 * loading is not a placement stall. The first cut of VICE-146 sent these
 * frames through the stall helper, and the pedestrian boot burst died
 * silently at boot - once on the zero target, once on the models - before its
 * first attempt (the fix round; the live read showed the target 1377 and the
 * attempts 0). The car target and the car budget have positive floors (the
 * ring table's scale floor is the FOV factor, at least 1.0, and the ini
 * multipliers clamp at 0.2), so the boot wait is the pedestrian burst's in
 * practice; the shared helper serves both. A frame that ends without a
 * placement is a stall, and FILL_BURST_STALL_FRAMES of them in a row give the
 * burst up and hand the fill back to the trickle.
 *
 * The screen hides the fill on the frames the camera covers: the fully black
 * frames of a transition and the dark head of a fade (the fade is drawn over
 * the world, and FILL_BURST_HIDDEN_FADE is how much of it hides a spawn at the
 * ring). There the burst spends the whole budget: the world behind the fade is
 * dimmed to a quarter at most, so the work cannot be seen. On a visible frame
 * it spends the small budget, a few times the trickle and still bounded, and it
 * stops at FILL_BURST_TARGET_FRACTION of the local target, so a full area
 * cannot be over-spawned by a burst that outlives the transition.
 *
 * The random stream: the burst draws from the one global stream, exactly like
 * the trickle it replaces, and it does not reseed. What the shift reaches is
 * not everything after it: the car AI reseeds the stream from its own
 * m_nRouteSeed at every path pick (CCarCtrl::PickNextNodeRandomly and the three
 * other sites), so a car that picks its path after a burst is not shifted. The
 * draws that do follow the shared stream past a burst are the pedestrian models
 * and the pedestrian AI (the wander path of PedAI.cpp), the script randomness
 * (COMMAND_GENERATE_RANDOM_FLOAT of Script.cpp), the accident rolls and the
 * weather. The ADR (docs/adr/0021-the-fill-burst.md) states it, and the
 * counters count the attempts, so the shift is measurable.
 *
 * The burst is bounded by the window, so a wrong target cannot make it runaway:
 * the target is checked first every frame and every attempt inside the frame,
 * on the live counts (the totals of the pedestrians are recomputed every
 * attempt, exactly as the shipped start-of-game burst recomputed them). A
 * target that is not known yet (0, the boot frames) waits instead of ending
 * the run, inside the same give-up bound (VICE-146): the burst is not burned
 * on a target that cannot be filled.
 */
#define FILL_BURST_FRAMES 60			/* the work window: 2 s at 30 fps */
#define FILL_BURST_ATTEMPTS_PER_FRAME 25	/* a visible frame: about 4 x the trickle at the ring cap */
#define FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME 100 /* a hidden frame: the rate the shipped start-of-game burst spent */
#define FILL_BURST_HIDDEN_FADE 192		/* the fade that hides the fill: three quarters of the screen (CDraw::FadeValue) */
#define FILL_BURST_TARGET_FRACTION 0.9f		/* the stop line: 90 % of the local target */
#define FILL_BURST_STALL_FRAMES 30		/* frames in a row without a placement that give the burst up: 1 s */
#define FILL_BURST_BOOT_WAIT_FRAMES 600		/* the boot wait: a dedicated bound, about 20 s at 30 fps (VICE-146 fix) */
#define FILL_BURST_POOL_CHECK_ATTEMPTS 4	/* the ped pool is re-read every this many attempts inside a burst frame */

static_assert(FILL_BURST_HIDDEN_ATTEMPTS_PER_FRAME >= FILL_BURST_ATTEMPTS_PER_FRAME,
	"a hidden frame can spend at least the budget of a visible one");
static_assert(FILL_BURST_FRAMES > FILL_BURST_STALL_FRAMES,
	"the give-up bound must bind before the window does");
static_assert(FILL_BURST_TARGET_FRACTION > 0.0f && FILL_BURST_TARGET_FRACTION <= 1.0f,
	"the stop line is a fraction of the local target");
static_assert(FILL_BURST_HIDDEN_FADE > 0 && FILL_BURST_HIDDEN_FADE <= 255,
	"the hidden threshold is a fade value");
static_assert(FILL_BURST_POOL_CHECK_ATTEMPTS > 0, "the pool check step must make progress");
static_assert(FILL_BURST_BOOT_WAIT_FRAMES > FILL_BURST_STALL_FRAMES,
	"the boot wait must outlast the stall bound it must not burn");
/* The burst of a frame has to beat the trickle it replaces at the ring cap.
 * The ratio needs the ring table (RING_MAX_SCALE), which this table does not
 * include, so the check lives in the evidence program (utils/burst-fill),
 * which reads both tables. */

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
