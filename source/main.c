/* main.c -- ZOOKEEPER DX Switch wrapper entry point.
 *
 * Unity 2022.3 / IL2CPP. Loads libmain + libunity + libil2cpp, then drives the
 * lifecycle the Java UnityPlayer normally runs (initJni -> recreate GFX state ->
 * render loop), calling the native entry points recovered from libunity.so's
 * JNI_OnLoad (see unity_entrypoints.h). The engine owns its own EGL/GLES3 context
 * created from android_native_window(); SDL is audio/HID only.
 *
 * Heap + syscall scaffolding adapted from cr3_nx's main.c (MIT).
 */

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "nx_data_root.h"
#include "config.h"
#include "nx_patch_killerbean.h"  /* Fruit Ninja: recovered libunity patch + flags */
#include <dirent.h>
#include <strings.h>  /* strncasecmp */
#include "util.h"
#include "error.h"
#include <sys/statvfs.h>
#if KB_ASSET_PACK
#include "asset_pack.h"
#endif
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "opensles.h"
#include "unity_entrypoints.h"
#include "diag.h"

/* Printed by the compiler on every build. With DEBUG_LOG 0 there is no boot log
 * line to carry the revision, so this is the only place a stale tree shows up:
 * if the build output does not name the rev you just unzipped, you are building
 * something else. */
#define NX_STR2(x) #x
#define NX_STR(x) NX_STR2(x)
#pragma message ("building " GAME_NAME "_nx source rev " KB_SRC_REV " (DEBUG_LOG=" NX_STR(DEBUG_LOG) ")")

/* DATA_ROOT is now a runtime value (nx_data_root.h); config.h defines it. */
#define LIB_MAIN   "libmain.so"
#define LIB_UNITY  "libunity.so"
#define LIB_IL2CPP "libil2cpp.so"
#define LIB_FB_APP          "libFirebaseCppApp-11_9_0.so"
#define LIB_FB_ANALYTICS    "libFirebaseCppAnalytics.so"
#define LIB_FB_MESSAGING    "libFirebaseCppMessaging.so"
#define LIB_FB_REMOTECONFIG "libFirebaseCppRemoteConfig.so"

void unity_environment_init(const char *data_root);   /* unity_glue.c */

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* mmap arena. In overcommit mode (g_overcommit) this is a big *virtual* window in
 * the alias region: Unity's PROT_NONE pool reservations cost only address space
 * and physical pages are committed on demand via svcMapPhysicalMemory. In the
 * fallback path it's a fully heap-backed 256MB-aligned slab (the Switch has no
 * native overcommit). Consumed by mmap_fake/munmap_fake (libc_shim.c). */
void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
int    g_overcommit      = 0;          /* 1 = alias-region on-demand commit */
u64    g_alias_base = 0, g_alias_size = 0;
/* captured in __libnx_initheap for logging from main() (log file isn't open yet) */
unsigned g_oc_heap_mb = 0, g_oc_freed_mb = 0;
/* granular setup diagnostics so a failed gate tells us WHICH step bailed */
int      g_oc_hint_map = 0, g_oc_hint_unmap = 0;
unsigned g_oc_alias_mb = 0;
void    *g_oc_win = NULL;
int      g_oc_probe_tried = 0, g_oc_shrink_tried = 0;
/* stack-region overcommit arena armer (libc_shim.c) */
extern int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes);
unsigned g_oc_probe_rc = 0, g_oc_shrink_rc = 0;
unsigned long g_oc_win_addr = 0;
u64      g_oc_sysres = 0;   /* system resource size (0 => svcMapPhysicalMemory unusable) */

so_module main_mod, unity_mod, il2cpp_mod;

/* defined in libc_shim.c; consumed by the GC stop-the-world bridge there */
extern uintptr_t g_il2cpp_base;

/* Replacement icall for UnityEngine.Application::get_internetReachability.
 * Returns NetworkReachability.NotReachable (0). See the frame-0 install site
 * for why (unblocks FirebaseManager.IsGetMessage / the boot coroutine). The
 * il2cpp icall ABI for this static getter is "int32_t func(MethodInfo*)"; we
 * ignore the hidden arg and just report no network. */
static int32_t nx_internet_reachability(void) { return 0; }

/* Replacement for Common.FirebaseManager.IsGetMessage (instance method, returns
 * bool). The FirebaseLoading boot state spins on this; force it complete and log
 * once so the run tells us whether state 5 is even reached. */
static int nx_rd(uintptr_t a);            /* fwd decl: readable-memory check  */
static uintptr_t nx_pd(uintptr_t a);      /* fwd decl: safe pointer deref      */
static int32_t nx_is_get_message(void) {
  static int once = 0;
  if (!once) { once = 1; debugPrintf("[hook] FirebaseManager.IsGetMessage reached -> forced 1\n"); }
  return 1;
}

/* Unity's native time base is frozen in our environment (it never advances the
 * engine clock -- likely it expects Android Choreographer frame timestamps we
 * don't deliver). Every managed Time.* accessor therefore reads a frozen value:
 * deltaTime==0, time/realtimeSinceStartup constant. That freezes all time-based
 * game logic -- DOTween (the boot logo fade), WaitForSeconds, etc. -- which is
 * what holds the black screen (the fade never completes, so boot never starts).
 * Work around it by driving our own monotonic frame clock and redirecting the
 * managed Time accessors to it. nx_time_tick() runs once per render-loop frame. */
static volatile float  g_unity_dt   = 1.0f / 60.0f;
static volatile double g_unity_time = 0.0;
static volatile uint32_t g_frame_count = 0;   /* Time.frameCount source */
static uint64_t g_time_prev_ns  = 0;
static uint64_t g_time_start_ns = 0;
/* Unity native vsync primitives (round 56). Android's Choreographer signals
 * these every frame; on Switch we must. Triple at libunity+0x110e8d0:
 * mutex @+0x0, cond @+0x28 (0x110e8f8), counter @+0x58 (0x110e928). Confirmed
 * the vsync waiter: its wait fn (0x5ff730) is called only from the frame-
 * pacing fn at 0x5d928c (TimeManager::Sync, GetActualTargetFrameRate,
 * DisplayInfo, Swappy getter). */
extern int pthread_mutex_lock_fake(pthread_mutex_t **);
extern int pthread_mutex_unlock_fake(pthread_mutex_t **);
extern int pthread_cond_broadcast_fake(pthread_cond_t **);
static pthread_mutex_t **g_vsync_mutex   = 0;
static pthread_cond_t  **g_vsync_cond    = 0;
static volatile uint64_t *g_vsync_counter = 0;
static uint64_t nx_now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void nx_time_tick(void) {
  uint64_t now = nx_now_ns();
  g_frame_count++;                    /* advance Time.frameCount once per frame */
  if (!g_time_start_ns) g_time_start_ns = now;
  if (g_time_prev_ns) {
    double dt = (double)(now - g_time_prev_ns) / 1e9;
    if (dt < 0) dt = 0;
    if (dt > 0.1) dt = 0.1;            /* clamp, mirrors Unity maximumDeltaTime */
    g_unity_dt = (float)dt;
    g_unity_time += dt;
  }
  g_time_prev_ns = now;
}
static float nx_delta_time(void) { return g_unity_dt; }
static float nx_time_f(void)     { return (float)g_unity_time; }
static int   nx_frame_count(void){ return (int)g_frame_count; }
uint32_t     port_frame_count(void){ return g_frame_count; } /* audio pump warmup gate */
static float nx_realtime_since_startup(void) {
  uint64_t now = nx_now_ns();
  if (!g_time_start_ns) g_time_start_ns = now;
  return (float)((double)(now - g_time_start_ns) / 1e9);
}

/* fbstub42: TimeManager::Update entry hook. The Switch port's player loop drives
 * Update with a frozen vsync timestamp as newTime, so deltaTime collapses to the
 * 1e-5 floor and every native time reader (the PreloadManager included) starves,
 * which is what wedges async scene loading (the resident-scene black screen). We
 * redirect Update's entry (libunity 0x446114) here, replay its tiny prologue
 * (frameCount++, aux counter++, pause check), then re-enter its body (0x446138 --
 * frameless, re-reads everything from x0) with newTime = GetTimeSinceStartup(),
 * the engine's own monotonic clock (0x446578) which DOES advance. Update then
 * derives all deltaTime variants and m_Time correctly. Offsets verified against
 * both the game binary and the symbolized 2022.3.62f2 reference engine. */
static void   (*g_unity_update_body)(void *, double) = NULL; /* 0x4e21bc Update body */
static void     *g_tm = NULL;                       /* captured TimeManager instance */
static void   *(*g_get_time_manager)(void) = NULL;  /* 0x4e2794 GetTimeManager() (subsystem 7) */
static uint64_t  g_clk_base_ns = 0;
static volatile uint64_t g_last_main_tick_ns = 0;
static Mutex     g_clock_lock;                       /* main-hook vs clock-thread */
static Thread    g_clock_thr;
#define CLOCK_STALL_NS 100000000ULL                  /* 100ms main silence => stalled */
/* Re-run Update's body with a wall-clock newTime so deltaTime/m_Time advance even
 * while UnityMain is parked in a synchronous scene-load (the frame-0 async hang). */
static void nx_clock_tick(void *tm) {
  uint64_t now = nx_now_ns();
  if (!g_clk_base_ns) g_clk_base_ns = now;
  double wall    = (double)(now - g_clk_base_ns) / 1e9;
  double sref    = *(volatile double *)((char *)tm + 0xe8);   /* m_StartupRef */
  double newTime = sref + wall;
  if (g_unity_update_body) g_unity_update_body(tm, newTime);
  /* Heartbeat: is the clock actually advancing the splash/menu transition?
   * Log time + frameCount every ~2s of wall clock. If time moves but the
   * screen stays black, the transition is not time-gated; if time is stuck,
   * this is the freeze. */
  { static uint64_t last_hb = 0;
    if (now - last_hb > 2000000000ull) {
      last_hb = now;
      extern volatile unsigned long long g_alloc_calls, g_alloc_ns;
      static unsigned long long a_calls0 = 0, a_ns0 = 0;
      unsigned long long ac = g_alloc_calls, an_ms = g_alloc_ns / 1000000ull;
      unsigned long long dac = ac - a_calls0, dan = an_ms - (a_ns0 / 1000000ull);
      a_calls0 = ac; a_ns0 = g_alloc_ns;
      extern volatile unsigned long long g_readdir_calls, g_readdir_ns, g_opendir_calls;
      static unsigned long long rd0 = 0, rdns0 = 0, od0 = 0;
      unsigned long long rd = g_readdir_calls, rdns = g_readdir_ns / 1000000ull, od = g_opendir_calls;
      unsigned long long drd = rd - rd0, drd_ms = rdns - (rdns0 / 1000000ull), dod = od - od0;
      rd0 = rd; rdns0 = g_readdir_ns; od0 = od;
      extern volatile unsigned long long g_read_calls, g_fsread_calls, g_fsread_ns, g_fsread_bytes, rc_hits, rc_fills;
      static unsigned long long r0 = 0, fr0 = 0, frns0 = 0, frby0 = 0, h0 = 0, f0 = 0;
      unsigned long long rdc = g_read_calls, frc = g_fsread_calls, frns = g_fsread_ns / 1000000ull;
      unsigned long long frkb = g_fsread_bytes / 1024ull, hh = rc_hits, ff = rc_fills;
      unsigned long long drc = rdc - r0, dfrc = frc - fr0, dfrns = frns - (frns0/1000000ull);
      unsigned long long dfrkb = frkb - (frby0/1024ull), dh = hh - h0, df = ff - f0;
      r0 = rdc; fr0 = frc; frns0 = g_fsread_ns; frby0 = g_fsread_bytes; h0 = hh; f0 = ff;
      extern volatile unsigned long long g_fx_wait, g_fx_eagain, g_fx_slept, g_fx_wake, g_fx_nosys;
      static unsigned long long x0 = 0, xe0 = 0, xs0 = 0, xw0 = 0, xn0 = 0;
      unsigned long long xw = g_fx_wait, xe = g_fx_eagain, xs = g_fx_slept,
                        xk = g_fx_wake, xn = g_fx_nosys;
      unsigned long long dxw = xw - x0, dxe = xe - xe0, dxs = xs - xs0,
                        dxk = xk - xw0, dxn = xn - xn0;
      x0 = xw; xe0 = xe; xs0 = xs; xw0 = xk; xn0 = xn;
      extern volatile unsigned long long g_mainwait_ns, g_mainwait_n;
      extern volatile unsigned long long g_mainwait_cond_ns, g_mainwait_cond_n;
      static unsigned long long mw0 = 0, mn0 = 0, cw0 = 0, cn0 = 0;
      unsigned long long mw = g_mainwait_ns / 1000000ull, mn = g_mainwait_n;
      unsigned long long cw = g_mainwait_cond_ns / 1000000ull, cn = g_mainwait_cond_n;
      unsigned long long dmw = mw - (mw0 / 1000000ull), dmn = mn - mn0;
      unsigned long long dcw = cw - (cw0 / 1000000ull), dcn = cn - cn0;
      mw0 = g_mainwait_ns; mn0 = mn; cw0 = g_mainwait_cond_ns; cn0 = cn;
      /* round 74: top blocked threads this window (tid=ms/waits). */
      { extern volatile int g_twait_tid[]; extern volatile unsigned long long g_twait_ns[];
        extern volatile unsigned long long g_twait_cnt[];
        static unsigned long long tns0[64], tcn0[64];
        char tb[220]; int tp = 0; tb[0] = 0;
        for (int pick = 0; pick < 5; pick++) {
          int best = -1; unsigned long long bestd = 0;
          for (int k = 0; k < 64; k++) {
            if (!g_twait_tid[k]) continue;
            unsigned long long d = g_twait_ns[k] - tns0[k];
            if (d > bestd) { bestd = d; best = k; }
          }
          if (best < 0 || bestd < 1000000ull) break;
          unsigned long long dc = g_twait_cnt[best] - tcn0[best];
          tp += snprintf(tb + tp, sizeof(tb) - (size_t)tp, " %d=%llums/%lluw",
                         g_twait_tid[best], bestd / 1000000ull, dc);
          tns0[best] = g_twait_ns[best]; tcn0[best] = g_twait_cnt[best];
          if (tp > 180) break;
        }
        for (int k = 0; k < 64; k++) { tns0[k] = g_twait_ns[k]; tcn0[k] = g_twait_cnt[k]; }
        if (tb[0]) debugPrintf("[blk] top:%s\n", tb);
        /* round 77: every ~10s dump EVERY thread, so threads that never reach
         * the top 5 (notably Loading.PreloadManager) are finally visible. */
        { static unsigned hb_n = 0; static unsigned long long all0[64];
          if ((++hb_n % 5) == 0) {
            for (int k = 0; k < 64; k++) {
              if (!g_twait_tid[k]) continue;
              unsigned long long d = g_twait_ns[k] - all0[k];
              debugPrintf("[blkall] tid=%d blocked=%llums\n",
                          g_twait_tid[k], d / 1000000ull);
            }
            for (int k = 0; k < 64; k++) all0[k] = g_twait_ns[k];
          } }
      }
      /* ---- live panel probe: REMOVED, and this is why -------------------
       * This block used to call GameObject.get_activeInHierarchy() and, worse,
       * GameObject.SetActive() from the heartbeat -- i.e. from our render loop,
       * at an arbitrary point in the frame rather than from managed code during
       * the engine's own update.
       *
       * SetActive is not a passive write. It cascades OnEnable/OnDisable, dirties
       * canvases, and reorders what the EventSystem raycasts against. Driving
       * that from outside the update cycle broke ALL UI on the level-selection
       * screen, not just the popup -- a far worse regression than the bug it was
       * chasing, and entirely self-inflicted.
       *
       * The lesson generalises: reading engine state off-cycle is merely
       * unreliable, but MUTATING it off-cycle corrupts the frame. The popup is
       * suppressed instead by replacing LevelMap_WhatsNew.Start(), which runs
       * inside the engine's own call at exactly the right moment. If a live
       * probe is ever wanted again it must be read-only, and even then the
       * values are a snapshot from mid-frame. */

      debugPrintf("[time] hb: Time.time=%llums wall=%llums frameCount=%llu "
                  "alloc_calls+=%llu alloc_ms+=%llu readdir+=%llu readdir_ms+=%llu opendir+=%llu "
                  "read+=%llu fsread+=%llu fsread_ms+=%llu fsread_KB+=%llu rc_hit+=%llu rc_fill+=%llu "
                  "fxwait+=%llu fxeagain+=%llu fxslept+=%llu fxwake+=%llu fxnosys+=%llu "
                  "mainblk_fx_ms+=%llu mainwaits_fx+=%llu "
                  "mainblk_cond_ms+=%llu mainwaits_cond+=%llu\n",
                  (unsigned long long)(newTime * 1000.0),
                  (unsigned long long)(wall * 1000.0),
                  (unsigned long long)*(volatile uint64_t *)((char *)tm + 0xc8),
                  dac, dan, drd, drd_ms, dod,
                  drc, dfrc, dfrns, dfrkb, dh, df,
                  dxw, dxe, dxs, dxk, dxn, dmw, dmn, dcw, dcn);
      { /* declared in libc_shim.h, which main.c does not include; a local
         * prototype avoids dragging the whole shim header in here. */
        extern void nx_arena_usage_mb(unsigned *, unsigned *, unsigned *);
        unsigned ar = 0, ac = 0, at = 0;
        nx_arena_usage_mb(&ar, &ac, &at);
        debugPrintf("[mem] arena reserved=%u MB committed=%u MB of %u MB\n",
                    ar, ac, at);
        /* Cross a pressure threshold and say so even in a release build. A
         * Unity out-of-memory shows up as a fault inside its own memory-report
         * formatter (registers full of "ALLOC_TYPETREE ... 0B | reserved"),
         * which tells you nothing about what filled up -- this does. */
        { /* How much of the session has gone into GC pauses. If total_ms is a
           * small fraction of wall time the stutter is not the collector. */
          extern void nx_gc_pause_stats(unsigned *, unsigned *, unsigned *);
          unsigned gn = 0, gt = 0, gm = 0;
          nx_gc_pause_stats(&gn, &gt, &gm);
          extern int nx_gc_bailouts(void);
          extern void nx_gc_capture_stats(unsigned *, unsigned *, unsigned *,
                                          unsigned *, unsigned *);
          unsigned cok = 0, cnt = 0, cnh = 0, cnc = 0, cbr = 0;
          nx_gc_capture_stats(&cok, &cnt, &cnh, &cnc, &cbr);
          /* captured = threads whose SP+registers reached the mark. The four
           * miss counters must stay 0: any of them means a thread was scanned
           * from a stale range, which is the r119..r137 bug. */
          { extern void nx_canary_stats(unsigned *, unsigned *, unsigned *);
            unsigned clive = 0, cev = 0, cslots = 0;
            nx_canary_stats(&clive, &cev, &cslots);
            /* Tail-canary coverage. A high evict count means the table is
             * thrashing and "no SMASH" proves little -- see nx_alloc.c. */
            static unsigned last_ev = 0;
            if (cev != last_ev || clive) {
              last_ev = cev;
              debugPrintf("[canary] live=%u of %u slots, evicted=%u%s\n",
                          clive, cslots, cev,
                          cev > cslots ? "   <- table thrashing: absence of SMASH is weak evidence" : "");
            } }
          if (gn) debugPrintf("[gc] pauses=%u total=%u ms max=%u ms bailouts=%d "
                              "captured=%u miss(thr/hnd/ctx/roots)=%u/%u/%u/%u\n",
                              gn, gt, gm, nx_gc_bailouts(), cok, cnt, cnh, cnc, cbr);
          /* Round 151. What the collector's futex waits actually did. nosleep
           * counts the waits that would have returned EAGAIN anyway -- under the
           * old code every one of those resumed the world and cost a sound
           * collection. slept counts the waits that would genuinely have
           * blocked; only those can now reach a bailout. */
          { extern void nx_gc_futex_stats(unsigned *, unsigned *, unsigned *);
            extern void nx_gc_futex_wakers(void);
            unsigned fns = 0, fsl = 0, frs = 0;
            nx_gc_futex_stats(&fns, &fsl, &frs);
            static unsigned last_sl = 0;
            if (fns || fsl)
              debugPrintf("[gc] collector futex waits: nosleep=%u (bailouts avoided) "
                          "slept=%u rescued=%u\n", fns, fsl, frs);
            if (fsl != last_sl) { last_sl = fsl; nx_gc_futex_wakers(); } }
        }
        if (at) {
          static unsigned hi = 0;
          const unsigned pct = (unsigned)((uint64_t)ar * 100u / at);
          if (pct >= 50 && pct / 25 > hi / 25) {
            hi = pct;
            debugLogNote("[mem] arena %u%% reserved (%u of %u MB)\n", pct, ar, at);
          }
        } }
      debugLogFlush();
    } }
}
static void nx_time_update_hook(void *tm) {
  g_tm = tm;
  { static int once = 0; if (!once) { once = 1; debugPrintf("[time] Update hook first fire (tm=%p)\n", tm); } }
  { extern volatile int g_main_tid; extern int gettid_fake(void);
    if (!g_main_tid) g_main_tid = gettid_fake(); }   /* round 72: unambiguously main */
  g_last_main_tick_ns = nx_now_ns();                          /* main thread is live */
  *(volatile uint64_t *)((char *)tm + 0xc8) += 1;            /* frameCount++ (prologue) */
  *(volatile uint32_t *)((char *)tm + 0xd0) += 1;            /* aux counter++           */
  if (*(volatile uint8_t *)((char *)tm + 0xf8) != 0) return; /* paused -> early return  */
  mutexLock(&g_clock_lock);
  nx_clock_tick(tm);
  mutexUnlock(&g_clock_lock);
}
static void nx_clock_thread(void *arg) {
  (void)arg;
  static uint8_t clk_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(clk_tls);
  uint64_t last_vsync_ns = 0;
  while (!jni_quit_requested) {
    svcSleepThread(8000000ULL);                              /* ~8ms keep-alive */
    /* Choreographer stand-in (round 56): pulse Unity's native vsync counter +
     * cond at ~60Hz so frame-pacing advances (drives the splash fade). */
    { uint64_t _n = nx_now_ns();
      if (g_vsync_mutex && g_vsync_cond && g_vsync_counter &&
          (_n - last_vsync_ns) >= KB_VSYNC_PERIOD_NS) {
        last_vsync_ns = _n;
        if (pthread_mutex_lock_fake(g_vsync_mutex) == 0) {
          ++*g_vsync_counter;
          pthread_cond_broadcast_fake(g_vsync_cond);
          pthread_mutex_unlock_fake(g_vsync_mutex);
        }
        static int _vlog = 0; if (_vlog < 2) { _vlog++; debugPrintf("[vsync] native vsync pulse armed (counter@%p)\n", (void*)g_vsync_counter); }
      } }
    /* Fetch the TimeManager singleton directly -- the entry hook at 0x4e2198 never
     * fires during PvZ's boot-coroutine save load (the player loop isn't ticking
     * Update yet), so g_tm stays NULL. GetTimeManager() returns it once the
     * subsystem exists; until then it's NULL and we skip. */
    void *tm = g_get_time_manager ? g_get_time_manager() : g_tm;
    { static void *seen = NULL; if (tm && tm != seen) { seen = tm; debugPrintf("[time] clock thread got TimeManager=%p (driving native clock)\n", tm); } }
    if (tm && g_unity_update_body &&
        (nx_now_ns() - g_last_main_tick_ns) > CLOCK_STALL_NS &&
        mutexTryLock(&g_clock_lock)) {                       /* only while main is silent */
      nx_clock_tick(tm);
#if LOG_VERBOSE
      { static unsigned _n = 0; if ((_n++ & 0x3f) == 0) debugPrintf("[time] clock thread driving Update (main stalled)\n"); }
#endif
      mutexUnlock(&g_clock_lock);
    }
  }
}
static void nx_install_time_fix(void) {
  uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
  /* ---- resume point: DERIVED AT RUNTIME, not assumed ---------------------
   * The 16-byte stub below clobbers the FIRST FOUR instructions of
   * TimeManager::Update. nx_time_update_hook() replicates everything those --
   * and the five that follow -- do:
   *
   *   +0x00  ldr  x8,  [x0,#0xc8]     frameCount
   *   +0x04  ldr  w9,  [x0,#0xd0]     aux counter
   *   +0x08  ldrb w10, [x0,#0xf8]     pause flag
   *   +0x0c  add  x8, x8, #1          <- last instruction the stub overwrites
   *   +0x10  add  w9, w9, #1
   *   +0x14  str  x8, [x0,#0xc8]
   *   +0x18  str  w9, [x0,#0xd0]
   *   +0x1c  cbz  w10, <body>
   *   +0x20  ret                      <- paused: bail out
   *   +0x24  ldr  d2, [x0,#0xe8]      <- THE REAL BODY STARTS HERE
   *
   * so the correct resume point is the instruction AFTER the ret.
   *
   * This used to be a hardcoded `entry + 16`, inherited from the 2022.3 port
   * where the prologue happened to be that long. On 2021.3 it is 0x24, and
   * resuming at +0x10 re-entered the sequence with x8/w9/w10 NEVER LOADED:
   * `str x8,[x0,#0xc8]` wrote a stale register into frameCount (observed: a
   * live libunity pointer, 0x7d79707f8) and `cbz w10` branched on garbage, so
   * the pause check -- and therefore the whole clock update -- was random.
   * The visible symptom was the game sitting on the splash forever.
   *
   * Scanning for the `ret` makes the resume point self-correcting rather than
   * a constant that silently rots. KB_TIME_UPDATE_BODY is kept as a
   * cross-check, and a disagreement is logged loudly. */
  {
    const uint32_t RET = 0xD65F03C0u;
    uintptr_t body = 0;
    for (unsigned o = 0; o < 0x80; o += 4) {
      if (*(volatile uint32_t *)(ub + KB_TIME_UPDATE_ENTRY + o) == RET) {
        body = KB_TIME_UPDATE_ENTRY + o + 4;
        break;
      }
    }
    if (!body) {
      debugPrintf("[time] SKIP Update hook: no `ret` within 0x80 of +0x%x -- "
                  "cannot locate the resume point safely\n",
                  (unsigned)KB_TIME_UPDATE_ENTRY);
      return;
    }
    if (body != KB_TIME_UPDATE_BODY)
      debugPrintf("[time] NOTE resume point +0x%x (scanned) != KB_TIME_UPDATE_BODY "
                  "+0x%x (header) -- using the scanned value\n",
                  (unsigned)body, (unsigned)KB_TIME_UPDATE_BODY);
    g_unity_update_body = (void (*)(void *, double))(ub + body);
    debugPrintf("[time] Update body resume = libunity+0x%x (entry+0x%x)\n",
                (unsigned)body, (unsigned)(body - KB_TIME_UPDATE_ENTRY));
  }
  g_get_time_manager  = (void *(*)(void))(ub + KB_TIME_GETMANAGER);            /* PvZ GetTimeManager() */
  /* Native vsync triple (round 56): stand in for Choreographer's per-frame
   * signal so Unity's frame-pacing wait (and the splash fade behind it) can
   * proceed. Offsets confirmed as the vsync waiter's mutex/cond/counter. */
  /* DERIVED FOR THIS GAME (was three hardcoded Fruit Ninja offsets, which is
   * what kept KB_HAVE_TIME_FIX gated). Read out of this binary's own
   * WaitVSync(long) @0x392c9c -- see nx_patch_killerbean.h. */
#if KB_HAVE_VSYNC_TRIPLE
  g_vsync_mutex   = (pthread_mutex_t **)(ub + KB_VSYNC_MUTEX);
  g_vsync_cond    = (pthread_cond_t  **)(ub + KB_VSYNC_COND);
  g_vsync_counter = (volatile uint64_t *)(ub + KB_VSYNC_COUNTER);
#endif
  uint32_t stub[4] = {
    0x58000050u,  /* ldr x16, #8 */
    0xd61f0200u,  /* br  x16     */
    (uint32_t)((uintptr_t)&nx_time_update_hook & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_time_update_hook >> 32),
  };
  /* VERIFY-FIRST. KB_TIME_UPDATE_ENTRY/WORD are derived for THIS game:
   * TimeManager::Update(double) @0x20b654, prologue `ldr x8,[x0,#0xc8]`
   * (0xF9406408). If the game is updated this stops matching and logs
   * instead of writing a 16-byte stub over an unrelated function. */
  if (*(volatile uint32_t *)(ub + KB_TIME_UPDATE_ENTRY) == KB_TIME_UPDATE_WORD) {
    so_patch_code((void *)(ub + KB_TIME_UPDATE_ENTRY), stub, sizeof stub);
  } else {
    debugPrintf("[time] SKIP Update hook: +0x%x = 0x%08x, not the expected "
                "prologue -- offsets are PvZ's, re-derive (PORTING sec 6)\n",
                (unsigned)KB_TIME_UPDATE_ENTRY,
                *(volatile uint32_t *)(ub + KB_TIME_UPDATE_ENTRY));
    return;
  }
  if (R_SUCCEEDED(threadCreate(&g_clock_thr, nx_clock_thread, NULL, NULL, 0x8000, 0x2C, -2)))
    threadStart(&g_clock_thr);
  debugPrintf("[boot] installed TimeManager::Update hook @libunity+KB_TIME_UPDATE_ENTRY "
              "+ clock thread (newTime <- startupRef + wallclock)\n");
}

/* --- boot finish-flag probe -------------------------------------------------
 * State 6 (SetFinishFlag) walks A=*(il2cpp+0x21fd0718); B=*A; obj=*(B+0x20);
 * obj2=*(*(obj+0xc0)+0x10); holder=*(obj2+0xb8 then deref); and writes
 * *(holder+0x10)=1 -- but ONLY if holder is non-null (else it stays at state 6
 * forever). CheckInitializeFinish polls the same obj2 (+0xe0 / +0x135). We read
 * the live chain so we can see exactly which link is null / whether the finish
 * flag ever gets set, without any destructive patching. */
static int nx_rd(uintptr_t a) {
  MemoryInfo mi; u32 pi;
  if (a < 0x1000) return 0;
  if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
  if (mi.type == MemType_Unmapped || !(mi.perm & Perm_R)) return 0;
  return 1;
}
static uintptr_t nx_pd(uintptr_t a) { return nx_rd(a) ? *(volatile uintptr_t *)a : 0; }
__attribute__((unused)) static void nx_probe_finish(uintptr_t base) {
  uintptr_t A    = nx_pd(base + KB_IL2CPP_FINISH_FLAG);
  uintptr_t B    = A    ? nx_pd(A)          : 0;
  uintptr_t obj  = B    ? nx_pd(B + 0x20)   : 0;
  uintptr_t c0   = obj  ? nx_pd(obj + 0xc0) : 0;
  uintptr_t obj2 = c0   ? nx_pd(c0 + 0x10)  : 0;
  uintptr_t b8   = obj2 ? nx_pd(obj2 + 0xb8): 0;
  uintptr_t hold = b8   ? nx_pd(b8)         : 0;
  int      flag  = (hold && nx_rd(hold + 0x10)) ? *(volatile uint8_t  *)(hold + 0x10) : -1;
  uint32_t e0    = (obj2 && nx_rd(obj2 + 0xe0)) ? *(volatile uint32_t *)(obj2 + 0xe0) : 0xffffffffu;
  int o135  = (obj  && nx_rd(obj + 0x135))  ? *(volatile uint8_t *)(obj + 0x135)  : -1;
  int o2135 = (obj2 && nx_rd(obj2 + 0x135)) ? *(volatile uint8_t *)(obj2 + 0x135) : -1;
  debugPrintf("[probe] A=%p B=%p obj=%p obj2=%p hold=%p | finishFlag=%d e0=0x%x obj.135=%d obj2.135=%d\n",
              (void *)A, (void *)B, (void *)obj, (void *)obj2, (void *)hold, flag, e0, o135, o2135);
}


/* libunity ~17M + libil2cpp ~36M + headroom for relocated segments */
#define SO_REGION_BYTES (160u * 1024 * 1024)

/* Reserve the virtual arena window at the TOP of the alias region (deep in the
 * 64GB region, where libnx never allocates) after verifying it is fully unmapped.
 * No physical backing yet -- pages are committed on demand. */
static void *overcommit_reserve_window(size_t size) {
  size = (size + MMAP_ARENA_ALIGN - 1) & ~(MMAP_ARENA_ALIGN - 1);
  if (!g_alias_base || g_alias_size < size + MMAP_ARENA_ALIGN) return NULL;
  u64 top = g_alias_base + g_alias_size;
  u64 win = (top - size) & ~(MMAP_ARENA_ALIGN - 1);
  u64 a = win;
  while (a < win + size) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return NULL;
    if (mi.type != MemType_Unmapped) return NULL;   /* collision -> bail */
    a = mi.addr + mi.size;
  }
  return (void *)win;
}

/* virtmemFindStack refuses large windows even when the stack region has room, so
 * scan the region directly via svcQueryMemory for the largest 256MB-aligned
 * unmapped hole (and log the whole map for diagnosis). svcMapMemory only aliases
 * into the stack region, so the OC window must live here. */
static void  *g_oc_win2    = NULL;   /* second-largest stack hole (OC window 2) */
static size_t g_oc_win2_sz = 0;
extern int oc_arena_add_window(void *window, size_t window_bytes);
static void *oc_find_stack_window(size_t want, size_t *out_size) {
  *out_size = 0;
  u64 sbase = 0, ssize = 0;
  svcGetInfo(&sbase, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&ssize, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
  if (!sbase || !ssize) return NULL;
  u64 end = sbase + ssize, a = sbase, best_a = 0, best_l = 0, sec_a = 0, sec_l = 0;
  int holes = 0, mapped = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 ms = mi.addr, me = mi.addr + mi.size;
    if (me <= a) break;                              /* no-progress guard */
    if (mi.type == MemType_Unmapped) {
      u64 hs = ms < sbase ? sbase : ms, he = me > end ? end : me;
      if (he > hs) {
        if (he - hs > best_l) { sec_l = best_l; sec_a = best_a; best_l = he - hs; best_a = hs; }
        else if (he - hs > sec_l) { sec_l = he - hs; sec_a = hs; }
        if (holes < 8)
          debugPrintf("[oc] stack hole %d: %p .. %p (%u MB)\n",
                      holes++, (void *)hs, (void *)he, (unsigned)((he - hs) >> 20));
      }
    } else mapped++;
    a = me;
  }
  debugPrintf("[oc] stack scan: base=%p size=%u MB, %d holes, %d mapped spans, largest=%u MB\n",
              (void *)sbase, (unsigned)(ssize >> 20), holes, mapped, (unsigned)(best_l >> 20));
  if (!best_a) return NULL;
  if (sec_a) {   /* stash the runner-up for OC window 2 */
    u64 a2 = (sec_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
    if (a2 < sec_a + sec_l) {
      u64 v2 = ((sec_a + sec_l) - a2) & ~(MMAP_ARENA_ALIGN - 1);
      if (v2 > want) v2 = want;
      if (v2) { g_oc_win2 = (void *)a2; g_oc_win2_sz = v2; }
    }
  }
  u64 aligned = (best_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
  if (aligned >= best_a + best_l) return NULL;
  u64 avail = ((best_a + best_l) - aligned) & ~(MMAP_ARENA_ALIGN - 1);
  if (!avail) return NULL;
  if (avail > want) avail = want;
  *out_size = avail;
  return (void *)aligned;
}

/* Try to set up alias-region overcommit, recording each step's outcome into the
 * g_oc_* globals (logged from main). Alias-region overcommit turned out to be
 * impossible on this process: svcMapPhysicalMemory requires a non-zero kernel
 * "system resource" pool (for page-table/block bookkeeping) and our title-override
 * process has none -> it returns InvalidState (0xfa01). The unsafe pool
 * (svcMapPhysicalMemoryUnsafe) is ~44MB and already consumed. So we just record the
 * diagnostics and stay on the fully heap-backed arena. (Confirmed via Atmosphere
 * kern_svc_physical_memory.cpp: `R_UNLESS(GetTotalSystemResourceSize() > 0,
 * ResultInvalidState())`.) */
static int overcommit_setup(void *addr, size_t size, size_t so_zone,
                            void **out_addr, size_t *out_fake) {
  (void)addr; (void)size; (void)so_zone; (void)out_addr; (void)out_fake;
  g_oc_hint_map   = envIsSyscallHinted(0x2c);
  g_oc_hint_unmap = envIsSyscallHinted(0x2d);
  svcGetInfo(&g_alias_base, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&g_alias_size, InfoType_AliasRegionSize,    CUR_PROCESS_HANDLE, 0);
  g_oc_alias_mb = (unsigned)(g_alias_size >> 20);
  svcGetInfo(&g_oc_sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
  return 0;   /* no system resource -> svcMapPhysicalMemory unusable; heap-backed */
}

/* Reserve a slice of address space for the .so images; the rest is the newlib
 * heap the engine mallocs from. (Verbatim from cr3_nx.) */
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t MB = 1024 * 1024;
  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  /* Preferred path: alias-region overcommit. Secures the virtual window and
   * test-commits a page FIRST, then shrinks the heap to [newlib + so_zone] so the
   * freed physical (~2.5GB) is available for on-demand commits. Everything is
   * secured before the shrink so a failure can't strand us with a shrunk heap. */
  void *oc_addr; size_t oc_fake;
  if (overcommit_setup(addr, size, so_zone, &oc_addr, &oc_fake)) {
    fake_heap_start = (char *)oc_addr;
    fake_heap_end   = (char *)oc_addr + oc_fake;
    heap_so_base    = (void *)ALIGN_MEM((uintptr_t)oc_addr + oc_fake, 0x1000);
    heap_so_limit   = so_zone;
    return;
  }

  /* Fallback: fully heap-backed 256MB-aligned arena (no overcommit). */
  const size_t big_align    = MMAP_ARENA_ALIGN;
  const size_t newlib_floor = 448 * MB;   /* malloc + il2cpp managed/GC heap */
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + newlib_floor + 256 * MB) {
    size_t avail = size - so_zone - big_align - newlib_floor;
    if (arena_sz > avail) arena_sz = avail & ~(big_align - 1);   /* clamp to RAM */
    fake_heap_size = size - so_zone - arena_sz - big_align;       /* newlib gets the rest */
  } else {
    /* heap too small for a dedicated arena (e.g. applet mode): skip it; the mmap
     * allocator falls back to a memalign-backed bitmap arena. */
    fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;
    arena_sz = 0;
  }

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

void nx_join_split_assets(const char *dir);   /* nx_splitjoin.c */
void nx_gpu_probe(void);                      /* nx_gpu_probe.c */

static void nx_tree_stats(const char *path, unsigned *nfiles, uint64_t *nbytes) {
  DIR *d = opendir(path);
  if (!d) return;
  struct dirent *e; char child[1024];
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    snprintf(child, sizeof child, "%s/%s", path, e->d_name);
    struct stat st;
    if (stat(child, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) nx_tree_stats(child, nfiles, nbytes);
    else { if (nfiles) (*nfiles)++; if (nbytes) *nbytes += (uint64_t)st.st_size; }
  }
  closedir(d);
}

/* Unity and IL2CPP WRITE into these at runtime (extracted metadata,
 * il2cpp/unity.ver). The pack is read-only and nx_rmtree() takes the whole
 * loose tree with it, so recreate the empty directories for those writes to
 * land in -- otherwise open(".../il2cpp/unity.ver") fails, Unity decides the
 * asset pack contains no Unity data, and raises its fatal dialog.
 * Ported from the Deus Ex GO loader (create_asset_skeleton). */
/* Dump a directory tree (one level deep) with sizes. The il2cpp extraction
 * has been failing invisibly; this makes the on-disk result part of the log. */
static void nx_dump_dir(const char *path, const char *tag) {
  DIR *d = opendir(path);
  if (!d) { debugPrintf("[dump] %s %s: (absent)\n", tag, path); return; }
  debugPrintf("[dump] %s %s:\n", tag, path);
  struct dirent *e; int n = 0;
  while ((e = readdir(d)) != NULL && n < 32) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char child[512];
    snprintf(child, sizeof child, "%s/%s", path, e->d_name);
    struct stat st;
    if (stat(child, &st) != 0) { debugPrintf("[dump]    %-40s (stat failed)\n", e->d_name); }
    else if (S_ISDIR(st.st_mode)) { debugPrintf("[dump]    %-40s <dir>\n", e->d_name); }
    else { debugPrintf("[dump]    %-40s %lld bytes%s\n", e->d_name,
                       (long long)st.st_size,
                       st.st_size == 0 ? "   *** EMPTY ***" : ""); }
    n++;
  }
  closedir(d);
  if (!n) debugPrintf("[dump]    (empty)\n");
}

static void nx_create_asset_skeleton(void) {
  /* Suffixes, not absolute paths: the root is a runtime value now, so a static
   * initializer cannot concatenate it. Joined with nx_path() in the loop. */
  static const char *dirs[] = {
    "/assets",
    "/assets/bin",
    "/assets/bin/Data",
    "/assets/bin/Data/Managed",
    "/assets/bin/Data/Managed/Metadata",
    "/assets/bin/Data/Managed/Resources",
    "/assets/bin/Data/Resources",
    /* NOT il2cpp/*: Unity stages into il2cpp_tmp/ and renames into place,
     * and a rename onto an existing non-empty directory fails. Let Unity
     * create those itself (mkdir_fake is real and now logs). */
  };
  for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); i++)
    mkdir(nx_path(dirs[i]), 0777);   /* EEXIST is fine */
  debugPrintf("[pack] recreated writable dir skeleton (%u dirs)\n",
              (unsigned)(sizeof(dirs) / sizeof(*dirs)));
}

static void nx_rmtree(const char *path) {
  DIR *d = opendir(path);
  if (!d) { unlink(path); return; }
  struct dirent *e;
  char child[1024];
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    snprintf(child, sizeof child, "%s/%s", path, e->d_name);
    struct stat st;
    if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) nx_rmtree(child);
    else unlink(child);
  }
  closedir(d);
  rmdir(path);
}

/* mounted != 0 -> a pack is live; required files live inside it and the loose
 * tree has been deleted, so consult the pack before the on-disk stat(). */
static void check_data(int mounted) {
  /* SINGLE-ARCHIVE layout -- data.unity3d is the entry point.
   * Killer Bean Unleashed ships one 54 MB data.unity3d plus loose .resource
   * streams; it has NO globalgamemanagers, which is what the inherited Fruit
   * Ninja check looked for. Getting this wrong does not fail gracefully: the
   * loader reports "assets missing" and refuses to boot even though the data
   * is present and correct. (Note this is the layout the PvZ Fusion core
   * originally assumed, so we are back on that path.) */
  const char *files[] = {
    LIB_MAIN, LIB_UNITY, LIB_IL2CPP,
    "assets/bin/Data/data.unity3d",
    "assets/bin/Data/Managed/Metadata/global-metadata.dat",
  };
  char path[768];
  struct stat st;
  for (unsigned i = 0; i < sizeof(files)/sizeof(*files); i++) {
    /* The three libs are never packed; the two asset paths may be. When a
     * pack is mounted, a hit there is authoritative and the loose file is
     * expected to be gone. */
#if KB_ASSET_PACK
    if (mounted && !strncmp(files[i], "assets/", 7) &&
        asset_pack_stat_path(files[i], NULL, NULL))
      continue;
#endif
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, files[i]);
    if (stat(path, &st) < 0)
      /* Print the full path, not just the relative name: when the folder name
       * itself is wrong, every entry appears "missing" and the bare name gives
       * no hint as to why. */
      fatal_error("Missing data file:\n%s\n\nLooked in: %s\n\n"
                  "If the file IS there, check the folder name -- this build\n"
                  "expects exactly: %s", files[i], path, DATA_ROOT);
  }

  /* Unjoined .splitN parts.
   *
   * NOT APPLICABLE TO THIS GAME, kept as a generic guard. Some Unity Android
   * builds chop large assets into 1 MiB chunks that the engine reassembles
   * through a JAVA class ("AndroidSplitFile" sits in libunity.so beside
   * "Unable to resolve method", i.e. it is a JNI FindClass target, not native
   * code). With no Java runtime nothing reassembles them, so the loader has
   * to. Killer Bean Unleashed ships ZERO .split parts -- its asset set was
   * surveyed and contains none -- so this list is empty and the loop below
   * does nothing. It is left in place, rather than deleted, so a future game
   * update that introduces splits is caught instead of silently booting with
   * truncated assets. */
  static const char *splitbases[] = { NULL };
  for (unsigned i = 0; i < sizeof(splitbases)/sizeof(*splitbases); i++) {
    char sp[800];
    if (!splitbases[i]) continue;   /* empty list for this game -- see above */
#if KB_ASSET_PACK
    /* a base that made it into the pack was joined before packing */
    if (mounted && asset_pack_stat_path(splitbases[i], NULL, NULL)) continue;
#endif
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, splitbases[i]);
    if (stat(path, &st) == 0) continue;              /* already joined */
    snprintf(sp, sizeof sp, "%s/%s.split0", DATA_ROOT, splitbases[i]);
    if (stat(sp, &st) == 0)
      fatal_error("Could not join split asset:\n%s\n\n"
                  "The loader tried to reassemble the .split0/.split1/...\n"
                  "parts automatically and failed. Check debug.log for a\n"
                  "[join] line. Usual causes: SD card full, or the parts\n"
                  "were copied incompletely.",
                  splitbases[i]);
  }
}

/* load a module, advance the .so arena, resolve its imports against the table */
static int load_module(so_module *mod, const char *name) {
  char path[768];
  snprintf(path, sizeof path, "%s/%s", DATA_ROOT, name);
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  debugPrintf("[mod] %-14s virtbase=%p size=0x%zx  (resolve: addr - virtbase = vaddr)\n",
              name, mod->load_virtbase, mod->load_size);
  crx_resolve_imports(mod);   /* so_resolve(mod, dynlib_functions, ...) */
  /* NOTE: so_patch_stack_canaries() intentionally NOT called. Per-thread bionic
   * TLS (install_bionic_tls) makes the engine's stack-protector guard consistent,
   * so the canary checks pass on their own. NOPing 2000+ b.ne sites risked a
   * false-positive in non-canary code (e.g. allocator list logic) -> corruption. */
  return 0;
}

/* engine entry points (unity_entrypoints.h), resolved post-finalize */
static fn_initJni  Unity_initJni;
static fn_gfxstate Unity_nativeRecreateGfxState;
static fn_v        Unity_nativeSendSurfaceChanged;
static fn_z        Unity_nativeRender;
static fn_inject   Unity_nativeInjectEvent;
static fn_v        Unity_nativeResume;
static fn_vz       Unity_nativeFocusChanged;
static fn_z        Unity_nativeDone;
static fn_v        Unity_nativeApplicationUnload;

/* ---------------------------------------------------------------------------
 * In-memory libunity patch (ported from VLN's nx_patch_unity_regions): the SD
 * card now ships the STOCK libunity.so and the boot patches it after load,
 * instead of distributing a pre-modified binary. 23 instruction words, from a
 * byte-exact diff of the known-good patched .so vs stock (Unity 2022.3.62f2):
 * 21 sites relax the allocator's memory-region granularity 256MB->64MB so the
 * engine fits the so_loader address space on a 4GB Switch, plus two branch
 * forces (0x5d24cc cond->uncond, 0x5d4e98 ldr->skip) from the same known-good
 * build. Verify-first like VLN: every original word must match before anything
 * is written; a fully pre-patched .so is detected and accepted; any other
 * mismatch leaves the binary untouched (different Unity build) with a loud log.
 * ------------------------------------------------------------------------- */
static int nx_patch_libunity(uintptr_t ub) {
  /* PvZ Fusion 3.6.1 (Unity 2022.3.62f1c1): tables live in nx_patch_killerbean.h.
   * VERIFY-FIRST like the original -- every {from} word must match before ANY
   * write; a fully pre-patched .so is accepted; any mismatch patches nothing. */
  const NxPatchWord *P = KB_PATCH_WORDS;
  const int N = KB_PATCH_WORDS_N;
  int stock = 0, patched = 0;
  for (int i = 0; i < N; i++) {
    uint32_t cur = *(volatile uint32_t *)(ub + P[i].off);
    if (cur == P[i].from) stock++;
    else if (cur == P[i].to) patched++;
    else {
      debugPrintf("[patch] libunity word mismatch @+0x%x: have 0x%08x want 0x%08x -> SKIP all (offset table may be off for this build)\n",
                  (unsigned)P[i].off, cur, P[i].from);
      return 0;
    }
  }
  if (patched == N) {
    debugPrintf("[patch] libunity already pre-patched (%d sites) -- ok\n", N);
  } else if (stock != N) {
    debugPrintf("[patch] libunity PARTIALLY patched (%d/%d) -> SKIP (won't mix builds)\n", patched, N);
    return 0;
  } else {
    for (int i = 0; i < N; i++)
      so_patch_code((void *)(ub + P[i].off), &P[i].to, sizeof P[i].to);
    debugPrintf("[patch] libunity region granularity 256MB->64MB patched (%d sites)\n", N);
  }
  /* Optional branch forces (only when you have located them; see nx_patch_killerbean.h). */
  if (KB_BRANCH_FORCES_N > 0) {
    const NxPatchWord *B = KB_BRANCH_FORCES;
    for (int i = 0; i < KB_BRANCH_FORCES_N; i++) {
      uint32_t cur = *(volatile uint32_t *)(ub + B[i].off);
      if (cur == B[i].from)
        so_patch_code((void *)(ub + B[i].off), &B[i].to, sizeof B[i].to);
      else if (cur != B[i].to)
        debugPrintf("[patch] branch-force @+0x%x mismatch (have 0x%08x) -> skip\n",
                    (unsigned)B[i].off, cur);
    }
    debugPrintf("[patch] libunity branch forces applied (%d sites)\n", KB_BRANCH_FORCES_N);
  }
  return 1;
}

/* ---------------------------------------------------------------------------
 * PvZ 62f1c1 first-boot il2cpp hacks. With the GC stop-the-world bridge fixed for
 * PvZ (libc_shim.c pthread_kill_gc now reads the correct suspend/restart/ack
 * globals), the Boehm GC works, so -- exactly like the VLN port -- we do NOT try to
 * disable it. An earlier attempt to il2cpp_gc_disable() mid-il2cpp_init deadlocked
 * on a GC lock held by a not-yet-running helper thread (verified on hardware: hung
 * right after set_mode, UnityMain parked in a futex lock). The GC now runs normally
 * and the bridge keeps its POSIX-signal stop-the-world from hanging. All this does
 * is redirect Time.get_* to our frame clock. Called once at boot, before the loop.
 * ------------------------------------------------------------------------- */
/* Is this address mapped and readable? A patch must never fault merely by
 * CHECKING its site -- round 120 killed boot doing exactly that, reading
 * il2cpp+0x159dfcc before the module's text was mapped. Verifying costs one
 * syscall and turns an unmapped site into a skipped patch. */
static int nx_addr_readable(uintptr_t a) {
  MemoryInfo mi; u32 pi;
  if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
  if (mi.type == MemType_Unmapped) return 0;
  if (!(mi.perm & Perm_R)) return 0;
  return 1;
}

#if KB_HAVE_LIVENESS_GUARD
/* ---- liveness guard veneer (round 129) -----------------------------------
 * Entered in place of il2cpp's add_process_object(obj, state). Derivation and
 * rationale: nx_patch_killerbean.h. Register facts this depends on, all read
 * off the disassembly rather than assumed:
 *
 *   x0 = obj, x1 = state (LivenessState*), state->filter at +8
 *   obj->klass at +0, low bit = LivenessState's mark bit
 *   klass->typeHierarchy at +0xc8, ->typeHierarchyDepth at +0x130,
 *   ->flags halfword at +0x135, has_references = bit 5
 *
 * x16/x17 are IP0/IP1 and x8 is caller-saved, so all three are free at a
 * function entry; x0/x1/x19/x20/x21/x30 are untouched on the path that falls
 * through to the original body. Both early exits return w0 = 0, which is what
 * the two original early exits (null object, already-marked) also return. */
/* HIDDEN visibility is load-bearing, not tidiness. The veneer addresses this
 * with adrp/:lo12:, and we build -fPIE: a default-visibility symbol is formally
 * preemptible, which is the case where GNU ld rejects R_AARCH64_ADR_PREL_PG_HI21
 * ("may be overridden"). Hidden makes it non-preemptible, so the pair is always
 * a valid link-time resolution. */
__attribute__((visibility("hidden"))) uint64_t g_fn_liveness_body = 0;
extern void fn_liveness_guard(void);

__asm__(
  ".text\n"
  ".balign 4\n"
  ".global fn_liveness_guard\n"
  ".type   fn_liveness_guard, %function\n"
  "fn_liveness_guard:\n"
  "    cbz   x0, .Lfnlg_zero\n"                  /* !object                   */
  "    ldr   x16, [x0]\n"                        /* obj->klass, tagged        */
  "    tbnz  w16, #0, .Lfnlg_zero\n"             /* IS_MARKED                 */
  "    ands  x16, x16, #0xfffffffffffffffe\n"    /* GET_CLASS                 */
  "    b.eq  .Lfnlg_zero\n"                      /* klass == NULL  (r119)     */
  "    add   x17, x16, #0x135\n"                 /* flags: offset is odd, so  */
  "    ldrh  w17, [x17]\n"                       /*  ldrh cannot fold it      */
  "    tbnz  w17, #5, .Lfnlg_orig\n"             /* has_references: unchanged */
  "    ldr   x17, [x1, #8]\n"                    /* state->filter             */
  "    cbz   x17, .Lfnlg_orig\n"                 /* no filter: unchanged      */
  "    ldr   x16, [x16, #0xc8]\n"                /* klass->typeHierarchy      */
  "    cbz   x16, .Lfnlg_zero\n"                 /* NEW: never set up -> false */
  ".Lfnlg_orig:\n"
  "    stp   x30, x21, [sp, #-0x20]!\n"          /* the four clobbered        */
  "    stp   x20, x19, [sp, #0x10]\n"            /*  prologue instructions,   */
  "    ldr   x8,  [x0]\n"                        /*  minus the cbz we did     */
  "    adrp  x16, g_fn_liveness_body\n"
  "    add   x16, x16, :lo12:g_fn_liveness_body\n"
  "    ldr   x16, [x16]\n"
  "    br    x16\n"                              /* -> add_process_object+0x10 */
  ".Lfnlg_zero:\n"
  "    mov   w0, wzr\n"
  "    ret\n"
  ".size fn_liveness_guard, .-fn_liveness_guard\n"
);
#endif

/* REVERTED (round 123). A per-frame attempt to switch off
 * RemoteConfigObject.ApplyRemoteConfigObjects via il2cpp_class_from_name() and
 * friends crashed on the FIRST loop iteration: zero frames rendered, no log line
 * from the function itself, fault at libil2cpp+0x1578a68 reading klass+0x135 --
 * i.e. inside il2cpp, walking metadata that is not ready until the engine's
 * first nativeRender has run.
 *
 * I called that change "fail-safe by construction" because every return value
 * was checked. That was wrong: checking what a function RETURNS does nothing
 * when the function faults internally. Round 120 was the same mistake one level
 * out (the check itself was a memory access); here the CALL itself is the unsafe
 * act, and no amount of validating its result helps.
 *
 * The proven technique in this port is the opposite: patch the faulting
 * instruction, having verified its bytes first (round 119). If the
 * IDataSource.ConvertFromTo crash needs addressing, that is the way to do it --
 * not by driving the il2cpp API from outside the engine's own lifecycle. */

#if KB_HOOK_CLOSE_WHATSNEW
/* ---------------------------------------------------------------------------
 * LevelMap_WhatsNew.CloseWhatsNew -- FULL REPLACEMENT of the managed method.
 *
 * Deliberately not a trampoline. A trampoline would have to re-execute the four
 * displaced prologue instructions, and the third of them is
 * `adrp x20, #0x2ee5000` -- PC-relative, so a verbatim copy in a scratch buffer
 * computes the wrong page and the following `ldrb w8,[x20,#0x73a]` reads
 * garbage. Rebuilding it as an absolute movz/movk sequence is doable but is
 * hand-written self-modifying code for what is, here, a two-line job: the only
 * effect of this method that matters is hiding the panel.
 *
 * What is NOT reproduced, and the consequence: the original also runs two
 * il2cpp class-init calls behind a once-flag, and afterwards stores the current
 * version into a static so the popup does not reappear. Skipping that means the
 * popup WILL show again on the next launch. That is a deliberate trade -- being
 * able to dismiss it beats being blocked by it -- and it is why this sits
 * behind KB_HOOK_CLOSE_WHATSNEW rather than being unconditional.
 *
 * Diagnostically this is decisive either way:
 *   "ENTER" appears  -> the click DOES reach the handler; the fault was inside
 *                       it, and the panel pointer logged beside it says whether
 *                       the serialized reference was the problem.
 *   nothing appears  -> the click never reaches the handler at all, so the
 *                       fault is raycast/blocking or an unwired onClick, i.e.
 *                       scene state rather than anything the wrapper does.
 * ------------------------------------------------------------------------- */
static unsigned g_cwn_hits = 0;

static void kb_close_panel(void *self, unsigned panel_off, const char *what) {
  const uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
  void *panel = self ? *(void **)((char *)self + panel_off) : NULL;

  g_cwn_hits++;
  debugPrintf("[ui] %s ENTER #%u self=%p panel=%p\n",
              what, g_cwn_hits, self, panel);

  if (!self) {
    debugPrintf("[ui] %s: null `this` -- not dispatched as an instance call; "
                "leaving the popup alone\n", what);
    return;
  }
  if (!panel) {
    debugPrintf("[ui] %s: panel field is NULL. The serialized reference is "
                "missing, so even the stock method would have stopped here "
                "(it does `cbz` past the SetActive). Nothing to hide.\n", what);
    return;
  }
  /* Same call the stock body makes: SetActive(panel, false, NULL). The NULL
   * MethodInfo matches the original's `mov x2, xzr`. */
  typedef void (*il2_setactive_t)(void *, int32_t, void *);
  ((il2_setactive_t)(b + KB_IL2_GameObject_SetActive))(panel, 0, NULL);
  debugPrintf("[ui] %s: SetActive(false) done -- panel hidden (version flag "
              "NOT saved, so it returns next launch)\n", what);
}

/* ---- Awake / Start trampolines -------------------------------------------
 * These log and then run the STOCK body, unlike the close hooks which replace
 * it. Their side effects matter: Awake assigns dungeon_refresh, Start decides
 * whether the popup appears at all.
 *
 * Written as a hand-built thunk because the two ADRPs in the displaced prologue
 * are PC-relative -- copying them verbatim into a buffer computes the wrong
 * page and every field access afterwards reads garbage. Instead the thunk
 * rebuilds them from values resolved at install time.
 *
 * x30 discipline is the part worth reading twice: the entry stub arrives via
 * `br`, not `bl`, so x30 still holds the engine's return address. The `bl` to
 * the logger would destroy it, so it is saved and restored around the call --
 * BEFORE the displaced `stp x19,x30,[sp,#0x10]` runs, which is what the stock
 * body will later pop. */
/* NOT static, and neither are the two log helpers below. The thunks are
 * top-level __asm__, which is assembled independently of C scoping: it resolves
 * `kb_wn_awake_log` and `g_wn_awake_x20` as ordinary link-time symbols. Marking
 * them static gives them internal linkage, emits no global symbol, and the link
 * fails with "undefined reference" from inside the thunk -- which is exactly
 * what happened on the first build. External linkage is required here, not a
 * style choice. */
/* Retained only so the Start replacement can note the instance in the log; the
 * heartbeat sweep that used to consume it is gone (see the note in the frame
 * loop). Nothing dereferences this outside the engine's own call now, which is
 * deliberate: it is a raw pointer to a MonoBehaviour that the engine may destroy
 * on scene change. */
void *g_wn_self = 0;
uint64_t g_wn_awake_x20, g_wn_awake_x21, g_wn_awake_resume;
uint64_t g_wn_start_x20, g_wn_start_x21, g_wn_start_resume;

/* activeSelf / activeInHierarchy for one panel. The distinction is the whole
 * point: a panel can be activeSelf=true under a deactivated parent and never
 * reach the screen, so only activeInHierarchy tells us what is actually drawn
 * and raycast against. */
__attribute__((unused)) static void kb_wn_panel(const char *nm, void *go) {
  const uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
  if (!go) { debugPrintf("      %-10s NULL\n", nm); return; }
  typedef uint8_t (*il2_getb_t)(void *, void *);
  const uint8_t self_a =
      ((il2_getb_t)(b + KB_IL2_GO_get_activeSelf))(go, NULL);
  const uint8_t hier_a =
      ((il2_getb_t)(b + KB_IL2_GO_get_activeInHierarchy))(go, NULL);
  debugPrintf("      %-10s %p  activeSelf=%d activeInHierarchy=%d%s\n",
              nm, go, self_a, hier_a,
              hier_a ? "   <-- ON SCREEN" : "");
}

__attribute__((unused)) static void kb_wn_log(void *self, const char *what) {
  if (!self) { debugPrintf("[ui] LevelMap_WhatsNew.%s: null this\n", what); return; }
  void *wn = *(void **)((char *)self + KB_WHATSNEW_PANEL_OFF);
  void *nd = *(void **)((char *)self + KB_NEWDAY_PANEL_OFF);
  void *sb = *(void **)((char *)self + KB_SPECIALBONUS_PANEL_OFF);
  debugPrintf("[ui] LevelMap_WhatsNew.%s self=%p dungeon_refresh=%p\n",
              what, self, *(void **)((char *)self + KB_WHATSNEW_REFRESH_OFF));
  /* Any sibling reporting activeInHierarchy=1 alongside the What's New panel is
   * a full-screen candidate sitting above the X in the raycast order -- which
   * would absorb every tap and is the leading explanation for why this is the
   * only dead button in the game. */
  kb_wn_panel("whatsnew", wn);
  kb_wn_panel("newday",   nd);
  kb_wn_panel("special",  sb);

#if KB_HIDE_BLOCKING_PANELS
  /* Act on the finding in the same pass, rather than costing another build to
   * confirm it. A sibling only gets hidden when it AND panel_whatsnew are both
   * live -- that is precisely the blocking arrangement, and it cannot fire when
   * the game is legitimately showing a sibling on its own. */
  { const uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
    typedef uint8_t (*il2_getb_t)(void *, void *);
    typedef void    (*il2_setactive_t)(void *, int32_t, void *);
    const il2_getb_t live = (il2_getb_t)(b + KB_IL2_GO_get_activeInHierarchy);
    if (wn && live(wn, NULL)) {
      struct { void *go; const char *nm; } sib[] = { { nd, "newday" }, { sb, "special" } };
      for (unsigned i = 0; i < 2; i++) {
        if (!sib[i].go || !live(sib[i].go, NULL)) continue;
        ((il2_setactive_t)(b + KB_IL2_GameObject_SetActive))(sib[i].go, 0, NULL);
        debugPrintf("[ui] %s was live ALONGSIDE the What's New panel -- hidden. "
                    "If the close button works now, that panel was covering it.\n",
                    sib[i].nm);
      }
    }
  }
#endif
}
__attribute__((used)) void kb_wn_awake_log(void *self) { kb_wn_log(self, "Awake"); }   /* see linkage note above */
__attribute__((used)) void kb_wn_start_log(void *self) {   /* see linkage note above */
  /* Keep `this` so the popup can be sampled WHILE it is on screen. The probe at
   * this call site fires at method ENTRY, before the body runs, and Start() does
   * its SetActive in a tail call at +0x74 -- so entry-time state showed all
   * three panels inactive even though the popup appears moments later. Sampling
   * once, early, answered the wrong question. */
  g_wn_self = self;
  /* dungeon_refresh is assigned at Awake +0x6c. Non-null here means Awake ran
   * to completion; null means it took one of its two early `cbz` exits and the
   * component was never fully configured. */
  kb_wn_log(self, "Start");
}

__asm__(
".text\n"
".balign 4\n"
".globl kb_wn_awake_thunk\n"
".hidden kb_wn_awake_thunk\n"
"kb_wn_awake_thunk:\n"
"   stp x29, x30, [sp, #-32]!\n"
"   stp x0, x1, [sp, #16]\n"
"   bl  kb_wn_awake_log\n"
"   ldp x0, x1, [sp, #16]\n"
"   ldp x29, x30, [sp], #32\n"
"   stp x21, x20, [sp, #-0x20]!\n"      /* displaced +0x0 */
"   stp x19, x30, [sp, #0x10]\n"        /* displaced +0x4 */
"   adrp x20, g_wn_awake_x20\n"
"   ldr  x20, [x20, #:lo12:g_wn_awake_x20]\n"
"   adrp x21, g_wn_awake_x21\n"
"   ldr  x21, [x21, #:lo12:g_wn_awake_x21]\n"
"   adrp x16, g_wn_awake_resume\n"
"   ldr  x16, [x16, #:lo12:g_wn_awake_resume]\n"
"   br   x16\n"
".balign 4\n"
".globl kb_wn_start_thunk\n"
".hidden kb_wn_start_thunk\n"
"kb_wn_start_thunk:\n"
"   stp x29, x30, [sp, #-32]!\n"
"   stp x0, x1, [sp, #16]\n"
"   bl  kb_wn_start_log\n"
"   ldp x0, x1, [sp, #16]\n"
"   ldp x29, x30, [sp], #32\n"
"   stp x21, x20, [sp, #-0x20]!\n"
"   stp x19, x30, [sp, #0x10]\n"
"   adrp x20, g_wn_start_x20\n"
"   ldr  x20, [x20, #:lo12:g_wn_start_x20]\n"
"   adrp x21, g_wn_start_x21\n"
"   ldr  x21, [x21, #:lo12:g_wn_start_x21]\n"
"   adrp x16, g_wn_start_resume\n"
"   ldr  x16, [x16, #:lo12:g_wn_start_resume]\n"
"   br   x16\n"
);
extern void kb_wn_awake_thunk(void);
extern void kb_wn_start_thunk(void);

#if KB_SKIP_WHATSNEW
/* Replacement for LevelMap_WhatsNew.Start(). Safe to replace rather than
 * trampoline because the stock body's only observable effect is the SetActive
 * call at +0x74; the code before it is a once-flag and two il2cpp class-init
 * calls, neither of which anything else reads. Here the active argument is
 * forced to false instead of being derived from the version comparison. */
#if KB_LEVELMAP_BUTTON_PROBE
/* ---- round 155 DIAGNOSTIC: do the map's uGUI clicks arrive at all? --------
 *
 * The Lean nodes select now, so LeanTouch is fed. Button_play and the menu
 * button beside it are NOT Lean -- they are ordinary uGUI Buttons, and their
 * handlers are these two. uGUI dispatch is known to work on the store screen
 * (a previous log caught StandaloneInputModule.ProcessTouchPress invoking
 * WeaponStore_IAP.Purchase_Unlimited_Ammo), so the question is whether it also
 * works on the map.
 *
 * These are REPLACEMENTS, not trampolines: the original body does not run. That
 * costs nothing today -- both buttons are already dead -- and it answers the
 * only question that matters, with no ambiguity:
 *
 *   line appears when you tap Play  -> the click DOES arrive; uGUI is fine on
 *                                      this screen and the fault is inside
 *                                      PlayLevel/Menu (managed state, most
 *                                      likely selected_level never set by the
 *                                      Lean path). Turn this off; hook nothing.
 *   no line at all                  -> the click never reaches the handler, so
 *                                      the EventSystem raycast is not finding
 *                                      these Buttons on this screen. That is a
 *                                      uGUI-side problem, separate from Lean,
 *                                      and it also explains why the What's New
 *                                      X never worked.
 *
 * Turn KB_LEVELMAP_BUTTON_PROBE off once it has answered. */
static void kb_levelmap_playlevel(void *self) {
  debugPrintf("[ui] LevelMap_Control.PlayLevel() REACHED (self=%p) -- the uGUI "
              "click arrived; original body suppressed by the probe\n", self);
}
static void kb_levelmap_menu(void *self) {
  debugPrintf("[ui] LevelMap_Control.Menu() REACHED (self=%p) -- the uGUI click "
              "arrived; original body suppressed by the probe\n", self);
}
#endif

#if KB_LEAN_INPUT_FROM_LOADER
/* ---- LeanInput served from our own pointer state (round 154) --------------
 * See nx_patch_killerbean.h for the derivation. These two replacements are the
 * whole bridge: LeanTouch polls GetTouchCount()/GetTouch() every frame, and
 * everything downstream -- fingers, taps, LeanSelectable.IsSelected, and hence
 * every level node on the map -- follows from what they answer.
 *
 * Vector2 is a 2-float struct returned through a pointer out-param here, so the
 * ABI is plain: w0=index, x1=&id, x2=&position, x3=&pressure, x4=&set. */
extern int nx_touch_count(void);
extern int nx_touch_get(int i, int *id, float *x, float *y, int *set);

static int kb_lean_get_touch_count(void) {
  /* Round 158: the ONLY safe place to bring the new input system up. We are
   * inside a managed Update() here -- LeanTouch called us -- so the runtime is
   * initialised, this thread is attached, and Unity.InputSystem is loaded.
   * Doing it from the render loop at frame 0 faulted in Class::Init(NULL). */
  { extern void nx_newinput_managed_tick(void); nx_newinput_managed_tick(); }
  const int n = nx_touch_count();
  static int last = -1;
  if (n != last) { last = n;
    static unsigned m = 0;
    if (m++ < 40) debugPrintf("[lean] GetTouchCount -> %d\n", n); }
  return n;
}

typedef struct { float x, y; } KbVec2;
static void kb_lean_get_touch(int index, int *id, KbVec2 *position,
                              float *pressure, uint8_t *set) {
  int i = 0, s = 0; float x = 0.0f, y = 0.0f;
  const int ok = nx_touch_get(index, &i, &x, &y, &s);
  if (id)       *id = ok ? i : 0;
  if (position) { position->x = x; position->y = y; }
  if (pressure) *pressure = 1.0f;
  if (set)      *set = (uint8_t)(ok ? s : 0);
  { static unsigned m = 0;
    if (m++ < 40)
      debugPrintf("[lean] GetTouch(%d) -> id=%d (%.0f,%.0f) set=%d\n",
                  index, i, (double)x, (double)y, s); }
}
#endif

#if KB_LEAN_POINTOVERGUI_FALSE
/* Replacement for LeanTouch.PointOverGui(Vector2). A Vector2 is an HFA of two
 * floats, so it arrives in s0/s1 -- taking it as (float,float) matches the ABI
 * exactly. Returns bool in w0. See the installer below for why. */
static uint8_t kb_lean_point_over_gui(float x, float y) {
  static unsigned n = 0;
  if (n++ < 20)
    debugPrintf("[lean] PointOverGui(%.0f,%.0f) -> forced false\n",
                (double)x, (double)y);
  return 0;
}
#endif

static void kb_wn_start_skip(void *self) {
  const uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
  g_wn_self = self;                      /* recorded for the log only */
  if (!self) return;
  void *panel = *(void **)((char *)self + KB_WHATSNEW_PANEL_OFF);
  debugPrintf("[ui] Start (skip mode): forcing panel_whatsnew off, panel=%p\n",
              panel);
  if (panel)
    ((void (*)(void *, int32_t, void *))(b + KB_IL2_GameObject_SetActive))(panel, 0, NULL);
}
#endif

/* One thin entry per method: the hook stub is reached with x0 = `this`, so each
 * of these just supplies its own field offset and label. */
__attribute__((unused)) static void kb_close_whatsnew(void *self)      { kb_close_panel(self, KB_WHATSNEW_PANEL_OFF,     "CloseWhatsNew"); }
__attribute__((unused)) static void kb_close_newday(void *self)        { kb_close_panel(self, KB_NEWDAY_PANEL_OFF,       "Close_NewDay"); }
__attribute__((unused)) static void kb_close_specialbonus(void *self)  { kb_close_panel(self, KB_SPECIALBONUS_PANEL_OFF, "Close_SpecialBonus"); }
#endif /* KB_HOOK_CLOSE_WHATSNEW */

static int g_boot_hacks_done = 0;
static void nx_boot_il2cpp_hacks(void) {
  if (g_boot_hacks_done) return;
  g_boot_hacks_done = 1;
  uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;

  /* redirect managed Time.get_* to our frame clock (PvZ RVAs, metadata-derived) */
  struct { uint32_t off; void *fn; const char *nm; } th[] = {
    { KB_IL2_get_realtimeSinceStartup, (void *)&nx_realtime_since_startup, "realtimeSinceStartup" },
    { KB_IL2_get_deltaTime,            (void *)&nx_delta_time,             "deltaTime" },
    { KB_IL2_get_unscaledDeltaTime,    (void *)&nx_delta_time,             "unscaledDeltaTime" },
    { KB_IL2_get_smoothDeltaTime,      (void *)&nx_delta_time,             "smoothDeltaTime" },
    { KB_IL2_get_time,                 (void *)&nx_time_f,                 "time" },
    { KB_IL2_get_unscaledTime,         (void *)&nx_time_f,                 "unscaledTime" },
    { KB_IL2_get_timeSinceLevelLoad,   (void *)&nx_time_f,                 "timeSinceLevelLoad" },
    { KB_IL2_get_frameCount,           (void *)&nx_frame_count,            "frameCount" },
  };
  for (unsigned i = 0; i < sizeof(th) / sizeof(th[0]); i++) {
    /* A zero offset means the property does not exist in THIS build (this game
     * has no Time.smoothDeltaTime -- the managed linker stripped it). Skip it
     * explicitly rather than probing libil2cpp+0, which would read the ELF
     * header and log a confusing "not the expected prologue". */
    if (th[i].off == 0) {
      debugPrintf("[boot] Time.get_%s: absent in this build, nothing to hook\n",
                  th[i].nm);
      continue;
    }
    uint32_t cur = *(volatile uint32_t *)(b + th[i].off);
    if (cur != KB_TIME_THUNK_WORD) {
      debugPrintf("[boot] SKIP Time.get_%s @il2cpp+0x%x: 0x%08x is not the "
                  "expected icall-thunk prologue -- re-dump (PORTING sec 6)\n",
                  th[i].nm, th[i].off, cur);
      continue;
    }
    uint32_t s[4];
    s[0] = 0x58000050u; s[1] = 0xd61f0200u;   /* ldr x16,#8 ; br x16 */
    memcpy(&s[2], &th[i].fn, 8);
    so_patch_code((void *)(b + th[i].off), s, sizeof s);
    debugPrintf("[boot] hooked Time.get_%s @il2cpp+0x%x\n", th[i].nm, th[i].off);
  }
#if KB_HOOK_CLOSE_WHATSNEW
  /* Verify-first, like every other patch in this tree: if the prologue is not
   * the word dump.cs + disassembly agreed on, the game was updated and the RVA
   * is stale -- log and leave it alone rather than writing a stub over whatever
   * happens to live there now. */
#if KB_WHATSNEW_HOOKS && KB_WN_AWAKE_TRAMPOLINE
  /* Awake/Start trampolines. Resolve the two ADRP page values and the resume
   * address from the runtime base first -- the thunk reads them, so they must
   * be set BEFORE the entry stub is written or the first call jumps to zero.
   * Both words of the shared prologue are verified, not just the first: these
   * two methods sit adjacent to several siblings with identical opening
   * instructions, so one word is a weaker guarantee than it looks. */
  { struct { uint32_t off; uint32_t pgA, pgB; void *thunk;
             uint64_t *x20, *x21, *res; const char *nm; int swap; } wn[] = {
      { KB_IL2_WhatsNew_Awake, KB_WN_PAGE_A, KB_WN_PAGE_AWAKE_B,
        (void *)&kb_wn_awake_thunk, &g_wn_awake_x20, &g_wn_awake_x21,
        &g_wn_awake_resume, "Awake", 0 },
#if !KB_SKIP_WHATSNEW
      { KB_IL2_WhatsNew_Start, KB_WN_PAGE_A, KB_WN_PAGE_START_B,
        (void *)&kb_wn_start_thunk, &g_wn_start_x20, &g_wn_start_x21,
        &g_wn_start_resume, "Start", 1 },
#endif
    };
    for (unsigned i = 0; i < sizeof(wn)/sizeof(wn[0]); i++) {
      const uint32_t w0 = *(volatile uint32_t *)(b + wn[i].off);
      const uint32_t w1 = *(volatile uint32_t *)(b + wn[i].off + 4);
      if (w0 != KB_WHATSNEW_PRO_W0 || w1 != KB_WHATSNEW_PRO_W1) {
        debugPrintf("[ui] SKIP %s trampoline @il2cpp+0x%x: prologue %08x %08x != "
                    "expected %08x %08x\n", wn[i].nm, wn[i].off, w0, w1,
                    (unsigned)KB_WHATSNEW_PRO_W0, (unsigned)KB_WHATSNEW_PRO_W1);
        continue;
      }
      /* Awake loads x20 from page A and x21 from page B; Start is the other way
       * round. Getting this backwards would leave both methods reading their
       * once-flags and statics out of the wrong page -- silently. */
      *wn[i].x20 = (uint64_t)(b + (wn[i].swap ? wn[i].pgB : wn[i].pgA));
      *wn[i].x21 = (uint64_t)(b + (wn[i].swap ? wn[i].pgA : wn[i].pgB));
      *wn[i].res = (uint64_t)(b + wn[i].off + 0x10);
      __builtin___clear_cache((char *)wn[i].x20, (char *)wn[i].res + 8);

      uint32_t st[4];
      st[0] = 0x58000050u; st[1] = 0xd61f0200u;   /* ldr x16,#8 ; br x16 */
      memcpy(&st[2], &wn[i].thunk, 8);
      so_patch_code((void *)(b + wn[i].off), st, sizeof st);
      debugPrintf("[ui] trampolined LevelMap_WhatsNew.%s @il2cpp+0x%x -> %p "
                  "(x20=%llx x21=%llx resume=%llx)\n", wn[i].nm, wn[i].off,
                  wn[i].thunk, (unsigned long long)*wn[i].x20,
                  (unsigned long long)*wn[i].x21, (unsigned long long)*wn[i].res);
    }
  }

#endif /* KB_WHATSNEW_HOOKS && KB_WN_AWAKE_TRAMPOLINE */

#if KB_WHATSNEW_HOOKS
  /* Per-entry expected prologue word. The three close handlers open with
   *   F81E0FF4  str x20, [sp, #-0x20]!
   * but Start opens with
   *   A9BE53F5  stp x21, x20, [sp, #-0x20]!
   * so a single shared constant would have failed Start's verify and silently
   * skipped the very hook that suppresses the popup. */
  { struct { uint32_t off; uint32_t word; void *fn; const char *nm; } cw[] = {
#if KB_WN_CLOSE_HOOKS
      { KB_IL2_CloseWhatsNew,      KB_CLOSE_WHATSNEW_WORD, (void *)&kb_close_whatsnew,     "CloseWhatsNew" },
      { KB_IL2_Close_NewDay,       KB_CLOSE_WHATSNEW_WORD, (void *)&kb_close_newday,       "Close_NewDay" },
      { KB_IL2_Close_SpecialBonus, KB_CLOSE_WHATSNEW_WORD, (void *)&kb_close_specialbonus, "Close_SpecialBonus" },
#endif
#if KB_SKIP_WHATSNEW
      { KB_IL2_WhatsNew_Start,     KB_WHATSNEW_PRO_W0,     (void *)&kb_wn_start_skip,      "Start(skip)" },
#endif
    };
    for (unsigned i = 0; i < sizeof(cw)/sizeof(cw[0]); i++) {
      const uint32_t cur = *(volatile uint32_t *)(b + cw[i].off);
      if (cur != cw[i].word) {
        debugPrintf("[ui] SKIP %s hook @il2cpp+0x%x: 0x%08x != expected 0x%08x "
                    "-- re-dump and update the RVA\n",
                    cw[i].nm, cw[i].off, cur, cw[i].word);
        continue;
      }
      uint32_t st[4];
      st[0] = 0x58000050u; st[1] = 0xd61f0200u;   /* ldr x16,#8 ; br x16 */
      memcpy(&st[2], &cw[i].fn, 8);
      so_patch_code((void *)(b + cw[i].off), st, sizeof st);
      debugPrintf("[ui] hooked LevelMap_WhatsNew.%s @il2cpp+0x%x -> %p\n",
                  cw[i].nm, cw[i].off, cw[i].fn);
    }
  }
#endif /* KB_WHATSNEW_HOOKS (close/skip replacements) */

#if KB_OFFLINE_PURCHASES
  /* Round 160. Replace the two buy-button handlers so a press restores an item
   * the player has claimed in purchases.txt, instead of calling a store that
   * cannot exist here. The grant itself is the game's own Owned_* method,
   * called from kb_purchases.c -- not patched, so the real unlock/save code
   * runs. Per-entry expected words: the two handlers happen to share a
   * prologue, and checking each against its own address is what stops a shared
   * constant from silently patching the wrong function. */
  { extern void kb_purchases_init(const char *, uintptr_t);
    extern void kb_purchase_weapons_pack(void *, void *);
    extern void kb_purchase_unlimited_ammo(void *, void *);
    kb_purchases_init(DATA_ROOT, b);
    struct { uint32_t off, w0, w1; void *fn; const char *nm; } iw[] = {
      { KB_IL2_Purchase_Weapons_Pack,   KB_PURCHASE_WEAPONS_W0, KB_PURCHASE_WEAPONS_W1,
        (void *)&kb_purchase_weapons_pack,   "Purchase_Weapons_Pack" },
      { KB_IL2_Purchase_Unlimited_Ammo, KB_PURCHASE_AMMO_W0,    KB_PURCHASE_AMMO_W1,
        (void *)&kb_purchase_unlimited_ammo, "Purchase_Unlimited_Ammo" },
    };
    for (unsigned i = 0; i < sizeof(iw)/sizeof(iw[0]); i++) {
      const volatile uint32_t *p = (const volatile uint32_t *)(b + iw[i].off);
      if (p[0] != iw[i].w0 || p[1] != iw[i].w1) {
        debugPrintf("[iap] SKIP %s @il2cpp+0x%x: 0x%08x,0x%08x != expected "
                    "0x%08x,0x%08x\n", iw[i].nm, iw[i].off, p[0], p[1],
                    iw[i].w0, iw[i].w1);
        continue;
      }
      uint32_t st[4];
      st[0] = 0x58000050u; st[1] = 0xd61f0200u;   /* ldr x16,#8 ; br x16 */
      memcpy(&st[2], &iw[i].fn, 8);
      so_patch_code((void *)(b + iw[i].off), st, sizeof st);
      debugPrintf("[iap] hooked %s @il2cpp+0x%x -> %p\n", iw[i].nm, iw[i].off, iw[i].fn);
    }
  }
#endif

#if KB_EVENTSYSTEM_TICK_ALL
  /* Round 156. See nx_patch_killerbean.h for the derivation and the evidence. */
  { const uint32_t cur = *(volatile uint32_t *)(b + KB_IL2_EventSystem_Update_Br);
    if (cur != KB_EVENTSYSTEM_BR_WORD) {
      debugPrintf("[ui] SKIP EventSystem.Update patch @il2cpp+0x%x: 0x%08x != "
                  "expected 0x%08x -- re-dump and update the RVA\n",
                  (unsigned)KB_IL2_EventSystem_Update_Br, cur,
                  (unsigned)KB_EVENTSYSTEM_BR_WORD);
    } else {
      const uint32_t nop = KB_ARM64_NOP;
      so_patch_code((void *)(b + KB_IL2_EventSystem_Update_Br), &nop, sizeof nop);
      debugPrintf("[ui] EventSystem.Update: `if (current != this) return` NOPped "
                  "@il2cpp+0x%x -- every EventSystem now ticks its modules\n",
                  (unsigned)KB_IL2_EventSystem_Update_Br);
    }
  }
#endif

#if KB_LEVELMAP_BUTTON_PROBE
  { struct { uint32_t off; uint32_t word; void *fn; const char *nm; } pw[] = {
      { KB_IL2_LevelMap_PlayLevel, KB_LEVELMAP_PLAYLEVEL_WORD,
        (void *)&kb_levelmap_playlevel, "LevelMap_Control.PlayLevel" },
      { KB_IL2_LevelMap_Menu,      KB_LEVELMAP_MENU_WORD,
        (void *)&kb_levelmap_menu,      "LevelMap_Control.Menu" },
    };
    for (unsigned i = 0; i < sizeof(pw)/sizeof(pw[0]); i++) {
      const uint32_t cur = *(volatile uint32_t *)(b + pw[i].off);
      if (cur != pw[i].word) {
        debugPrintf("[ui] SKIP %s probe @il2cpp+0x%x: 0x%08x != expected 0x%08x\n",
                    pw[i].nm, pw[i].off, cur, pw[i].word);
        continue;
      }
      uint32_t st[4];
      st[0] = 0x58000050u; st[1] = 0xd61f0200u;   /* ldr x16,#8 ; br x16 */
      memcpy(&st[2], &pw[i].fn, 8);
      so_patch_code((void *)(b + pw[i].off), st, sizeof st);
      debugPrintf("[ui] PROBE on %s @il2cpp+0x%x -> %p (body suppressed)\n",
                  pw[i].nm, pw[i].off, pw[i].fn);
    }
  }
#endif

#if KB_LEAN_INPUT_FROM_LOADER
  { struct { uint32_t off; uint32_t word; void *fn; const char *nm; } lw[] = {
      { KB_IL2_LeanGetTouchCount, KB_LEAN_GETTOUCHCOUNT_WORD,
        (void *)&kb_lean_get_touch_count, "LeanInput.GetTouchCount" },
      { KB_IL2_LeanGetTouch,      KB_LEAN_GETTOUCH_WORD,
        (void *)&kb_lean_get_touch,       "LeanInput.GetTouch" },
    };
    for (unsigned i = 0; i < sizeof(lw)/sizeof(lw[0]); i++) {
      const uint32_t cur = *(volatile uint32_t *)(b + lw[i].off);
      if (cur != lw[i].word) {
        debugPrintf("[lean] SKIP %s @il2cpp+0x%x: 0x%08x != expected 0x%08x "
                    "-- re-dump and update the RVA\n",
                    lw[i].nm, lw[i].off, cur, lw[i].word);
        continue;
      }
      uint32_t st[4];
      st[0] = 0x58000050u; st[1] = 0xd61f0200u;   /* ldr x16,#8 ; br x16 */
      memcpy(&st[2], &lw[i].fn, 8);
      so_patch_code((void *)(b + lw[i].off), st, sizeof st);
      debugPrintf("[lean] hooked %s @il2cpp+0x%x -> %p\n",
                  lw[i].nm, lw[i].off, lw[i].fn);
    }
  }
#endif

#if KB_LEAN_POINTOVERGUI_FALSE
  /* ---- experiment, round 153 --------------------------------------------
   * Force LeanTouch.PointOverGui() to answer false.
   *
   * The map's level nodes are LeanSelectables polled by LevelMap_Button.Update,
   * and LeanTouch's selectors ignore any finger whose StartedOverGui is set --
   * which is this function, evaluated once at the DOWN through
   * EventSystem.RaycastAll. uGUI does not consult it at all, which is exactly
   * the split observed: store buttons fire, map nodes do nothing, no exception.
   *
   * Binary outcome, and both answers are worth the build:
   *   map responds  -> our EventSystem raycast reports UI under the finger
   *                    where Android's does not; fix that and revert this.
   *   no change     -> PointOverGui is NOT the gate; the next suspect is
   *                    LeanTouch's finger bookkeeping itself.
   *
   * Safe either way: false only means "stop ignoring fingers that began over
   * UI", so at worst a tap on a UI element also reaches the map. */
  { const uint32_t cur = *(volatile uint32_t *)(b + KB_IL2_LeanPointOverGui);
    if (cur != KB_LEAN_POINTOVERGUI_WORD) {
      debugPrintf("[lean] SKIP PointOverGui hook @il2cpp+0x%x: 0x%08x != expected "
                  "0x%08x -- re-dump and update the RVA\n",
                  (unsigned)KB_IL2_LeanPointOverGui, cur,
                  (unsigned)KB_LEAN_POINTOVERGUI_WORD);
    } else {
      uint32_t st[4];
      st[0] = 0x58000050u; st[1] = 0xd61f0200u;   /* ldr x16,#8 ; br x16 */
      void *fn = (void *)&kb_lean_point_over_gui;
      memcpy(&st[2], &fn, 8);
      so_patch_code((void *)(b + KB_IL2_LeanPointOverGui), st, sizeof st);
      debugPrintf("[lean] hooked LeanTouch.PointOverGui @il2cpp+0x%x -> %p "
                  "(forced false)\n", (unsigned)KB_IL2_LeanPointOverGui, fn);
    }
  }
#endif
#endif

#if KB_FORCE_SPLASH_FINISH
  /* Force SplashScreen.m_isSplashFinished (round 57): the end-of-fade Animation
   * Event that calls SplashFinished() does not fire on our port, so patch the
   * coroutine's flag load (ldrb w8,[x21,#0x48]) to mov w8,#1. */
  { uint32_t *site = (uint32_t *)(b + KB_IL2_SPLASH_FINISHED_CHECK);
    if (*site == 0x394122a8u) {
      uint32_t mov1 = 0x52800028u;
      so_patch_code((void *)site, &mov1, sizeof mov1);
      debugPrintf("[boot] forced splash-finish gate @il2cpp+0x%x (was ldrb, now mov w8,#1)\n",
                  (unsigned)KB_IL2_SPLASH_FINISHED_CHECK);
    } else {
      debugPrintf("[boot] SKIP splash-finish force @il2cpp+0x%x: 0x%08x != expected ldrb 0x394122a8\n",
                  (unsigned)KB_IL2_SPLASH_FINISHED_CHECK, *site);
    } }
#endif
#if KB_BYPASS_UNITY_SPLASH
  /* Bypass Unity native splash (round 58): GetShouldShowSplashScreen -> 0 so
   * the engine never holds on its own splash screen. libunity, not il2cpp. */
  { uintptr_t ub2 = (uintptr_t)unity_mod.load_virtbase;
    volatile uint32_t *gs = (uint32_t *)(ub2 + KB_GET_SHOULD_SHOW_SPLASH);
    if (gs[0] == 0xf81f0ffeu && gs[1] == 0x941aaa72u) {
      uint32_t ret0[2] = { 0x52800000u, 0xd65f03c0u };  /* mov w0,#0 ; ret */
      so_patch_code((void *)gs, ret0, sizeof ret0);
      so_flush_caches(&unity_mod);
      debugPrintf("[boot] bypassed Unity native splash (GetShouldShowSplashScreen->0) @libunity+0x%x\n",
                  (unsigned)KB_GET_SHOULD_SHOW_SPLASH);
    } else if (gs[0] == 0x52800000u && gs[1] == 0xd65f03c0u) {
      debugPrintf("[boot] Unity native splash already bypassed\n");
    } else {
      debugPrintf("[boot] SKIP native-splash bypass @libunity+0x%x: 0x%08x 0x%08x != expected\n",
                  (unsigned)KB_GET_SHOULD_SHOW_SPLASH, gs[0], gs[1]);
    } }
#endif
#if KB_PRELOAD_NO_EARLY_EXIT
  /* Round 75: keep retrying SingleStep for the whole budget instead of
   * abandoning the frame when the integrate queue is briefly empty. */
  { uintptr_t ub4 = (uintptr_t)unity_mod.load_virtbase;
    volatile uint32_t *eb = (uint32_t *)(ub4 + KB_PRELOAD_EXIT_BRANCH);
    uint32_t nop = 0xd503201fu;
    if (eb[0] == 0x36000160u) {
      so_patch_code((void *)eb, &nop, sizeof nop);
      so_flush_caches(&unity_mod);
      debugPrintf("[boot] preload early-exit NOPed @libunity+0x%x\n",
                  (unsigned)KB_PRELOAD_EXIT_BRANCH);
    } else if (eb[0] == nop) {
      debugPrintf("[boot] preload early-exit already NOPed\n");
    } else {
      debugPrintf("[boot] SKIP preload early-exit @libunity+0x%x: 0x%08x != expected\n",
                  (unsigned)KB_PRELOAD_EXIT_BRANCH, eb[0]);
    } }
#endif
#if KB_PRELOAD_BUDGET_MS
  /* Raise the async-load per-frame budget (round 68). See config.h. */
  { uintptr_t ub3 = (uintptr_t)unity_mod.load_virtbase;
    volatile uint32_t *tb = (uint32_t *)(ub3 + KB_PRELOAD_BUDGET_TABLE_LOAD);
    volatile uint32_t *df = (uint32_t *)(ub3 + KB_PRELOAD_BUDGET_DEFAULT);
    uint32_t movN = 0x52800015u | (((uint32_t)KB_PRELOAD_BUDGET_MS & 0xffffu) << 5);
    if (tb[0] == 0xb8687935u && df[0] == 0x52800095u) {
      so_patch_code((void *)tb, &movN, sizeof movN);
      so_patch_code((void *)df, &movN, sizeof movN);
      so_flush_caches(&unity_mod);
      debugPrintf("[boot] async-load budget -> %d ms/frame @libunity+0x%x\n",
                  (int)KB_PRELOAD_BUDGET_MS, (unsigned)KB_PRELOAD_BUDGET_TABLE_LOAD);
    } else if (tb[0] == movN && df[0] == movN) {
      debugPrintf("[boot] async-load budget already raised\n");
    } else {
      debugPrintf("[boot] SKIP async-load budget @libunity+0x%x: 0x%08x 0x%08x != expected\n",
                  (unsigned)KB_PRELOAD_BUDGET_TABLE_LOAD, tb[0], df[0]);
    } }
#endif
  so_flush_caches(&il2cpp_mod);   /* make the Time-hook code patches live */
  debugPrintf("[boot] Time.get_* hooks installed; GC left running (handled by bridge)\n");
}

int main(int argc, char *argv[]) {
  /* FIRST, before anything that touches a path -- debugPrintf included, since
   * the log file itself lives under the resolved root. This makes the folder
   * name on the SD card irrelevant: the root comes from where the .nro
   * actually is. See nx_data_root.h. */
  nx_resolve_data_root(argc, argv);

  socketInitializeDefault();
  debugPrintf("[boot] === killerbean_nx start (Unity 2021.3.31f1, region64mb build) ===\n");
  debugPrintf("[boot] data root: %s\n", g_data_root);
  debugPrintf("[boot] resolved:  %s\n", g_data_root_how);
  debugPrintf("[boot] argv[0]:   %s\n",
              (argc >= 1 && argv && argv[0]) ? argv[0] : "(none)");

  /* Load config.txt. When the file is
   * missing, autogenerate a documented one with the defaults; when it holds
   * retired options (`portrait` among them now), rewrite it without them. */
  {
    int crc = read_config(nx_path("/" CONFIG_NAME));
    if (crc != 0) {
      write_config(nx_path("/" CONFIG_NAME));
      debugPrintf("[boot] %s config.txt (handheld_res=%d docked_res=%d)\n",
                  crc < 0 ? "created" : "rewrote",
                  config.handheld_res, config.docked_res);
    } else {
      debugPrintf("[boot] config: handheld_res=%d docked_res=%d\n",
                  config.handheld_res, config.docked_res);
    }
  }

  /* Sweep Unity's case-sensitivity probe files: CASESENSITIVETEST<guid> strays
   * from older builds, plus the single hidden scratch the probe is redirected
   * to now (libc_shim.c casetest_redirect). */
  {
    DIR *dd = opendir(DATA_ROOT);
    int swept = 0;
    if (dd) {
      struct dirent *de;
      while ((de = readdir(dd))) {
        if (strncasecmp(de->d_name, "CASESENSITIVETEST", 17) == 0 ||
            strcmp(de->d_name, ".casetest") == 0) {
          char pth[320]; snprintf(pth, sizeof pth, "%s/%s", DATA_ROOT, de->d_name);
          if (unlink(pth) == 0) swept++;
        }
      }
      closedir(dd);
    }
    if (swept) debugPrintf("[boot] swept %d case-sensitivity probe file(s)\n", swept);
  }

  /* CWD fix (mirrors MMX enter_data_dir): title-override / hbloader leaves the
   * working dir at the .nro folder or the SD root, NOT the game dir. Unity &
   * il2cpp read many files through *relative* paths ("assets/bin/Data/...") and
   * our basename_fallback stats relative to cwd, so a wrong cwd silently yields
   * empty/missing reads -> NULL il2cpp classes. chdir into DATA_ROOT so every
   * relative read resolves under sdmc:/switch/zookeeper. (Absolute "sdmc:/..."
   * reads are unaffected.) */
  {
    char cwd[256] = {0};
    getcwd(cwd, sizeof cwd);
    int rc = chdir(DATA_ROOT);
    char cwd2[256] = {0};
    getcwd(cwd2, sizeof cwd2);
    struct stat st;
    /* NOTE: this probe runs BEFORE the pack is mounted (mount happens ~200
     * lines below), so on a packed install these are expected to read 0 --
     * the loose files are gone and the pack is not up yet. The label below
     * says which source answered so the line cannot be misread as "the data
     * is missing", which is how it wasted time during the round-85/86 hunt.
     * The pack lookups are kept for the case where mounting moves earlier. */
    int reach_assets = stat("assets/bin/Data/data.unity3d", &st) == 0;
    int reach_meta   = stat("assets/bin/Data/Managed/Metadata/global-metadata.dat", &st) == 0;
    int reach_guid   = stat("assets/bin/Data/unity_app_guid", &st) == 0;
#if KB_ASSET_PACK
    const int _packed = asset_pack_active();
    if (_packed) {
      if (!reach_assets) reach_assets = asset_pack_stat_path("assets/bin/Data/data.unity3d", NULL, NULL);
      if (!reach_meta)   reach_meta   = asset_pack_stat_path("assets/bin/Data/Managed/Metadata/global-metadata.dat", NULL, NULL);
      if (!reach_guid)   reach_guid   = asset_pack_stat_path("assets/bin/Data/unity_app_guid", NULL, NULL);
    }
#else
    const int _packed = 0;
#endif
    debugPrintf("[boot] cwd was '%s' -> chdir(%s)=%d -> '%s'\n", cwd, DATA_ROOT, rc, cwd2);
    debugPrintf("[boot] reachable(%s): data.unity3d=%d metadata=%d unity_app_guid=%d\n",
                _packed ? "pack" : "rel, pre-mount", reach_assets, reach_meta, reach_guid);
  }

  /* Force libunity to RE-EXTRACT il2cpp resources every boot. Observed: when
   * extraction is skipped (il2cpp/unity.ver present), il2cpp mmaps the extracted
   * global-metadata.dat and crashes in Class::Init(NULL); when extraction RUNS,
   * il2cpp uses the full source it reads for the copy and gets past that point.
   * The extracted copy is bad because our shim doesn't flush a writable
   * file-backed mmap back to disk, so it lands truncated. Removing the extracted
   * markers makes libunity redo the extraction each boot (uses the good source).
   * Proper fix = flush writable file-backed mmaps on munmap (tracked separately). */
  {
    /* Wipe BOTH trees, not three hand-listed files: a stale 0-byte
     * global-metadata.dat left directly under il2cpp/ was surviving every
     * boot because none of the three paths below matched it. */
    nx_rmtree(nx_path("/il2cpp"));
    nx_rmtree(nx_path("/il2cpp_tmp"));
    int a = unlink(nx_path("/il2cpp/unity.ver"));
    int b = unlink(nx_path("/il2cpp/Metadata/global-metadata.dat"));
    int c = unlink(nx_path("/il2cpp/Resources/mscorlib.dll-resources.dat"));
    debugPrintf("[boot] force re-extract: unlink unity.ver=%d metadata=%d resources=%d\n", a, b, c);
  }

  check_syscalls();
  debugPrintf("[boot] syscalls ok\n");
  {
    extern char *fake_heap_start, *fake_heap_end;
    debugPrintf("[boot] mem layout: newlib=%u MB, mmap arena=%u MB @ %p\n",
                (unsigned)((fake_heap_end - fake_heap_start) / (1024 * 1024)),
                (unsigned)(g_mmap_arena_size / (1024 * 1024)), g_mmap_arena_base);
    if (g_overcommit)
      debugPrintf("[boot] OVERCOMMIT on: heap shrunk to %u MB, freed %u MB physical; "
                  "arena reserved virtual @ %p (commit on demand)\n",
                  g_oc_heap_mb, g_oc_freed_mb, g_mmap_arena_base);
    else
      debugPrintf("[boot] OVERCOMMIT off (heap-backed): system_resource=%u MB "
                  "(svcMapPhysicalMemory needs >0; unsafe pool exhausted). map_hint=%d alias=%u MB\n",
                  (unsigned)(g_oc_sysres >> 20), g_oc_hint_map, g_oc_alias_mb);
  }

  /* Overcommit feasibility probe. Proper PROT_NONE overcommit on Switch needs a
   * physical-backing primitive (svcMapMemory in the stack region, or
   * svcMapPhysicalMemory in the alias region) plus a region large enough to hold
   * Unity's multi-GB reservations. MMX found svcMapMemory caps at ~2-3 pools (the
   * stack region is ~1GB). Log the region sizes + which mapping svc are granted so
   * we can size/choose the real overcommit (or rule it out) from real numbers. */
  {
    struct { const char *nm; int a, s; } R[] = {
      { "alias", InfoType_AliasRegionAddress, InfoType_AliasRegionSize },
      { "heap",  InfoType_HeapRegionAddress,  InfoType_HeapRegionSize  },
      { "stack", InfoType_StackRegionAddress, InfoType_StackRegionSize },
    };
    for (unsigned i = 0; i < 3; i++) {
      u64 a = 0, s = 0;
      svcGetInfo(&a, R[i].a, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&s, R[i].s, CUR_PROCESS_HANDLE, 0);
      debugPrintf("[probe] region %-5s base=0x%lx size=%u MB\n",
                  R[i].nm, (unsigned long)a, (unsigned)(s >> 20));
    }
    u64 tot = 0, used = 0;
    svcGetInfo(&tot,  InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
    debugPrintf("[probe] mem total=%u MB used=%u MB free=%u MB\n",
                (unsigned)(tot >> 20), (unsigned)(used >> 20),
                (unsigned)((tot - used) >> 20));
    debugPrintf("[probe] svc hinted: MapPhysicalMemory(0x2c)=%d UnmapPhysical(0x2d)=%d "
                "MapMemory(0x24)=%d UnmapMemory(0x25)=%d\n",
                envIsSyscallHinted(0x2c), envIsSyscallHinted(0x2d),
                envIsSyscallHinted(0x24), envIsSyscallHinted(0x25));
  }

  /* Decisive probe: does svcMapMemory accept a dst in the (unmapped upper) HEAP
   * region? The stack region works but is only ~2GB (~8 regions). The heap region
   * is 8GB; its upper ~5GB sits unmapped above our heap. If svcMapMemory works
   * there too, we can host Unity's 256MB pools in ~7GB of backable address space
   * (~28 regions) without any libunity patching. Pure diagnostic: map 1 page,
   * verify the sentinel reads back, unmap. */
  {
    u64 hbase = 0, hsize = 0;
    svcGetInfo(&hbase, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&hsize, InfoType_HeapRegionSize,    CUR_PROCESS_HANDLE, 0);
    u64 probe = 0, a = hbase, end = hbase + hsize;
    while (a < end) {
      MemoryInfo mi; u32 pi;
      if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
      if (mi.addr + mi.size <= a) break;
      if (mi.type == MemType_Unmapped && mi.size >= MMAP_ARENA_ALIGN) {
        u64 al = (mi.addr + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
        if (al + 0x1000 <= mi.addr + mi.size) { probe = al; break; }
      }
      a = mi.addr + mi.size;
    }
    if (probe) {
      void *src = memalign(0x1000, 0x1000);
      if (src) {
        *(volatile u32 *)src = 0xABCD1234;
        Result rc = svcMapMemory((void *)probe, src, 0x1000);
        if (R_SUCCEEDED(rc)) {
          u32 v = *(volatile u32 *)probe;
          Result u = svcUnmapMemory((void *)probe, src, 0x1000);
          debugPrintf("[heapprobe] heap-region svcMapMemory @ 0x%lx rc=0x%x read=0x%x unmap=0x%x WORKS=%d\n",
                      (unsigned long)probe, rc, v, u, v == 0xABCD1234);
          if (R_SUCCEEDED(u)) free(src);
        } else {
          debugPrintf("[heapprobe] heap-region svcMapMemory @ 0x%lx FAILED rc=0x%x\n",
                      (unsigned long)probe, rc);
          free(src);
        }
      }
    } else {
      debugPrintf("[heapprobe] no unmapped 256MB-aligned spot found in heap region\n");
    }
  }

  /* Arm the stack-region overcommit arena. The boot probe confirmed svcMapMemory
   * aliases heap pages into the stack region; Unity reserves ~2.8GB of PROT_NONE
   * pools but commits only ~80MB. Reserve a 1280MB stack-region window (cheap
   * address space) + a 256MB heap commit-pool; the OC arena (libc_shim.c) then
   * holds Unity's big reservations there and aliases pool pages in on mprotect.
   * Any failure leaves OC disabled and the engine runs on the heap-backed arena. */
  {
    void *pool = NULL;
    size_t winsz = 0;
    void *win = oc_find_stack_window(OC_WINDOW_BYTES, &winsz);
    VirtmemReservation *rv = NULL;
    if (win && winsz) {
      virtmemLock();
      rv = virtmemAddReservation(win, winsz);   // keep libnx thread stacks out
      virtmemUnlock();
    }
    if (win && rv && winsz) {
      /* Step down rather than give up. A single memalign() of the full pool used
       * to be all-or-nothing: if it did not fit, OC was DISABLED and the engine
       * fell back to the heap-backed arena alone -- which at its current size has
       * no chance. That made OC_POOL_BYTES dangerous to raise, because the
       * failure mode of asking for too much was a dead boot rather than a
       * smaller pool. Now the ladder retries in 128 MB steps down to
       * OC_POOL_MIN_BYTES, so raising the constant can only ever help. */
      size_t pool_sz = OC_POOL_BYTES;
      for (;;) {
        pool = memalign(0x1000, pool_sz);
        if (pool || pool_sz <= OC_POOL_MIN_BYTES) break;
        pool_sz -= (size_t)128 * 1024 * 1024;
        debugPrintf("[oc] pool %u MB did not fit -- retrying at %u MB\n",
                    (unsigned)((pool_sz + (128u << 20)) >> 20), (unsigned)(pool_sz >> 20));
      }
      if (pool && oc_arena_init(win, winsz, pool, pool_sz)) {
        debugPrintf("[oc] ARMED: window %u MB @ %p, pool %u MB @ %p, heap-backed arena %u MB "
                    "(total reserve %u MB)%s\n",
                    (unsigned)(winsz >> 20), win, (unsigned)(pool_sz >> 20), pool,
                    (unsigned)(g_mmap_arena_size >> 20),
                    (unsigned)((winsz + g_mmap_arena_size) >> 20),
                    pool_sz < OC_POOL_BYTES ? "  [stepped down from OC_POOL_BYTES]" : "");
        if (g_oc_win2 && g_oc_win2_sz) {
          virtmemLock();
          VirtmemReservation *rv2 = virtmemAddReservation(g_oc_win2, g_oc_win2_sz);
          virtmemUnlock();
          if (rv2 && oc_arena_add_window(g_oc_win2, g_oc_win2_sz))
            debugPrintf("[oc] ARMED window 2: %u MB @ %p (total window VA %u MB)\n",
                        (unsigned)(g_oc_win2_sz >> 20), g_oc_win2,
                        (unsigned)((winsz + g_oc_win2_sz) >> 20));
        }
      }
      else
        debugPrintf("[oc] DISABLED: pool=%p init failed -> heap-backed only\n", pool);
    } else {
      debugPrintf("[oc] DISABLED: no usable stack hole (win=%p sz=%u MB rv=%p) -> heap-backed only\n",
                  win, (unsigned)(winsz >> 20), (void *)rv);
    }
  }

  /* fbstub45 PORTRAIT: ZOOKEEPER is a portrait game. Report a portrait surface
   * (W<H) everywhere Unity reads dimensions so the engine renders upright. The
   * compositor stretches the portrait buffer onto the landscape panel. (Stable;
   * the 640x1137 native-render attempt crashed early in boot -- the game's
   * pipeline appears to depend on its Screen.SetResolution low-res path.) */
  if (appletGetOperationMode() == AppletOperationMode_Console) { screen_width = 1080; screen_height = 1920; }
  else                                                         { screen_width = 720;  screen_height = 1280; }

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    debugPrintf("SDL_Init failed: %s\n", SDL_GetError());

  /* Reassemble any *.splitN assets before the preflight check. Unity does
   * this in Java on a phone (see nx_splitjoin.c); we have no Java, so we
   * do it once, here, directly on the SD card. No-op after first boot. */
  /* round 78: is the asset data repacked? One stat, no directory scan. */
  { char mp[768]; struct stat mst;
    snprintf(mp, sizeof mp, "%s/assets/bin/Data/merged0.assets", DATA_ROOT);
    if (stat(mp, &mst) == 0) {
      debugPrintf("[data] repacked assets detected (merged chunks present)\n");
    } else {
      debugPrintf("\n"
        "[data] ****************************************************************\n"
        "[data] * Asset data is NOT repacked.                                  *\n"
        "[data] * The first scene load will take a very long time (~20+ min),  *\n"
        "[data] * because the game loads ~4200 separate resource files and     *\n"
        "[data] * each load costs far more here than it does on Android.       *\n"
        "[data] * Run tools/asset_repack.py then tools/apply_repack.py on a PC *\n"
        "[data] * to fold them into a few chunks (~36 loads). See README.      *\n"
        "[data] ****************************************************************\n\n");
    } }
#if KB_ASSET_PACK
  /* round 82: handle the asset pack FIRST. If a pack exists, mount it and
   * skip joining/building (the loose tree is gone). Otherwise join splits,
   * build the pack from the joined files, and delete the loose tree. Doing
   * this before check_data() means its stat()s resolve through the pack. */
  int _pack_mounted = 0;
  if (asset_pack_open_existing(DATA_ROOT)) {
    _pack_mounted = 1;
    nx_create_asset_skeleton();   /* every boot: runtime writes need these */
    debugPrintf("[pack] mounted existing pack (%zu entries)\n",
                asset_pack_entry_count());
  }
#else
  int _pack_mounted = 0; (void)_pack_mounted;
#endif

  /* round 69: the join scan is a full readdir over ~4638 entries. Once the
   * four split sets are joined there is nothing to do, so check those four
   * base files first and skip the scan in the normal case. */
  if (!_pack_mounted)
  { /* Killer Bean ships NO .split parts (asset survey: 0 found), so this
     * fast path exists only to skip the directory scan. Probing the files
     * this game actually has keeps the skip working instead of always
     * falling through to a full scan that will find nothing. */
    /* Suffixes, joined at runtime: nx_path() is a function call, so it cannot
     * appear in a static initializer now that the root is resolved at boot. */
    static const char *jb[] = {
      "/assets/bin/Data/data.unity3d",
      "/assets/bin/Data/resources.resource",
      "/assets/bin/Data/sharedassets1.resource",
      "/assets/bin/Data/unity default resources" };
    struct stat jst; int need = 0;
    for (unsigned i = 0; i < sizeof(jb)/sizeof(*jb); i++)
      if (stat(nx_path(jb[i]), &jst) < 0) { need = 1; break; }
    if (need) nx_join_split_assets(nx_path("/assets/bin/Data"));
    else      debugPrintf("[join] all split sets already joined; scan skipped\n");
  }
#if KB_ASSET_PACK
  /* Build the pack on first boot, now that splits are joined. */
  if (!_pack_mounted) {
    debugPrintf("[pack] no pack yet; building (first boot, may take minutes)\n");
    { uint64_t tot = 0; unsigned nf = 0;
      nx_tree_stats(nx_path("/assets"), &nf, &tot);
      debugPrintf("[pack] loose tree: %u files, %llu MB\n",
                  nf, (unsigned long long)(tot >> 20));
      struct statvfs vfs;
      if (statvfs(DATA_ROOT, &vfs) == 0) {
        uint64_t freeb = (uint64_t)vfs.f_bavail * vfs.f_frsize;
        debugPrintf("[pack] SD free: %llu MB (need ~%llu MB for the pack)\n",
                    (unsigned long long)(freeb >> 20),
                    (unsigned long long)(tot >> 20));
        if (freeb < tot + (64u << 20))
          debugPrintf("[pack] *** WARNING: not enough free space; build will "
                      "likely fail. Free ~%llu MB and retry. ***\n",
                      (unsigned long long)(tot >> 20));
      } }
    debugPrintf("[pack] build: starting\n");
    if (!asset_pack_build(nx_path("/assets"), DATA_ROOT)) {
      debugPrintf("[pack] build FAILED: %s (keeping loose files)\n",
                  asset_pack_error());
    } else if (asset_pack_open_existing(DATA_ROOT)) {
      _pack_mounted = 1;
      debugPrintf("[pack] built + mounted pack (%zu entries)\n",
                  asset_pack_entry_count());
      startup_status_update("Removing unpacked asset files");
      debugPrintf("[pack] removing loose assets/ tree\n");
      nx_rmtree(nx_path("/assets"));
      nx_create_asset_skeleton();   /* runtime writes need somewhere to go */
    }
    startup_status_end();   /* release the progress console */
  }
#endif

  nx_dump_dir(nx_path("/il2cpp"), "before-boot");
  nx_dump_dir(nx_path("/il2cpp/Metadata"), "before-boot");
  check_data(_pack_mounted);

  /* load the three modules; libil2cpp resolves its engine calls against libunity
   * module-to-module during relocation. */
  debugPrintf("[boot] loading modules...\n");
  /* Report the FULL path on failure. "Could not load libmain.so" is useless
   * when the file is visibly on the card -- the actual fault is almost always
   * that DATA_ROOT and the SD folder name disagree, and you cannot see that
   * from the bare filename. Note debug.log lives under the same root, so if
   * the root is wrong there is no log either; this on-screen text is all you
   * get. */
  if (load_module(&main_mod,   LIB_MAIN)   < 0)
    fatal_error("Could not load %s\n\nLooked in: %s/%s\n\n"
                "If the file IS there, the folder name does not match.\n"
                "This build expects exactly: %s",
                LIB_MAIN, DATA_ROOT, LIB_MAIN, DATA_ROOT);
  debugPrintf("[boot] loaded libmain   @ virtbase %p\n", (void *)main_mod.load_virtbase);
  if (load_module(&unity_mod,  LIB_UNITY)  < 0) fatal_error("Could not load %s\n\nLooked in: %s/%s", LIB_UNITY, DATA_ROOT, LIB_UNITY);
  debugPrintf("[boot] loaded libunity  @ virtbase %p\n", (void *)unity_mod.load_virtbase);
  if (load_module(&il2cpp_mod, LIB_IL2CPP) < 0) fatal_error("Could not load %s\n\nLooked in: %s/%s", LIB_IL2CPP, DATA_ROOT, LIB_IL2CPP);
  debugPrintf("[boot] loaded libil2cpp @ virtbase %p\n", (void *)il2cpp_mod.load_virtbase);



  /* hand the il2cpp exec base to the GC stop-the-world bridge in libc_shim.c so
   * our pthread_kill can ack the Boehm GC's (undeliverable) suspend/restart
   * signals via its semaphore at il2cpp+0x25936c0. */
  g_il2cpp_base = (uintptr_t)il2cpp_mod.load_virtbase;

  /* Firebase native libs are intentionally NOT loaded. The first scene's
   * FirebaseManager only advances when the managed dependency check reports
   * DependencyStatus.Available(0); on a Switch there is no Google Play Services,
   * so the real libs could never report that (they return UnavailableMissing)
   * AND libFirebaseCppApp crashes our loader in its JNI_OnLoad logging path. We
   * instead answer the SDK's native P/Invoke lookups with stubs (firebase_stub.c
   * via dlsym_fake) that make the check resolve to Available. The 4 .so files can
   * be deleted from sdmc:/switch/zookeeper/. Firebase is cosmetic here (RemoteConfig
   * banner/news textures), so stubbing it costs only those images. */
  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);
  debugPrintf("[boot] modules finalized + flushed (canary-patch disabled)\n");

  /* Patch libunity AFTER finalize/flush -- the same point every other libunity
   * patch below runs at. so_patch_code aliases the target pages via
   * svcMapProcessMemory, but the module's segments must be finalized (mapped
   * with their final RX perms and relocated) first; doing it right after
   * load_module faulted at boot (fbstub94: stock .so on SD, PC in the patch
   * path before finalize). */
  nx_patch_libunity((uintptr_t)unity_mod.load_virtbase);

  /* Force FMOD to use its native OpenSL ES output instead of Unity's Java
   * AudioTrack driver.
   *
   * Mechanism (offsets in this paragraph are the Zookeeper 62f2 reference trace;
   * the PvZ 62f1c1 patch sites are the 0x7xxxxx offsets in the code below).
   * Unity's AudioManager FMOD init calls FMOD::System::setOutput with a
   * requested FMOD_OUTPUTTYPE in w1, derived at +0x6bea84 (mov w1,w21). FMOD's
   * setOutput walks the registered output list (+0xc80e84 loop) and matches the
   * requested type against each output's type field at output+0x78 (copied there
   * from the output description by the registrar +0xc744fc). The type constants,
   * read straight from each getDescriptionEx's desc+0x78:
   *     AudioTrack = 21 (0x15)   <- the default request; needs the JVM run loop
   *     OpenSL ES  = 22 (0x16)   <- callback-driven, self-driving via our shim
   * Default request is 21 (logged previously as "requested output: 21"), so the
   * Java AudioTrack output is selected and, with no JVM consumer, stays silent.
   *
   * fbstub63: rewrite the requested type at the setOutput call site from
   * "mov w1,w21" to "movz w1,#22", so Unity asks FMOD for OPENSL. FMOD finds the
   * registered OpenSL output (type 22), inits it -> dlopen(libOpenSLES.so) ->
   * slCreateEngine (our opensles.c shim) -> the engine drives its own callback
   * buffer queue. No Java handshake, correct lifecycle. The registration path is
   * left untouched (both AudioTrack and OpenSL register normally with their real
   * type fields). 0x2A1503E1 (mov w1,w21) -> 0x528002C1 (movz w1,#0x16). */
  {
    uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
    uint32_t req_opensl = 0x528002C1u; /* movz w1, #22 (FMOD_OUTPUTTYPE OPENSL) */
    /* PvZ 62f1c1 offset 0x757bb8 (signature-matched from zk 0x6bea84). Verify the
     * expected `mov w1,w21` before writing -- belt-and-suspenders, cannot corrupt. */
    if (*(volatile uint32_t *)(ub + KB_FMOD_OUTPUT_SITE) == 0x2A1503E1u) {
      so_patch_code((void *)(ub + KB_FMOD_OUTPUT_SITE), &req_opensl, sizeof req_opensl);
      debugPrintf("[fmod] output forced to OpenSL(22) @libunity+KB_FMOD_OUTPUT_SITE\n");
    } else {
      debugPrintf("[fmod] SKIP force-OpenSL: +fmod = 0x%08x, not `mov w1,w21` -- "
                  "libunity differs from expected 62f1c1 (see PORTING sec 3)\n",
                  *(volatile uint32_t *)(ub + KB_FMOD_OUTPUT_SITE));
    }
    /* Frame-pacing (Swappy) force-disable. This build registers Swappy (9 JNI entrypoints);
     * its init brings up a Choreographer/vsync-driven thread pool that never completes
     * on Switch -- there is no Android Choreographer to deliver frame callbacks -- so
     * engine-init parks in a pthread_join at frame 0 (verified on hardware: UnityMain
     * state=join for 18s, workers hard-parked in Swappy's 0xd6xxxx wait). libunity+
     * 0x652354 is the cached "is frame-pacing enabled?" getter: 13 call sites, each
     * `bl 0x652354 ; tbz w0,#0,<skip>`. Forcing it to return 0 makes every site take
     * the disabled path -> plain eglSwapBuffers, no pacing threads, no join. This is
     * how the Zookeeper base already boots (it never enables Swappy). Verify-first:
     * patch only if the prologue is the expected one. NOTE: this build emits
     * `stp x19,x30` (0xA9BF7BF3), not Fruit Ninja's `stp x30,x19` (0xA9BF4FFE)
     * -- same pair, opposite register order. The expected word now lives in
     * nx_patch_killerbean.h as KB_PACING_GUARD_WORD. */
    if (*(volatile uint32_t *)(ub + KB_PACING_GETTER) == KB_PACING_GUARD_WORD) {
      uint32_t off_pacing[2] = { 0x52800000u /* mov w0,#0 */, 0xD65F03C0u /* ret */ };
      so_patch_code((void *)(ub + KB_PACING_GETTER), off_pacing, sizeof off_pacing);
      debugPrintf("[pace] frame-pacing (Swappy) force-disabled @libunity+KB_PACING_GETTER\n");
    } else {
      debugPrintf("[pace] SKIP Swappy-disable: +pacing = 0x%08x, not the expected prologue -- "
                  "libunity differs (see PORTING)\n", *(volatile uint32_t *)(ub + KB_PACING_GETTER));
    }

    /* fbstub66: neutralise FMOD's OpenSL buffer-geometry validation.
     *
     * After slCreateEngine succeeds, FMOD's OpenSL init (+0xce67a0 -> continuation
     * +0xce6840) validates the output period against the DSP mixer buffer at
     * +0xce68d4..+0xce6924. It reads {sampleRate, framesPerBuffer} from the
     * AudioManager getProperty values (our jni_fake.c: 48000 / 64) and returns
     * FMOD error 60 ("Error initializing output device") if:
     *     sampleRate == 0, OR framesPerBuffer == 0, OR
     *     framesPerBuffer > (dspNumBuffers-1)*dspBufferLength  even after one halving.
     * dspNumBuffers (w20) / dspBufferLength (w21) come from the game's BAKED
     * AudioSettings (AudioSettings::GetDSPBufferSize, +0x646888) and pass straight
     * through the init wrapper +0xce63a8 unchanged. This title's baked buffer is
     * degenerate enough that even a 64-frame period fails the bound -- consistent
     * with dspNumBuffers == 1, which makes the bound (1-1)*len = 0 so NO positive
     * period can ever satisfy it. Reporting a smaller framesPerBuffer alone cannot
     * fix that case, so we also force the final bound check to pass.
     *
     * Patch the terminal compare-branch at +0xce6920 from "b.ls 0xce6930"
     * (0x54000089) to an unconditional "b 0xce6930" (0x14000004): the success path
     * at +0xce6930 then always runs and builds the audio player from the sane
     * sampleRate/channels. The buffer count it derives, N = (w20*w21)/period
     * (+0xce697c), stays >= 1 because we report a small 64-frame period. The two
     * zero-guards above (+0xce68ec/+0xce68fc) are left intact and pass (48000/64
     * are both non-zero). */
    uint32_t b_uncond = 0x14000004u; /* b +0x10 (was b.ls, 0x54000089) -- same local target */
    /* PvZ 62f1c1 offset 0xe11404 (signature-matched from zk 0xce6920). Verify a real b.ls. */
    if (KB_HAVE_FMOD_BUFFER_BYPASS &&
        *(volatile uint32_t *)(ub + KB_FMOD_BUFFER_SITE) == 0x54000089u) {
      so_patch_code((void *)(ub + KB_FMOD_BUFFER_SITE), &b_uncond, sizeof b_uncond);
      debugPrintf("[fmod] OpenSL buffer-geometry check bypassed @libunity+0xe11404\n");
    } else {
      debugPrintf("[fmod] SKIP buffer-geometry bypass: +0xe11404 = 0x%08x, not b.ls -- "
                  "libunity differs from expected 62f1c1 (see PORTING sec 3)\n",
                  *(volatile uint32_t *)(ub + KB_FMOD_BUFFER_SITE));
    }
  }

  /* Definitive probe: read F's canary b.ne at libunity 0xc44c34 in the EXECUTED
   * (removed with the probe itself). */
  /* PvZ debugging probe at libunity+0xc44c34 REMOVED: that address held a
   * specific canary branch in PvZ's 62f1c1 build. Reading it here prints a
   * meaningless word from an unrelated function and invites false
   * conclusions from debug.log. */

  /* The main thread runs init_array + the engine lifecycle; give it its own
   * stable bionic TLS for the stack-protector guard (tpidr_el0+0x28). */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);

  debugPrintf("[boot] running init arrays...\n");
  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&il2cpp_mod);
  so_free_temp(&main_mod); so_free_temp(&unity_mod); so_free_temp(&il2cpp_mod);
  debugPrintf("[boot] init arrays done\n");

  /* fake JNI + our environment, then HID */
  jni_init();
  unity_environment_init(DATA_ROOT);
  android_native_update_mode();
  android_native_input_init();
  debugPrintf("[boot] jni + env + hid ready\n");

  /* resolve UnityPlayer natives (load_virtbase + recovered offsets) */
  Unity_initJni                  = (fn_initJni) UNITY_RESOLVE(unity_mod, OFF_initJni);
  Unity_nativeRecreateGfxState   = (fn_gfxstate)UNITY_RESOLVE(unity_mod, OFF_nativeRecreateGfxState);
  Unity_nativeSendSurfaceChanged = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeSendSurfaceChangedEvent);
  Unity_nativeRender             = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeRender);
  Unity_nativeInjectEvent        = (fn_inject)  UNITY_RESOLVE(unity_mod, OFF_nativeInjectEvent);
  Unity_nativeResume             = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeResume);
  Unity_nativeFocusChanged       = (fn_vz)      UNITY_RESOLVE(unity_mod, OFF_nativeFocusChanged);
  Unity_nativeDone               = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeDone);
  Unity_nativeApplicationUnload  = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeApplicationUnload);
  debugPrintf("[boot] entry points resolved (initJni=%p render=%p)\n",
              (void *)Unity_initJni, (void *)Unity_nativeRender);

  /* re-assert the guard right before handing control to the engine, so no
   * intervening libnx/jni setup left tpidr in an unexpected state */
  install_bionic_tls(main_tls);

  /* drive the lifecycle the Java UnityPlayer would */
  extern void *fake_env, *fake_unityplayer_thiz, *fake_context_obj, *fake_surface_obj;
  extern void *fake_vm;

  /* Call libunity's real JNI_OnLoad(fake_vm) FIRST. It runs jni::Initialize(),
   * which caches the JavaVM into libunity's internal JNI manager; without this
   * ScopedJNI/LocalScope inside initJni get a NULL JNIEnv and crash. It also
   * AttachCurrentThread()s and RegisterNatives() for each subsystem (our fake
   * env handles FindClass/RegisterNatives as safe no-ops). */
  {
    typedef int (*fn_jnionload)(void *vm, void *reserved);
    fn_jnionload Unity_JNI_OnLoad = (fn_jnionload)UNITY_RESOLVE(unity_mod, OFF_JNI_OnLoad);
    debugPrintf("[boot] calling JNI_OnLoad(fake_vm)...\n");
    int jver = Unity_JNI_OnLoad(fake_vm, NULL);
    debugPrintf("[boot] JNI_OnLoad returned 0x%x\n", jver);
  }

  /* Register the JavaVM with the il2cpp runtime. il2cpp caches the VM in a
   * global it later checks; without it, il2cpp logs "Java VM not initialized"
   * and every managed AndroidJNI / AndroidJavaObject call (the Twitter SDK +
   * the SWIG-wrapped AppUtil module the first scene initializes) fails, hanging
   * scene load.
   *
   * We do NOT call libil2cpp's JNI_OnLoad: its first action is a log via
   * __android_log_print, whose GOT slot in libil2cpp is mis-bound (resolves to
   * a heap address -> Instruction Abort). Its only *essential* effects are two
   * global stores (verified by disassembling THIS PvZ 62f1c1 libil2cpp's
   * JNI_OnLoad @ 0x188e168): cache the VM at il2cpp+0x3c09c18, and store the JNI
   * handler fn-ptr (il2cpp+0x188e1ac, which the reg-fn @0x18ea05c writes) at
   * il2cpp+0x3c0abe8. Both land in PvZ's RW segment. Replicate the two stores. */
  {
    uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
#if KB_HAVE_IL2CPP_VM
    /* Fruit Ninja values, derived by disassembling THIS libil2cpp's
     * JNI_OnLoad @0x1588e1c -- see nx_patch_killerbean.h. Both targets are
     * in libil2cpp's .bss. These are NOT PvZ's addresses. */
    *(void **)(b + KB_IL2CPP_VM_GLOBAL)    = fake_vm;
    *(void **)(b + KB_IL2CPP_HANDLER_SLOT) = (void *)(b + KB_IL2CPP_HANDLER_FN);
#endif
    debugPrintf("[boot] il2cpp JavaVM global set (vm=%p)\n", fake_vm);
#if KB_HAVE_ISINST_GUARD
    /* Guard il2cpp's assignability check against a NULL klass (round 119).
     * DialogueConfig.m_dialoguePieces holds an Il2CppClass* instead of an array
     * and the check faults reading klass+0x135.
     *
     * Applied HERE, not at "[boot] loaded libil2cpp": at that point the module's
     * text is not yet mapped, and merely READING the site to verify it faulted
     * (round 120 -- far == il2cpp_base + 0x159dfcc, exactly this address). This
     * is the same place the JavaVM globals and Time hooks are written, which is
     * proven safe. */
    { volatile uint32_t *site = (uint32_t *)(b + KB_IL2CPP_ISINST_AND);
      if (!nx_addr_readable((uintptr_t)site)) {
        debugPrintf("[boot] SKIP IsInst guard: libil2cpp+0x%x not mapped yet\n",
                    (unsigned)KB_IL2CPP_ISINST_AND);
      } else if (site[0] == KB_IL2CPP_ISINST_AND_OLD) {
        uint32_t guard = KB_IL2CPP_ISINST_GUARD;
        so_patch_code((void *)site, &guard, sizeof guard);
        so_flush_caches(&il2cpp_mod);
        debugPrintf("[boot] il2cpp IsInst null-klass guard installed @libil2cpp+0x%x\n",
                    (unsigned)KB_IL2CPP_ISINST_AND);
      } else if (site[0] == KB_IL2CPP_ISINST_GUARD) {
        debugPrintf("[boot] il2cpp IsInst guard already installed\n");
      } else {
        debugPrintf("[boot] SKIP IsInst guard @libil2cpp+0x%x: 0x%08x != 0x%08x\n",
                    (unsigned)KB_IL2CPP_ISINST_AND, site[0],
                    (unsigned)KB_IL2CPP_ISINST_AND_OLD);
      }
    }
#endif

#if KB_HAVE_LIVENESS_GUARD
    /* Installed HERE for the same reason as the r119 guard above: this is the
     * point where libil2cpp's text is known mapped (round 120).
     *
     * NOT hook_arm64(): it stores straight into module text, which so_finalize
     * mapped RX. Build its 16-byte thunk and write it through so_patch_code's
     * writable alias instead. */
    { volatile uint32_t *site = (uint32_t *)(b + KB_IL2CPP_LIVENESS_ADD);
      if (!nx_addr_readable((uintptr_t)site)) {
        debugPrintf("[boot] SKIP liveness guard: libil2cpp+0x%x not mapped yet\n",
                    (unsigned)KB_IL2CPP_LIVENESS_ADD);
      } else if (site[0] == 0x58000051u) {
        debugPrintf("[boot] liveness guard already installed\n");
      } else if (site[0] != KB_LIVENESS_PROLOGUE[0] || site[1] != KB_LIVENESS_PROLOGUE[1] ||
                 site[2] != KB_LIVENESS_PROLOGUE[2] || site[3] != KB_LIVENESS_PROLOGUE[3]) {
        /* Loud, and patch NOTHING -- a moved add_process_object must not be
         * half-overwritten. Report all four words so the site can be re-derived
         * from the log alone. */
        debugPrintf("[boot] SKIP liveness guard @libil2cpp+0x%x: prologue is "
                    "%08x %08x %08x %08x, expected %08x %08x %08x %08x\n",
                    (unsigned)KB_IL2CPP_LIVENESS_ADD,
                    site[0], site[1], site[2], site[3],
                    KB_LIVENESS_PROLOGUE[0], KB_LIVENESS_PROLOGUE[1],
                    KB_LIVENESS_PROLOGUE[2], KB_LIVENESS_PROLOGUE[3]);
      } else {
        uint32_t thunk[4];
        const uint64_t dst = (uint64_t)(uintptr_t)&fn_liveness_guard;
        g_fn_liveness_body = (uint64_t)(b + KB_IL2CPP_LIVENESS_BODY);
        thunk[0] = 0x58000051u;   /* ldr x17, #8  */
        thunk[1] = 0xd61f0220u;   /* br  x17      */
        memcpy(&thunk[2], &dst, sizeof dst);
        if (so_patch_code((void *)site, thunk, sizeof thunk) == 0) {
          so_flush_caches(&il2cpp_mod);
          debugPrintf("[boot] il2cpp liveness typeHierarchy guard installed "
                      "@libil2cpp+0x%x -> %p (body 0x%llx)\n",
                      (unsigned)KB_IL2CPP_LIVENESS_ADD, (void *)(uintptr_t)dst,
                      (unsigned long long)g_fn_liveness_body);
        } else {
          g_fn_liveness_body = 0;
          debugPrintf("[boot] liveness guard: so_patch_code FAILED, not installed\n");
        }
      }
    }
#endif

  /* Round 146, from the ACPC port. il2cpp_gc_set_mode(1) is
   * IL2CPP_GC_MODE_ENABLED -- disassembled at libil2cpp+0x1593660: mode 1 does
   * `if (GC_is_disabled()) GC_enable()` then
   * `set_disable_automatic_collection(false)`. It is a defensive re-assert, not
   * a change: our logs already show collections running. Cheap insurance that
   * nothing has left automatic collection switched off, via a real exported
   * symbol so there is no offset to go stale. */
  { typedef void (*fn_set_mode)(int);
    fn_set_mode set_mode = (fn_set_mode)so_find_addr_rx(&il2cpp_mod, "il2cpp_gc_set_mode");
    if (set_mode) { set_mode(1); debugPrintf("[gc] il2cpp_gc_set_mode(ENABLED)\n"); } }

#if KB_GC_DISABLE
    /* Round 141 -- the decisive experiment, not a fix.
     *
     * Every guard since r119 has been placed where the bad value is READ. If the
     * corruption is premature reclamation -- the collector freeing something
     * still reachable, and the memory being handed out again -- then it is
     * created in the sweep and no amount of validation at the read side touches
     * it. Turning collection off answers that in one session:
     *
     *   corruption GONE  -> the collector is freeing live objects. The cause is
     *                       almost certainly that Boehm never gets a snapshot of
     *                       paused threads' registers and stack pointers: on
     *                       Android its SIG_SUSPEND handler runs ON each mutator
     *                       and spills them, and Horizon has no POSIX signals, so
     *                       we pause with svcSetThreadActivity and that handler
     *                       never runs.
     *   corruption STAYS -> the GC is exonerated after 20 rounds of suspicion,
     *                       and the hunt moves to the wild write we already have
     *                       direct evidence of (r134 boot 1: `str x1,[x0]` with
     *                       x0 = UTF-16 text).
     *
     * il2cpp_gc_disable is a real exported symbol, so this needs no derived
     * offset and cannot go stale. Memory grows unbounded while it is on -- the
     * pool is 1 GB and the game's live set is far smaller, so a few minutes of
     * play is fine, but do not ship with this set. */
    { uintptr_t f = so_find_addr_rx(&il2cpp_mod, "il2cpp_gc_disable");
      if (f) { ((void (*)(void))f)();
        debugPrintf("[boot] *** GC DISABLED (KB_GC_DISABLE): no collection will "
                    "run this session. Diagnostic only -- memory grows. ***\n"); }
      else debugPrintf("[boot] KB_GC_DISABLE set but il2cpp_gc_disable not found\n"); }
#endif


#if KB_HAVE_TIME_HOOKS
    nx_boot_il2cpp_hacks();   /* Time.get_* hooks -- REQUIRES FN-derived offsets */
#else
    /* DISABLED. nx_boot_il2cpp_hacks() patches CODE at six hardcoded
     * libil2cpp offsets that belong to PvZ Fusion, not this game. Running
     * it here would rewrite six unrelated functions inside Fruit Ninja's
     * libil2cpp. Re-derive the six UnityEngine.Time getters against this
     * game's global-metadata.dat before enabling (PORTING sec 6). */
#endif
  }

  /* (Firebase native libs are not loaded; their JNI_OnLoad is neither needed nor
   * safe to call -- see the boot-time note above. The managed SDK's native calls
   * are answered by firebase_stub.c through dlsym_fake.) */

  debugPrintf("[boot] calling initJni...\n");
  Unity_initJni(fake_env, fake_unityplayer_thiz, fake_context_obj);
  debugPrintf("[boot] initJni returned; nativeRecreateGfxState...\n");
  Unity_nativeRecreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  nx_gpu_probe();   /* hardware-vs-software ASTC, one line in debug.log */
  debugPrintf("[boot] gfx state created; sendSurfaceChanged...\n");
  Unity_nativeSendSurfaceChanged(fake_env, fake_unityplayer_thiz);
  debugPrintf("[boot] surface change sent; resuming + focusing player loop\n");

  /* CRITICAL: on Android the Unity player loop only advances Update/coroutines/
   * animation when the app is RESUMED and FOCUSED. The Java UnityPlayer drives
   * this from onResume()/onWindowFocusChanged(). We had been calling only
   * initJni + gfx + render, so the engine stayed paused: it loaded the boot
   * scene and ran Awake/Start once (hence Firebase init), then rendered a frozen
   * frame forever without ticking a single Update or coroutine -- which is why
   * StartInitializer.InitUpdate was never called. Issue the resume + focus
   * transitions the lifecycle normally would before the render loop. */
  Unity_nativeResume(fake_env, fake_unityplayer_thiz);
  Unity_nativeFocusChanged(fake_env, fake_unityplayer_thiz, 1 /* hasFocus */);
  debugPrintf("[boot] resumed + focus=true; entering render loop\n");
#if KB_HAVE_TIME_FIX
#if KB_HAVE_TIME_FIX
  nx_install_time_fix();   /* hook Update + clock thread before the first nativeRender */
#else
  /* DISABLED: nx_install_time_fix() uses PvZ libunity offsets (0x4e2198 /
   * 0x4e21bc / 0x4e2794). Enable only after re-deriving all three. */
#endif
#endif

  diag_thread_register(NULL, 0);
  diag_set_name(NULL, "NX_UIMain");   // the thread that drives nativeRender
  /* Watchdog re-enabled (fbstub88) with the snapshot-ordering fix: stacks are
   * now walked while the target thread is PAUSED (the fbstub86 self-crash came
   * from resuming first and walking a live stack). 6s thresholds. Its job now:
   * catch the first-present hang and dump the thread blocked in eglSwapBuffers. */
  diag_watchdog_start();

  int frame = 0;
  while (appletMainLoop() && !jni_quit_requested) {
    diag_frame(frame);   // heartbeat: lets the watchdog see progress (or its absence)
    nx_time_tick();      // advance our managed-Time clock once per frame
    /* fbstub42: the engine clock is now fixed at its true source by the
     * TimeManager::Update entry hook (installed at boot, see nx_install_time_fix):
     * Update is re-driven each frame with newTime = GetTimeSinceStartup(), so all
     * deltaTime variants advance and the PreloadManager can integrate. No per-frame
     * field poking needed here. */
    android_native_update_mode();
    android_native_feed_hid((uint8_t (*)(void*,void*,void*,int))Unity_nativeInjectEvent,
                            fake_env, fake_unityplayer_thiz);
    /* Round 157: the SAME pointer state, published to the other input backend.
     * feed_hid fills UnityEngine.Input (legacy, what uGUI's
     * StandaloneInputModule reads); this fills UnityEngine.InputSystem (what
     * LeanInput and InputSystemUIInputModule read). Android feeds both from one
     * MotionEvent; until now we fed one. Must run AFTER feed_hid, which is what
     * refreshes the snapshot both of them read, and BEFORE nativeRender, so the
     * events are in the queue when the engine pumps the frame. */
    { extern void nx_newinput_feed(void); nx_newinput_feed(); }
    if (!Unity_nativeRender(fake_env, fake_unityplayer_thiz)) break;
    if (frame == 0) {
      /* fbstub42: install the native engine-clock fix first thing. Drives
       * TimeManager::Update with a live newTime so deltaTime / m_Time advance for
       * native readers (PreloadManager), unblocking async scene loads. */
      /* nx_install_time_fix() moved before the render loop (installs too late here:
       * PvZ's first nativeRender (their symptom) blocks on the scene load and never returns). */
      /* Time.get_* hooks are installed at boot (nx_boot_il2cpp_hacks, right after
       * the JavaVM global is set). This call is a self-skipping backstop only. */
#if KB_HAVE_TIME_HOOKS
      nx_boot_il2cpp_hacks();
#endif
    }
    if (frame < 5 || (frame % 120) == 0) debugPrintf("[boot] frame %d rendered\n", frame);
#if KB_HAVE_FINISH_PROBE
    if (frame == 90 || frame == 300 || frame == 600 || frame == 1200)
      nx_probe_finish((uintptr_t)il2cpp_mod.load_virtbase);
#endif
    frame++;
  }

  /* Commit any sensitivity change made inside the last 3 seconds -- the pointer
   * normally debounces its own save, so quitting quickly after a D-pad tweak
   * would otherwise lose it. */
  android_native_input_shutdown();

  Unity_nativeApplicationUnload(fake_env, fake_unityplayer_thiz);
  Unity_nativeDone(fake_env, fake_unityplayer_thiz);

  opensles_shutdown();
  SDL_Quit();
  socketExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
