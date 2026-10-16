# The population count follows the spawn radius

The spawn distance slider moves the ring where cars and pedestrians are created
and removed. A wider ring spreads a fixed count over a larger area, so the
streets near the player thin out. The budget and the caps now scale with the
square of the spawn distance — the ring area — clamped below the pools, so a
wider ring keeps the streets as dense as the pools allow.

The alternative was to keep the count and the radius separate; we rejected it
because the owner wants a wide ring and a crowded street at the same time. The
cost is memory and CPU: the ped pool is 1536, the vehicle pool 320 and the
audio pool 1024 (about 2 MB more), and the caps (900 pedestrians, 256 vehicles,
200 random cars) are the safety line below the pools. At a spawn distance of
1.0 nothing changes.
