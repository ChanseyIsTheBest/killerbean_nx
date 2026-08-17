/* nx_data_root.h -- resolve the game folder at RUNTIME from wherever the .nro
 * actually lives, instead of hardcoding a folder name.
 *
 * WHY. Every path in this loader used to be built from a compile-time
 * `sdmc:/switch/<GAME_FOLDER>`. If the folder on the SD card was named
 * anything else -- say the .nro sat in `switch/killerbean_nx/` while the
 * loader expected `switch/killerbean/` -- the result was "Could not load
 * libmain.so" about a file that was plainly on the card. Worse, debug.log
 * lives under that same root, so a wrong root also meant NO LOG to diagnose
 * from. The folder name should never have been load-bearing.
 *
 * Now it isn't: the root comes from argv[0], which hbloader sets to the full
 * path of the .nro it launched. Put the folder anywhere, name it anything.
 */
#ifndef NX_DATA_ROOT_H
#define NX_DATA_ROOT_H

/* Resolved once, very early in main(), BEFORE the first debugPrintf. */
extern char g_data_root[512];   /* e.g. "sdmc:/switch/killerbean_nx"        */
extern char g_log_path[576];    /* g_data_root + "/debug.log"               */

/* How the root was found -- logged once so a bad layout is self-diagnosing. */
extern char g_data_root_how[512];

/* Must be the first thing main() calls. Safe to call with argc==0/argv==NULL:
 * it falls back through the compile-time default, the legacy name, and finally
 * a bounded scan of sdmc:/switch/. */
void nx_resolve_data_root(int argc, char *argv[]);

/* Build "<g_data_root><sub>" into a rotating static buffer. `sub` must start
 * with '/' (or be ""). Replaces the old `DATA_ROOT "/thing"` compile-time
 * concatenation, which cannot work once the root is a runtime value.
 *
 * ROTATING BUFFERS: 8 slots. That covers the normal case of one or two calls
 * live in a single expression. Do not stash the returned pointer long-term --
 * copy it if you need to keep it. */
const char *nx_path(const char *sub);

#endif /* NX_DATA_ROOT_H */
