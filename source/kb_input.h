/* Controller -> synthetic touch on the game's own HUD. See kb_input.c. */
#ifndef KB_INPUT_H
#define KB_INPUT_H

#include "nx_dual_pointer.h"   /* NxdpEvent */

/* Fill `out` with this frame's DOWN/MOVE/UP for the controller bindings.
 * Returns the count. The host appends these to nxdp_poll()'s events and runs
 * one loop over both, so a synthetic touch takes exactly the same path as a
 * finger -- same MotionEvent batching, same snapshot, and therefore both the
 * legacy and the new input backends. */
int kb_input_poll(NxdpEvent *out, int max, int screen_w, int screen_h);

#endif
