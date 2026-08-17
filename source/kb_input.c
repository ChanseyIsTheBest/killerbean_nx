/* kb_input.c -- controller -> synthetic touch, on Killer Bean's own HUD.
 *
 * Ported from the Happy Wheels loader's hw_input.c, which solved the same
 * problem: the game understands exactly one kind of input, a touch landing on
 * one of the sprites it draws itself. So the controller is mapped by injecting
 * touches where those sprites sit, not by driving a cursor around.
 *
 * WHAT CHANGED FROM THE HAPPY WHEELS VERSION
 *
 *  - It emits NxdpEvent, the same struct nxdp_poll() produces, and the host
 *    appends them to the real touchscreen's events and runs one loop over the
 *    lot. That is what makes a synthetic touch indistinguishable from a finger:
 *    it goes through the same MotionEvent batching, the same snapshot, and so
 *    reaches BOTH input backends -- the legacy one uGUI reads and the new one
 *    LeanTouch and InputSystemUIInputModule read.
 *  - Positions are normalised 0..1 and were measured off a 1280x720 gameplay
 *    capture, so they hold if the render size changes.
 *  - No learn mode yet; the table below is the whole configuration.
 *
 * Pointer ids start at 64 to stay clear of the touchscreen's own fingers and of
 * the cursors (this port uses ids 8 and 9 for those).
 */

#include <string.h>
#include <switch.h>

#include "config.h"
#include "nx_dual_pointer.h"
#include "kb_input.h"

#define KBI_PTR_BASE 64

enum { KBI_BUTTON = 0, KBI_STICKDIR };
enum { KBI_DIR_LEFT = 1, KBI_DIR_RIGHT, KBI_DIR_UP, KBI_DIR_DOWN };
enum { KBI_STICK_L = 0, KBI_STICK_R };

typedef struct {
  const char *name;
  uint64_t    button;   /* KBI_BUTTON only */
  float       nx, ny;   /* normalised position of the on-screen control */
  int         kind;
  int         stick;    /* KBI_STICK_L / _R, for KBI_STICKDIR */
  int         dir;      /* KBI_DIR_*,        for KBI_STICKDIR */
} KbiBind;

/* Measured off the 1280x720 gameplay capture, then normalised.
 *
 * Order fixes the pointer id (KBI_PTR_BASE + index), so inserting a row
 * renumbers everything after it -- harmless, since ids only need to be stable
 * within a frame, but worth knowing.
 *
 * A position of -1 means "not placed" and emits nothing at all. A guessed
 * position is worse than none: it can land on a real control and press it. */
static KbiBind g_bind[] = {
  /* name        button                nx        ny       kind         stick        dir */

  /* --- bottom right: the action cluster --- */
  { "jump",      HidNpadButton_A,      0.8172f,  0.9347f, KBI_BUTTON,  0,           0 },
  { "gun",       HidNpadButton_B,      0.8961f,  0.8194f, KBI_BUTTON,  0,           0 },
  { "green",     HidNpadButton_Right,  0.7320f,  0.9375f, KBI_BUTTON,  0,           0 },

  /* --- bottom left: the movement arrows, both on the LEFT stick --- */
  { "move_left", 0,                    0.0898f,  0.8958f, KBI_STICKDIR, KBI_STICK_L, KBI_DIR_LEFT  },
  { "move_right",0,                    0.1992f,  0.8958f, KBI_STICKDIR, KBI_STICK_L, KBI_DIR_RIGHT },

  /* --- the weapon-switch pair ---
   *
   * SPEC NOTE: the request placed these top RIGHT. On the capture the only
   * thing top right is the pause disc; the teal left/right arrowheads are top
   * LEFT, at the coordinates below. Bound there because that is where the
   * control actually is. Pause is left unbound rather than guessed. */
  { "prev",      HidNpadButton_L,      0.0898f,  0.0833f, KBI_BUTTON,  0,           0 },
  { "next",      HidNpadButton_R,      0.1445f,  0.0833f, KBI_BUTTON,  0,           0 },
};
#define KBI_N ((int)(sizeof(g_bind) / sizeof(g_bind[0])))

/* Which bindings were down last frame, so this frame can be expressed as
 * DOWN / MOVE / UP rather than as a level. Index matches g_bind. */
static int   g_was[KBI_N];
static float g_last_x[KBI_N], g_last_y[KBI_N];

int kb_input_poll(NxdpEvent *out, int max, int screen_w, int screen_h) {
#if !KB_CONTROLLER_TOUCH
  (void)out; (void)max; (void)screen_w; (void)screen_h;
  return 0;
#else
  if (!out || max <= 0 || screen_w <= 0 || screen_h <= 0) return 0;

  /* Cursor mode is modal, deliberately. While the cursor is up the sticks
   * belong to it, and a held button would otherwise press whatever HUD slot
   * happens to sit under its bound position -- including things on a menu that
   * is not the gameplay HUD at all. So: cursor visible, bindings silent.
   *
   * Any binding that was down when the cursor appeared still gets its UP, or
   * the game would be left holding a button forever. */
  const int silent = nxdp_cursor_visible();

  PadState pad;
  padInitializeDefault(&pad);
  padUpdate(&pad);
  const uint64_t buttons = silent ? 0 : padGetButtons(&pad);
  const HidAnalogStickState ls = padGetStickPos(&pad, 0);
  const HidAnalogStickState rs = padGetStickPos(&pad, 1);

  const float STICK_DEAD = 0.18f;
  int n = 0;

  for (int i = 0; i < KBI_N && n < max; i++) {
    int fired = 0;
    float px = 0.0f, py = 0.0f;

    if (g_bind[i].nx >= 0.0f && g_bind[i].ny >= 0.0f && !silent) {
      px = g_bind[i].nx * (float)screen_w;
      py = g_bind[i].ny * (float)screen_h;

      if (g_bind[i].kind == KBI_STICKDIR) {
        const HidAnalogStickState st =
            (g_bind[i].stick == KBI_STICK_R) ? rs : ls;
        const float sx = st.x / 32767.0f, sy = st.y / 32767.0f;
        const float ax = sx < 0 ? -sx : sx, ay = sy < 0 ? -sy : sy;
        /* Dominant axis wins, so a diagonal picks one control instead of
         * firing a horizontal and a vertical together. */
        switch (g_bind[i].dir) {
          case KBI_DIR_LEFT:  fired = (sx < -STICK_DEAD) && (ax >= ay); break;
          case KBI_DIR_RIGHT: fired = (sx >  STICK_DEAD) && (ax >= ay); break;
          case KBI_DIR_UP:    fired = (sy >  STICK_DEAD) && (ay >  ax); break;
          case KBI_DIR_DOWN:  fired = (sy < -STICK_DEAD) && (ay >  ax); break;
          default: break;
        }
      } else {
        fired = (buttons & g_bind[i].button) != 0;
      }
    }

    const int id = KBI_PTR_BASE + i;

    if (fired && !g_was[i]) {
      out[n].id = id; out[n].x = px; out[n].y = py; out[n].phase = NXDP_DOWN;
      n++;
      g_was[i] = 1; g_last_x[i] = px; g_last_y[i] = py;
    } else if (!fired && g_was[i]) {
      /* UP at the last known position: an UP somewhere else reads as a drag. */
      out[n].id = id; out[n].x = g_last_x[i]; out[n].y = g_last_y[i];
      out[n].phase = NXDP_UP;
      n++;
      g_was[i] = 0;
    } else if (fired && (px != g_last_x[i] || py != g_last_y[i])) {
      out[n].id = id; out[n].x = px; out[n].y = py; out[n].phase = NXDP_MOVE;
      n++;
      g_last_x[i] = px; g_last_y[i] = py;
    }
  }
  return n;
#endif
}
