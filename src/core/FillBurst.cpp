#include "common.h"

#include "FillBurst.h"

#include "Draw.h"

/*
 * The hidden frames of a fill burst (VICE-126).
 *
 * A transition (an interior exit, the end of a cutscene, a restart) fades the
 * screen, and the game draws the fade over the world with the opacity
 * CDraw::FadeValue (0 = no fade, 255 = fully black, main.cpp). While the fade
 * covers at least FILL_BURST_HIDDEN_FADE of the screen the world behind it is
 * dimmed to a quarter, and the fill happens at the ring, a hundred metres and
 * more away where the fog already hides a spawn: that is the moment the burst
 * spends its large budget.
 *
 * The fully black frames of a transition are FILL_BURST_HIDDEN_FADE's own case
 * (a complete fade is 255), so the test covers them without a second rule, and
 * it covers the dark head and tail of a fade, which is where a real transition
 * spends most of its hidden frames: the script's fade-out and fade-in commands
 * leave only a few frames at full black (CCamera::ProcessFade).
 *
 * The fade of the camera is the drawn fade value, not the camera's own: the
 * population reads what the player sees, and no camera code has to change.
 */
bool
FillBurstIsHidden(void)
{
	return CDraw::FadeValue >= FILL_BURST_HIDDEN_FADE;
}
