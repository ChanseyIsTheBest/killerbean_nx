/* config.h -- Plants vs Zombies Fusion 3.6.1 Switch wrapper configuration
 * (forked from the Zookeeper DX / CR3 wrapper config.)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

/* ============================ MEMORY LAYOUT ==============================
 * These are engine-fitting parameters, not game content -- identical to the
 * Zookeeper DX port because Fruit Ninja Classic + is the SAME Unity minor version
 * (2022.3.62) and we apply the SAME 256MB->64MB region-granularity patch
 * (see nx_patch_killerbean.h). Do not change unless you know the allocator math.
 * ======================================================================== */

// The engine + libc++ + il2cpp heap need a generous newlib heap; the rest of
// system memory is handed to the .so loader (see __libnx_initheap).
#define MEMORY_MB 768

// Anonymous-mmap arena. Unity reserves big region-aligned pools by over-mmapping
// then munmapping the unaligned head/tail. We back anonymous mmaps from a
// dedicated, region-aligned arena with a per-page used-bitmap so sub-range
// munmap frees exactly the trimmed pages. Region granularity is 64MB to match
// the libunity patch.
#define MMAP_ARENA_ALIGN    ((size_t)64 * 1024 * 1024)    // 64MB region granularity (libunity patched 256MB->64MB, nx_patch_killerbean.h). NOTE: 16MB was tried and CORRUPTED Unity's Dynamic Heap allocator at init (overlapping regions from the over-map/trim pattern -> null-prev free-list crash). 64MB is the known-good floor.
/* Round 152: 192 -> 896 MB. The r151 log finally produced the failure this
 * comment has predicted for dozens of rounds, with the exact recorded
 * signature:
 *
 *   [oc] ARMED: window 704 MB          <- only 704 this boot; 1152-1600 in others
 *   [mem] arena 81% reserved (157 of 192 MB)
 *   [mmap] OC window full for 319 MB -> heap-backed arena
 *   [mmap] arena full -> newlib fallback (#1) -- memory pressure
 *   [mmap] 127 MB (prot=0x0 anon=1) -> 0x0     <- mmap returned NULL
 *   [mmap] arena full -> newlib fallback (#2) -- memory pressure
 *   -> crash in libunity
 *
 * "couldn't fit a 127MB OC-window overflow -> mmap NULL -> Unity crash" is
 * word for word what 512 MB did. We were running 192.
 *
 * It also explains the intermittency: the OC window is clamped to the largest
 * stack-region hole, which is different every boot (704 MB here, 1152-1600 in
 * runs that were fine). A big window absorbs the spill; a small one pushes it
 * into this arena, which at 192 MB could not take it. "A couple of perfect runs
 * then a crash" is that lottery.
 *
 * 896 is the value this comment has recommended all along = 1.75x the 512 fail
 * point. Anything at or below 512 is KNOWN to fail -- do not "compromise" at
 * 400. Paid for by right-sizing OC_POOL_BYTES below. */
#define MMAP_ARENA_RESERVE  ((size_t)896 * 1024 * 1024)

// Stack-region overcommit (OC) arena (see libc_shim.c): PROT_NONE reservations
// held in a stack-region window, committed pages backed from a small heap pool.
#define OC_WINDOW_BYTES     ((size_t)2048 * 1024 * 1024)  // 32x64MB cheap PROT_NONE reservation. Was 1536; the window-finder clamps to the largest stack-region hole (min(this, hole)), so raising the cap lets a run use its full hole and spill fewer reservations into the (real-memory) arena.
// Commit-pool: real memory backing touched pages of the OC window. Unity is told it
// has 512 MB (libc_shim.c __sysconf PHYS_PAGES + dalvik.vm.heapsize), and it reserves
// its heaps as big PROT_NONE regions that route here, so this pool must be able to
// back the full 512 MB Unity believes it has -- at 256 MB the scene load exhausted it
// (~270 MB working set) and the next uncommitted page faulted -> hard OOM crash.
// malloc. UPDATE 2: trimming the pool 1280->1024 + arena 1024->512 fixed the malloc
// OOM (malloc 1409 -> game pushed past the null-buffer crash) but 512 starved the
// arena (see above). That round settled on pool 896 + arena 896 + malloc 1153.
//
// UPDATE 3 (round 107): those last two no longer describe the tree. The ACTUAL
// values are pool 512 and MMAP_ARENA_RESERVE 896 as of round 152. (Between
// r107 and r152 they were pool 1024 / arena 192, and neither comment said so --
// the recorded "balance" was fiction for 45 rounds. It is accurate again now.) Against the observed 2752 MB newlib heap that gives
// pool 1024 + arena 192 + so_region 160 = 1376 MB, leaving ~1376 MB for malloc.
// Known failure points, for reference when one of these OOMs again:
//   pool live 551 | arena fail 512 | malloc fail 641
// Note the arena is now BELOW its recorded failure point. It has been booting, so
// the enlarged OC window is evidently absorbing what used to spill there, but it
// is the next thing to look at if an allocation failure shows up in the arena.
// RAISED 896 -> 1024 (round 107). Fruit Ninja's live footprint is not PvZ's --
// 244 MB observed at ModeSelect here, but the out-of-memory crash happened later,
// in play, and this is the pool that has to absorb it. +128 MB comes out of the
// plain-malloc share, which currently sits ~1376 MB against a recorded failure
// point of 641 MB, so there is room to give.
//
// Safe to raise now: main.c retries in 128 MB steps down to OC_POOL_MIN_BYTES if
// the allocation does not fit, and logs what it settled on. Previously a pool
// that was too big meant OC DISABLED and a dead boot, which is why this number
// had not been touched.
/* Round 152: 1024 -> 512 MB, to pay for the arena above without asking newlib
 * for more in total. 512 is this comment's own documented floor -- "must be
 * able to back the full 512 MB Unity believes it has" -- and the r151 session
 * peaked at `[oc] committed 235 MB (pool 235/1024)`, so it never used even half
 * of 512. The pool was over-reserved by ~4x while the arena starved.
 *
 * Budget against the observed 2752 MB newlib heap:
 *     before  pool 1024 + arena 192 = 1216   -> malloc ~1536, arena OOM'd
 *     after   pool  512 + arena 896 = 1408   -> malloc ~1344, still above the
 *                                               1153 the history settled on
 * If a scene ever pushes the pool past 512 the symptom is a hard OOM on an
 * uncommitted page (see above), and the fix is to take it back off the arena. */
#define OC_POOL_BYTES       ((size_t)512 * 1024 * 1024)   // commit-pool (touched pages only)
#define OC_POOL_MIN_BYTES   ((size_t)640 * 1024 * 1024)   // ladder floor; below the
                                                          // 551 MB live high-water there
                                                          // is no point continuing

// Overcommit (alias-region) mode: reserve a big *virtual* window (PROT_NONE
// costs only address space) and commit physical pages on demand -- true
// overcommit, matching Android.
#define MMAP_VIRT_RESERVE   ((size_t)6144 * 1024 * 1024)  // 6 GB virtual reservation window
#define OVERCOMMIT_HEAP_MB  608u                          // newlib malloc + .so load zone

/* ============================ GAME IDENTITY =============================== */

// Fruit Ninja Classic + ships the engine as the standard modern Unity trio; libmain.so
// dlopens libunity.so which dlopens libil2cpp.so. (No libcrx/MVGL here -- this
// is a normal IL2CPP game, so main.c loads libmain/libunity/libil2cpp directly
// and these SO_NAME macros are unused, kept only for parity with the base.)
#define SO_NAME      "libunity.so"
#define SO_CPP_NAME  "libil2cpp.so"

// The SD-card folder holding the .nro + the game files.
/* MUST match the folder name on the SD card. Deliberately identical to the
 * Makefile's TARGET (killerbean_nx) so the data folder and the .nro share one
 * name -- `switch/killerbean_nx/killerbean_nx.nro`, which is what the homebrew
 * convention leads you to do anyway.
 *
 * The reference tree this came from had TARGET=fruitninja_nx but
 * GAME_FOLDER="fruitninja", so the .nro and its data folder had DIFFERENT
 * names. That inconsistency was inherited here and caused exactly the failure
 * you would expect: everything placed in switch/killerbean_nx/ while the
 * loader looked in switch/killerbean/, reporting "Could not load libmain.so"
 * about a file that was plainly sitting on the card. Fixed by making the two
 * names the same. */
#include "nx_data_root.h"   /* g_data_root / g_log_path / nx_path() */

#define GAME_FOLDER  "killerbean_nx"   /* fallback only -- see nx_data_root.h */
#define DATA_ROOT_DEFAULT "sdmc:/switch/" GAME_FOLDER

/* Bump when shipping. Printed at compile time (#pragma message in main.c)
 * and at boot, so a stale source tree is obvious from either the build
 * output or debug.log. */
#define KB_SRC_REV   "kb-r1"

/* ---- Android package name -- YOU MUST SET THIS FROM YOUR OWN APK ---------
 * Returned by our fake getPackageName(). Unity surfaces it as
 * Application.identifier, and game code (and any SDK keying off it) reads it.
 *
 * The package name is NOT present in libunity/libil2cpp -- it lives in the
 * APK's AndroidManifest.xml, which was not part of the binary set this port
 * was derived from. There is therefore NO derived value to put here, and a
 * plausible-looking guess is worse than an obvious placeholder: both the
 * Zookeeper and PvZ ports shipped silently reporting the WRONG package name
 * because someone inherited a string that looked right.
 *
 * Get the real one:
 *     aapt dump badging KillerBean.apk | head -1
 * and paste the exact string here.
 *
 * Killer Bean Unleashed also ships Google Play Billing (com.android.billingclient
 * is in classes.dex) and Unity Ads. Both are stubbed by the loader, but an IAP
 * stub that reports a mismatched package can make the store path take a
 * different branch, so getting this right is not merely cosmetic.            */
/* RESOLVED from classes.dex -- no manifest needed after all.
 * The dex defines com/KillerBeanStudios/KillerBeanUnleashed/BuildConfig with an
 * APPLICATION_ID field, and the dotted form appears verbatim exactly once in
 * the string pool:  "com.KillerBeanStudios.KillerBeanUnleashed"
 * Note the CamelCase segments -- this is not the all-lowercase form you would
 * guess from the studio name, which is precisely why the placeholder was left
 * in rather than filled with something plausible. */
#define GAME_PACKAGE "com.KillerBeanStudios.KillerBeanUnleashed"
/* CONFIRMED STILL UNSET as of the first boot log:
 *     Unity: ApplicationInfo PUT.YOUR.PACKAGE.NAME.HERE version 2.1.6
 * The engine reports it verbatim as Application.identifier. Not the cause of
 * the splash hang, but set it before blaming anything on the IAP/store path. */

/* ---- split-asset auto-join (nx_splitjoin.c) ------------------------------
 * INERT FOR THIS GAME. Fruit Ninja shipped some assets as 1 MiB .split0/.split1
 * parts that Unity reassembles in Java on a phone; Killer Bean Unleashed does
 * not -- its asset set was surveyed and contains ZERO .split parts (it ships a
 * single 54 MB data.unity3d instead). The join pass runs at first boot, finds
 * no part sets, logs "0 set(s) found" and costs nothing.
 *
 * The code is kept rather than deleted so that a future game update which
 * introduces splits is handled without re-porting the module.                */
#define JOIN_DELETE_PARTS 0

/* Diagnostic: trace futex WAIT/WAKE in the contended allocator region to
 * locate a lost wake. Verbose; enabled for this bring-up build only. */
#define KB_FUTEX_TRACE 0

#define CONFIG_NAME "config.txt"
/* Resolved at RUNTIME from the .nro's own location -- see nx_data_root.h.
 * The old compile-time strings are kept only as the fallback default, so a
 * folder name that does not match no longer bricks the boot (and no longer
 * hides the log, which used to live under the same wrong root). */
#define LOG_NAME    g_log_path

// Returned for getenv("HOME")/getpwuid()->pw_dir. Point it at the (writable)
// game data root instead of letting the engine deref a NULL passwd.
#define GAME_HOME   ((const char *)g_data_root)
#ifndef DATA_ROOT
#define DATA_ROOT   ((const char *)g_data_root)
#endif

// flip to 1 (and rebuild) to get file logging (debug.log) for on-hardware debugging
/* File logging.
 *
 *   1 = ON  [shipped]. debug.log is written normally: boot trace, per-frame
 *           diagnostics, [gc]/[mem] notes and the [xd] crash dump.
 *
 *   0 = ABSOLUTE SILENCE. debug.log is never created -- not by a crash, not by
 *       a "note", not at all. Every logging entry point compiles to an empty
 *       stub, so there is no file, no formatting cost and no SD traffic.
 *
 * Note that 0 does NOT disable the watchdog thread: it is also the escape hatch
 * that undoes a wedged GC stop-the-world (round 101), so it always runs. */
#define DEBUG_LOG 1   /* release build: no debug.log, no SD traffic. Set to 1
                       * when investigating anything -- every diagnostic in this
                       * tree goes dark at 0, including the crash dump and the
                       * JNI approximation ledger. The watchdog still runs; it is
                       * not a logger, it is what un-wedges a stalled GC. */

/* GC stop-the-world (round 100).
 *
 *   1 = CORRECT. Mutator threads are really paused while the collector marks.
 *       This is what a garbage collector requires, and without it the mark loop
 *       can read a half-published object and fault (round 99: klass == 0).
 *       The cost is real: the game's worker threads are stopped for the whole
 *       mark, so a large collection shows up as an occasional frame hitch.
 *
 *   0 = OLD BEHAVIOUR. Ack the suspend without pausing anyone. Smooth, and
 *       wrong -- this is the configuration that crashed ~20% of the time when
 *       starting a new game.
 *
 * Left ON: an occasional hitch is a better failure than a crash. Flip it to 0
 * if you would rather have the old behaviour back. */
#define KB_GC_STOP_WORLD 1

/* Strip "gc-max-time-slice" from boot.config as it is served to the engine.
 *
 * That key puts Unity's collector in INCREMENTAL mode: marking is split across
 * many short slices with the mutators running in between, and the invariant is
 * held together by write barriers plus a correct stop-the-world for each slice.
 * This port's stop-the-world is not reliable enough for that -- it works for ~32
 * collections in 33 and bails on the rest (round 110) -- and every crash so far
 * has landed in the mark loop reading a klass of 0, which is what a broken
 * incremental invariant looks like.
 *
 * With the key removed the collector runs non-incremental: fewer, larger, atomic
 * collections, with no between-slice invariant to violate. Expect occasional
 * longer pauses in exchange.
 *
 * 0 restores the game's shipped setting. */
#define KB_GC_NON_INCREMENTAL 1

/* High-volume per-operation traces. These were invaluable for the black-screen /
 * boot-hang triage but are catastrophic for load speed once the game runs: every
 * data.unity3d read/lseek and most mprot calls fflush two lines to the SD card, so
 * a synchronous scene load (~1700 bundle reads) takes minutes instead of seconds
 * and looks like a hang. Keep them OFF for normal play; flip to 1 to re-trace. */
#define TRACE_BUNDLE_IO 0   /* per-read/lseek trace of globalgamemanagers */
#define TRACE_MPROT     0   /* per-mprotect commit trace */

/* Per-frame / per-asset traces that dominate the log once the game runs:
 * the doFrame proxy line (every frame), [io] open (every asset, ~3x), and
 * the clock-stall beacon. Off = a readable log and far less SD I/O; flip to
 * 1 only to re-trace JNI proxy dispatch or asset open order. */
#define LOG_VERBOSE 0

/* Mutex ownership tracker (self-deadlock / holder naming). It takes a global
 * lock on every pthread_mutex op, which serialises the engine's mutex
 * traffic and reorders acquisition -- fine for a one-off deadlock hunt, but
 * it must be OFF for normal runs or it changes timing enough to hang boot.
 * Flip to 1 only to re-diagnose a mutex deadlock. */
#define MTXOWN_ENABLE 0

/* Force glFinish() before eglSwapBuffers on the first N presents, to work
 * around / localise the first-present hang (mesa blocking in its flush/
 * fence phase). 0 disables. A small number (a few frames) is enough to get
 * past the initial present without serialising steady-state rendering. */
/* round 63: adaptive futex re-poll floor. 250us gives fast recovery of
 * silent-writer waits (the async-load bottleneck) while backoff keeps idle
 * threads cheap. Lower = faster loads but more wakeups. */
#define KB_FUTEX_HOP_MIN 250000ULL
/* round 70: re-poll CEILING. Handoffs whose wake never arrives directly cost
 * one tick of this, so it sets the load speed. 16ms was the old value and is
 * why loading crawled; 1ms is ~16x faster. Lower = faster loads, more CPU. */
#define KB_FUTEX_HOP_MAX 1000000ULL
/* round 64: vsync/Choreographer pulse period. 16ms == ~60fps cap; the load
 * is frame-gated so a shorter period renders (and loads) faster. Delta-time
 * is hooked so game speed is unchanged. */
#define KB_VSYNC_PERIOD_NS 16000000ULL
#define KB_SWAP_FINISH_N 8

/* Round 153: minimum number of ENGINE FRAMES a touch stays down before its UP
 * is injected. 0 disables the hold entirely (round-152 behaviour).
 *
 * Why it exists: we inject on our own HID poll, Android delivers on the UI
 * thread and the engine samples once per frame. A tap that fits between two
 * Update() calls arrives as a single touch already in phase Ended. uGUI still
 * fires the click (ProcessTouchPress takes pressed and released in one call),
 * which is why the weapon-store buttons work. LeanTouch does not -- it records
 * fingers across frames and drops an UP for a finger it never saw pressed --
 * and the level map is LeanTouch (LevelMap_Button holds a LeanSelectable).
 *
 * 2 frames is one full sampled frame with the finger down, ~33 ms at 60 Hz,
 * shorter than any real tap. Raise it only if the log still shows taps landing
 * inside one frame. */
#define KB_TOUCH_MIN_FRAMES 0   /* round 153: measured, taps already span 4-11
                                 * engine frames (DOWN frame=1115, UP frame=1119),
                                 * so nothing was ever collapsing. Theory dead;
                                 * the hold never fired and is off. */

/* Round 153 experiment: force LeanTouch.PointOverGui() to false.
 * The level map is LeanTouch end to end -- LevelMap_Button.Update polls
 * LeanSelectable.IsSelected -- and LeanTouch's selectors skip any finger whose
 * StartedOverGui is set, which is PointOverGui at the DOWN via
 * EventSystem.RaycastAll. uGUI never consults it, which matches the observed
 * split. 1 = force false and log the first 20 calls, 0 = leave the game's own
 * answer alone. */
#define KB_LEAN_POINTOVERGUI_FALSE 0   /* round 154: measured -- PointOverGui was
                                        * never called ONCE across 40+ taps, so no
                                        * finger was ever created and this was not
                                        * the gate. Off. */

/* Round 154, THE fix: answer LeanInput.GetTouchCount()/GetTouch() from the
 * loader's own pointer state.
 *
 * LeanInput reads UnityEngine.InputSystem.EnhancedTouch (the NEW input system);
 * we inject via UnityPlayer.nativeInjectEvent, which is the LEGACY path. Android
 * feeds both from one MotionEvent, we feed one -- so uGUI works and every
 * LeanTouch-driven control, which on the level map is all of them, is dead.
 * 0 restores the game's own (empty) answer. */
#define KB_LEAN_INPUT_FROM_LOADER 1   /* round 159: the new input system is
                                       * confirmed working, so this hook is now
                                       * redundant -- LeanInput reads
                                       * EnhancedTouch, which reads the
                                       * Touchscreen device we feed. Left ON so
                                       * this build changes one thing at a time;
                                       * set to 0 next round and check the map
                                       * nodes still select, then delete it. */

/* Round 157: feed UnityEngine.InputSystem for real, instead of hooking the
 * things that read it. See source/nx_newinput.c for the full derivation --
 * device via InputSystem.AddDevice("Touchscreen") through the exported il2cpp
 * API, events via NativeInputSystem::QueueInputEvent, no code patching.
 *
 * If this works, KB_LEAN_INPUT_FROM_LOADER above becomes redundant (LeanInput
 * reads EnhancedTouch, which reads the device we are now feeding) and should be
 * turned off so there is one input path rather than two. Leave it ON for the
 * first run: if the new path fails, the map still works as it does today. */
#define KB_NEW_INPUT_SYSTEM 1

/* Round 159: map the physical buttons onto the game's on-screen HUD by
 * injecting touches where its sprites are drawn (see source/kb_input.c). The
 * bindings go silent whenever the cursor is visible, so the cursor and the
 * buttons never fight over the same stick or press a menu control that happens
 * to sit under a bound position. */
#define KB_CONTROLLER_TOUCH 1

/* Round 159: ONE cursor instead of two. With the physical buttons now mapped
 * onto the game's own HUD (kb_input.c), a second cursor is not a second way to
 * point -- it is a second thing competing for the sticks and triggers. In
 * single mode either stick moves the one cursor, either trigger taps, L+R
 * together recenters it (singly they are the weapon-switch arrows), and the
 * second cursor is neither drawn nor reported as a finger. 0 restores the
 * original dual-cursor behaviour. */
#define KB_SINGLE_CURSOR 1

/* Round 160: restore entitlements listed in <data_root>/purchases.txt by
 * calling the game's own Owned_* grant. Play billing does not exist on this
 * console -- the store cannot initialise and cannot sell anything -- so this is
 * the only way to use items already bought on the player's Play account. The
 * file is generated commented-out: nothing is unlocked until the player says
 * so. See source/kb_purchases.c. */
#define KB_OFFLINE_PURCHASES 1

/* Round 162: refuse to overwrite a non-empty save value with an empty one.
 * A game that writes "" over real progress has had a READ fail, and persisting
 * that turns a transient failure into a permanent wipe. Keeps the last
 * non-empty value and logs every block. Cost: an intentional in-game erase of a
 * save slot may not stick. 0 restores plain last-write-wins. */
#define KB_PROTECT_SAVES 1

/* Round 165: how many freed JNI reference STRUCTS to keep mapped before
 * actually returning them to the allocator. The engine uses references after
 * deleting them; retiring the struct instead of freeing it means the stale read
 * sees tag == 0 ("not one of ours") rather than 0xDE poison, so the guards
 * answer safely instead of faulting. Structs only -- payloads are still freed
 * immediately -- so 512 costs under 35 KB (FakeObject is the largest at 68 bytes). 0 disables it and frees
 * immediately, which is the round-164 behaviour that crashed. */
#define KB_REF_QUARANTINE 512

/* Round 155 DIAGNOSTIC. Replace LevelMap_Control.PlayLevel()/Menu() with two
 * log lines to find out whether the map's uGUI clicks arrive at all. The bodies
 * do NOT run while this is on -- which costs nothing, since both buttons are
 * already dead. Turn it off once the log has answered. */
#define KB_LEVELMAP_BUTTON_PROBE 0   /* round 156: answered. Both probes installed
                                      * and NEVER fired, so the click was not
                                      * reaching the handler -- the fault was the
                                      * EventSystem, not the handler. Off, so the
                                      * real PlayLevel/Menu bodies run again. */

/* Round 156, THE fix for the map's uGUI: NOP the `if (current != this) return`
 * at the top of EventSystem.Update so every enabled EventSystem ticks. level3
 * ships two of them and the new-Input-System one wins, starving the legacy
 * module we actually feed. Harmless in every other scene, which has only one
 * EventSystem and so never took that branch. 0 restores stock behaviour. */
#define KB_EVENTSYSTEM_TICK_ALL 0   /* round 157: MEASURED AND REVERTED. The NOP
                                     * applied cleanly and Play/Menu still did
                                     * nothing, so the current!=this branch was
                                     * not the gate. It was a hack standing in
                                     * for the real problem -- that we never feed
                                     * the new Input System at all. See
                                     * tools/newinput_derivation_raw.txt. */

/* round 151: how long the COLLECTOR may wait on a futex with the world still
 * stopped, before falling back to resuming everyone (the bailout).
 *
 * 0 = OFF. The collector resumes the world the moment a wait would really
 *     block, which is the round-150 behaviour minus the spurious cases.
 *     Start here: gc_collector_futex_wait() first has to show, in the log,
 *     that any wait blocks at all.
 *
 * Non-zero = poll for that many ns before giving up. Worth turning on ONLY if
 *     the log shows `slept=` climbing, i.e. real blocking. The bet it makes is
 *     that the waker is a thread the stop-the-world never paused (Boehm's own
 *     marker threads are not in GC_threads, and neither is anything started
 *     before GC init), in which case the wake lands and the collection stays
 *     sound. 2 ms is a sensible first value: long enough for a runnable thread
 *     to reach its wake, far short of the 60 ms watchdog, and under a frame.
 *
 * The hop is the poll interval. Do not raise the budget past ~10 ms without
 * also raising the watchdog, or the watchdog will fire first and force-resume,
 * which is the outcome this is trying to avoid. */
/* ROUND 152: turned ON, on the round-151 evidence. That run recorded
 *   nosleep=0 slept=1 rescued=0
 *   BAILOUT #1 ... futex ua=0x84c1ca0c0 *ua=00000001 expected=00000000
 *                  caller=unity+0x98e2e4
 *   collector-wait ua=0x84c1ca0c0: 1 wake(s), last waker tid=7718
 *                  <- a RUNNING thread wakes this
 * Read it in order: the wait would genuinely have blocked (so the round-150
 * "every bailout is spurious" theory is dead), but the word had already moved
 * to 1 by the time the bailout line printed a few microseconds later, and the
 * waker was a thread the stop-the-world never paused. The wake was in flight;
 * we resumed the world rather than wait for it. A 2 ms poll catches that with
 * ~1000x margin and keeps the collection sound. */
#define KB_GC_FUTEX_INEPOCH_NS     2000000ULL   /* 2 ms */
#define KB_GC_FUTEX_INEPOCH_HOP_NS 50000ULL     /* 50 us */

/* ---- libil2cpp hook gates (see patches/patch_sources.py) -----------------
 * libil2cpp is GAME CODE: every offset into it is specific to one build of one
 * game. The PvZ core hardcodes hooks at PvZ's offsets. Both are OFF here because
 * neither was re-derived for Fruit Ninja; turning one on without re-deriving it
 * first will patch unrelated functions. Symptoms and method: PORTING sec 6.    */
#define KB_HAVE_TIME_HOOKS 1   /* DERIVED from dump.cs for THIS build: 8 of 9
                                * Time.get_* icall thunks located and verified by
                                * disassembly + their own icall name strings.
                                * smoothDeltaTime is absent from this build and
                                * is left at 0. See nx_patch_killerbean.h. */
#define KB_FORCE_SPLASH_FINISH 1  /* round 57: skip the stuck end-of-fade animation-event gate */
/* round 68: async-load integration budget, ms per frame. Unity default is
 * 4ms (High would be 50). Bigger = faster scene loads, fewer frames during
 * loading. 0 disables the patch. */
#define KB_PRELOAD_BUDGET_MS 4
/* round 75: NOP UpdatePreloading's early-exit branch so the main thread keeps
 * retrying SingleStep for the whole budget instead of yielding the frame the
 * moment the integrate queue is briefly empty. 0 disables. */
#define KB_PRELOAD_NO_EARLY_EXIT 1
/* round 69: per-asset "[io] DATA open" trace. Costs an extra fstat plus a
 * log line for each of the ~2200 assets loaded, so keep it off by default. */
#define KB_TRACE_DATA_IO 0
/* round 79: build+mount the Subway-Surfers-style asset pack. First boot
 * packs assets/bin/Data into one file; later boots mount it. 0 = off. */
#define KB_ASSET_PACK 1
#define KB_BYPASS_UNITY_SPLASH 1  /* round 58: GetShouldShowSplashScreen->0 (native Unity splash) */
/* JNI approximation ledger (round 130). Records every JNI call answered by a
 * catch-all -- empty string, NULL object, 0, no-op -- deduped and counted, and
 * marks the ones Unity reads back via ExceptionCheck. Android raises at this
 * boundary and we cannot; this is the half of that which costs nothing. The
 * ledger is reprinted in full on any crash dump, so a fault log now carries
 * "here is everything we faked" instead of needing a separate run.
 * Set to 0 for absolute silence (DEBUG_LOG 0 already suppresses the output). */
#define KB_JNI_LOUD 0   /* release build */

/* Poison freed newlib blocks with 0xDE (no quarantine, nothing retained).
 * Makes use-after-free deterministic instead of layout-dependent -- see the
 * comment in nx_alloc.c's nx_free_inner. Costs one memset per free; set to 0 to
 * restore the previous behaviour exactly. */
#define KB_POISON_FREE 1


/* Diagnostic ONLY: call il2cpp_gc_disable() after init so no collection ever
 * runs. Answers "is the corruption premature reclamation?" in one session.
 * Memory grows unbounded -- never ship with this set to 1. */
#define KB_GC_DISABLE 0

/* The on-device asset-pack payload verifier is GONE (round 142). It ran once,
 * printed "verify OK: payload matches header (b3e645de7a86ae28)", and that is
 * recorded -- it had no business costing a 255 MB blocking read on the boot
 * path. The same check now lives in tools/verify_pack.py, run on a PC. */

/* Region-index level-1 fix: the two `lsr #40 -> #38` sites that pair with the
 * widened level-2 field.
 *
 * NO LONGER A GUESS -- CONFIRMED against a symbolized Unity 2019.4.36f1
 * reference and an independently-derived, verified port for that engine
 * (Ticket to Earth). 2019.4 spells the same level-1 index as TWO instructions
 * that 2021.3's compiler folds into one:
 *
 *     2019.4   lsr x8, x1, #0x1c ; asr w8, w8, #0xc     -> net ptr >> 40
 *     2021.3   lsr x8, x1, #0x28                        -> ptr >> 40
 *
 * That port patches the 2019.4 `lsr #28` down to #26 (leaving the fixed #12
 * field width alone), giving net ptr >> 38. Our `lsr #40 -> #38` is the
 * arithmetically identical transform on the folded form. It patches this site
 * in BOTH GetAllocatorContainingPtr and GetBlockInfoFromPointer -- exactly the
 * two functions involved here.
 *
 * Keep at 1. The switch stays only as a bisect handle.
 *   1 = 23 sites (correct: level 1 moves with level 2)
 *   0 = 21 sites (level 1 left behind -> pointers above 256GB alias) */
/* Hook LevelMap_WhatsNew.CloseWhatsNew. This is a FULL REPLACEMENT of the
 * managed method, not a trampoline -- the original body never runs. It answers
 * whether the popup's close button reaches its handler at all, and doubles as a
 * workaround by doing the one thing that matters: hiding the panel.
 * See nx_patch_killerbean.h. Set to 0 for stock behaviour. */
/* When the What's New popup is up, force any SIBLING panel that is also live
 * to hide. LevelMap_WhatsNew owns three panels but Start() only ever sets the
 * active state of panel_whatsnew -- the other two keep whatever the scene
 * shipped. A live full-screen sibling sits above the close button in the
 * raycast order and absorbs every tap, which matches the one symptom left: the
 * only dead button in a game whose UI otherwise works everywhere.
 *
 * Only acts when a sibling AND panel_whatsnew are BOTH activeInHierarchy, so it
 * cannot hide a panel the game is legitimately showing on its own. Set to 0 to
 * observe without intervening. */
/* MASTER SWITCH for every LevelMap_WhatsNew hook -- the Awake trampoline, the
 * three close replacements and the Start replacement.
 *
 * DEFAULT 0. The level-selection UI stopped working after these were added and
 * the popup was never dismissable anyway, so the honest position is that they
 * cost more than they bought. Turning the whole family off in one place gives a
 * known state to measure from, instead of leaving five hooks live while
 * guessing which one is responsible.
 *
 * Set to 1 to re-enable the family; the individual gates below still apply
 * within it. Re-enable ONE at a time -- that is the step that was skipped when
 * they were introduced together. */
#ifndef KB_WHATSNEW_HOOKS
#define KB_WHATSNEW_HOOKS 1
#endif

/* The family is now enabled but split, so exactly ONE hook installs by default:
 * Start(skip). Turning everything off at once created a catch-22 -- the popup
 * blocks the level-selection UI, so the UI could not be tested to find out
 * whether the hooks broke it.
 *
 * Start(skip) is the right one to run alone:
 *   - it is the only hook that is actually needed (it suppresses the popup);
 *   - it executes INSIDE the engine's own call to Start(), so unlike the
 *     removed heartbeat sweep it cannot mutate engine state off-cycle;
 *   - it is a full replacement of a method whose only observable effect is one
 *     SetActive call, so its blast radius is exactly that call.
 *
 * The other two are OFF and should be re-enabled one at a time, if at all:
 *   KB_WN_AWAKE_TRAMPOLINE  hand-written asm; the most likely to misbehave and
 *                           purely diagnostic -- it buys nothing at runtime.
 *   KB_WN_CLOSE_HOOKS       three close replacements; pointless while the
 *                           buttons cannot be clicked in the first place. */
#ifndef KB_WN_AWAKE_TRAMPOLINE
#define KB_WN_AWAKE_TRAMPOLINE 0
#endif
#ifndef KB_WN_CLOSE_HOOKS
#define KB_WN_CLOSE_HOOKS 0
#endif

/* Skip the "What's New" popup outright.
 *
 * Its close button cannot be clicked on this port -- the input path is clean and
 * the handler is correctly wired, but the tap never reaches it -- so the popup
 * is a hard block on reaching the level map. Rather than keep chasing the
 * raycast, do not show it.
 *
 * Two layers:
 *   1. LevelMap_WhatsNew.Start() is replaced. Its ONLY effect is
 *        panel_whatsnew.SetActive(savedVersion != currentVersion)
 *      -- everything before that is a once-flag and two class-init calls -- so
 *      replacing it with SetActive(panel, false) is behaviour-preserving apart
 *      from the popup never appearing.
 *   2. A per-heartbeat sweep hides any of the three panels that goes live
 *      later, which also covers panel_NewDay and panel_SpecialBonus. Their
 *      close buttons are presumably just as unclickable, so the same block
 *      would otherwise recur on a daily-refresh or bonus popup.
 *
 * Cost: the version flag is never written, so the game still thinks the popup
 * is pending. Harmless -- it is suppressed again every launch. Set to 0 to
 * restore stock behaviour. */
#ifndef KB_SKIP_WHATSNEW
#define KB_SKIP_WHATSNEW 1
#endif

#ifndef KB_HIDE_BLOCKING_PANELS
#define KB_HIDE_BLOCKING_PANELS 1
#endif

#ifndef KB_HOOK_CLOSE_WHATSNEW
#define KB_HOOK_CLOSE_WHATSNEW 1
#endif

#ifndef KB_REGION_L1_FIX          /* -DKB_REGION_L1_FIX=0 overrides without editing */
#define KB_REGION_L1_FIX 1
#endif

#define KB_HAVE_GC_BRIDGE  1   /* DERIVED from THIS libil2cpp (8 globals) -- see nx_patch_killerbean.h */

extern int screen_width;
extern int screen_height;

/* ----------------------------- Language ----------------------------------
 * NOT INHERITED FROM PvZ -- re-derived for this game, and it works differently.
 *
 * PvZ's build read its locale as a STRING from an AndroidJavaClass, so its
 * config mapped 1/2 onto the literals "en"/"zh" returned by jni_fake's
 * getLanguage(). Fruit Ninja does not do that. Its managed code calls
 *
 *     UnityEngine.Application::get_systemLanguage()
 *
 * which returns Unity's SystemLanguage ENUM. The engine populates that at init
 * from the Java locale, so jni_fake's getLanguage() still feeds it -- but the
 * game compares against an enum, not against our string, so there is no
 * game-specific token to guess.
 *
 * Practical consequence: LANG_AUTO is the only value with confirmed meaning.
 * The overrides below simply force the locale jni_fake reports; verify on
 * hardware which SystemLanguage the engine derives before trusting them.
 *
 * (The binary also carries I18N.CJK/MidEast/Other/Rare/West and RTLTMPro, so
 * the title has broad language coverage including right-to-left. The ar-*
 * tokens visible in libil2cpp are mscorlib CULTURE TABLES from I18N.MidEast,
 * not a list of shipped game languages -- do not read them as one.)        */
#define LANG_AUTO 0   /* follow the Switch system language -- recommended */
#define LANG_EN   1   /* force en */
#define LANG_ZH   2   /* force zh -- retained from the base; UNVERIFIED here */

/* ORIENTATION -- almost certainly LANDSCAPE, but VERIFY IT ANYWAY.
 *
 * What is established from the binaries you supplied: the game's own managed
 * code never touches ScreenOrientation. Every ScreenOrientation symbol in
 * dump.cs belongs to UnityEngine itself (Screen.orientation, the abstract
 * ScreenOrientation properties on the display/device classes), not to
 * Assembly-CSharp or to Corgi Engine. There are no "Landscape"/"Portrait"
 * literals in the game assemblies and none in classes.dex.
 *
 * So the APK's AndroidManifest.xml (android:screenOrientation) is
 * AUTHORITATIVE, and the manifest was NOT part of the file set this port was
 * derived from. Check it yourself:
 *     aapt dump badging KillerBean.apk | grep -i orientation
 *
 *   landscape -> nothing to do; this build is correct as-is.
 *   portrait  -> restore the Zookeeper TATE compositor-rotation path, AND
 *                revisit nx_dual_pointer, which maps the panel to render space
 *                with a straight stretch and applies no rotation. Aiming will
 *                be wrong otherwise -- silently, which is the worst kind.
 *
 * (Killer Bean Unleashed is a side-scrolling shooter and is landscape in every
 * published build, so the expected answer is landscape. That is an argument
 * from the genre, not from your binaries, which is why it is not treated here
 * as settled.)                                                             */

/* INPUT MODE -- the single most important design decision in this port.
 *
 * Corgi Engine's InputManager exposes a built-in desktop/gamepad path. From
 * dump.cs, verified field layout of InputManager : MMSingleton<InputManager>:
 *
 *     +0x18  bool                InputDetectionActive
 *     +0x28  bool                AutoMobileDetection
 *     +0x2C  InputForcedMode     ForcedMode      { None=0, Mobile=1, Desktop=2 }
 *     +0x30  bool                HideMobileControlsInEditor
 *     +0x34  MovementControls    MovementControl
 *     +0x39  bool                <IsMobile>k__BackingField
 *
 * That gives two viable strategies:
 *
 *   A. SYNTHETIC TOUCH (what this build does, and what you should boot first).
 *      Feed MotionEvents through nativeInjectEvent so the game's existing
 *      on-screen MMTouchButton / MMTouchJoystick widgets are pressed by a
 *      cursor driven from the sticks. Requires no managed patching at all, so
 *      it cannot desync from game logic, and it is known-good in this loader
 *      lineage. Cost: you are aiming a virtual finger at virtual buttons.
 *
 *   B. FORCED DESKTOP MODE (better feel, more work, NOT enabled here).
 *      Write ForcedMode = Desktop (2) and IsMobile = false into the
 *      InputManager singleton after it is constructed, then satisfy Corgi's
 *      desktop reads. This is how you would get true 1:1 stick aiming and
 *      real button mapping. It is left OFF because it needs the singleton
 *      instance address at runtime, which must be resolved for THIS build,
 *      and a half-applied switch leaves the game reading neither path.
 *      See PORTING_KILLERBEAN.md sec 6.2 for the procedure.               */
typedef struct {
  int handheld_res;   /* render height in handheld mode: 720 or 1080 */
  int docked_res;     /* render height when docked:      720 or 1080 */
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
