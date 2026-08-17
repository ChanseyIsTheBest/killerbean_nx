/* nx_newinput.c -- feed UnityEngine.InputSystem (the "new" input system)
 * ---------------------------------------------------------------------------
 *
 * WHY THIS EXISTS
 *
 * We inject touches through UnityPlayer.nativeInjectEvent, which is the LEGACY
 * Android input path. It fills UnityEngine.Input, which is what uGUI's
 * StandaloneInputModule reads -- and that is why the weapon-store buttons work.
 *
 * It does NOT fill UnityEngine.InputSystem, which has its own device model and
 * its own native event queue. On Android the engine feeds BOTH backends from a
 * single MotionEvent. We were feeding one. Two things on the level map read the
 * other one:
 *
 *   LeanInput.GetTouchCount()  -> EnhancedTouch.Touch.activeTouches
 *   level3's second EventSystem -> InputSystemUIInputModule
 *
 * which is exactly why the map's Lean nodes and its uGUI buttons were both dead
 * while every other screen worked. Round 154 bridged LeanInput by hooking it;
 * that was the right idea at the wrong layer. This is the layer.
 *
 * HOW
 *
 * No code patching at all. libil2cpp exports the whole il2cpp_* runtime API, so
 * we can create the device the same way managed code would and queue events
 * through the same entry point the Android backend uses:
 *
 *   InputSystem.AddDevice("Touchscreen", null, null)   -> InputDevice
 *   InputDevice.deviceId                               -> ushort for the header
 *   NativeInputSystem::QueueInputEvent(IntPtr)         -> the native queue
 *
 * EVERYTHING BELOW IS DERIVED FROM THIS BUILD -- see
 * tools/newinput_derivation_raw.txt for the dump.cs line numbers and the
 * disassembly that produced each constant. Nothing here is carried over from
 * another game or another Unity version.
 *
 *   NativeInputEvent   structSize 20
 *     +0x00 int    type          FourCC
 *     +0x04 ushort sizeInBytes
 *     +0x06 ushort deviceId
 *     +0x08 double time
 *     +0x10 int    eventId
 *   StateEvent
 *     +0x00 baseEvent (20)
 *     +0x14 FourCC stateFormat
 *     +0x18 stateData[]
 *     Type = 1398030676 = 'STAT'
 *   TouchscreenState              Format = 'TSCR' (get_Format @ 0x1E7BD78 loads
 *                                 'T','S','C','R'), MaxTouches = 10
 *     +0x00 primaryTouchData[56]
 *     +0x38 touchData[560]        (kTouchDataOffset = 56)
 *   TouchState                    kSizeInBytes = 56
 *     +0x00 int touchId    +0x04 Vector2 position  +0x0C Vector2 delta
 *     +0x14 float pressure +0x18 Vector2 radius    +0x20 byte phaseId
 *     +0x21 tapCount +0x22 displayIndex +0x23 flags +0x24 uint updateStepCount
 *     +0x28 double startTime  +0x30 Vector2 startPosition
 *   TouchPhase  None 0 Began 1 Moved 2 Ended 3 Canceled 4 Stationary 5
 */

#include <switch.h>
#include <string.h>
#include <stdint.h>
#include "util.h"
#include "config.h"
#include "so_util.h"

#if KB_NEW_INPUT_SYSTEM

/* ---- il2cpp runtime API (all exported from libil2cpp; no patching) -------- */
typedef void *Il2CppDomain, *Il2CppAssembly, *Il2CppImage, *Il2CppClass,
             *Il2CppObject, *Il2CppString, *Il2CppMethod, *Il2CppThread;

static Il2CppDomain   (*p_domain_get)(void);
static Il2CppAssembly (*p_domain_assembly_open)(Il2CppDomain, const char *);
static Il2CppImage    (*p_assembly_get_image)(Il2CppAssembly);
static Il2CppClass    (*p_class_from_name)(Il2CppImage, const char *, const char *);
static Il2CppMethod   (*p_class_get_method_from_name)(Il2CppClass, const char *, int);
static Il2CppObject   (*p_runtime_invoke)(Il2CppMethod, void *, void **, Il2CppObject *);
/* NOTE: the 4th parameter is Il2CppException** -- an OUT pointer. It is typed
 * as Il2CppObject (a void*) above only because that is what this file's
 * opaque-handle typedefs give us; &exc below is a void** and the ABI matches. */
static Il2CppString   (*p_string_new)(const char *);
static void          *(*p_object_unbox)(Il2CppObject);
static Il2CppThread   (*p_thread_current)(void);
static Il2CppThread   (*p_thread_attach)(Il2CppDomain);
static void          *(*p_resolve_icall)(const char *);

/* NativeInputSystem::QueueInputEvent(IntPtr) and get_currentTime(), resolved as
 * ICALLS rather than as managed thunks: an icall pointer has the plain C
 * signature libunity registered, with no trailing MethodInfo* to get wrong. */
static void   (*p_queue_input_event)(void *);
static double (*p_current_time)(void);

static int   g_ni_ready = 0;      /* device created, queue resolved */
static int   g_ni_failed = 0;     /* gave up; stop retrying every frame */
static int   g_ni_device_id = 0;

static int resolve_api(void) {
  extern so_module il2cpp_mod;
  /* so_find_addr_rx() calls fatal_error() when a symbol is absent, which would
   * take the whole port down over an optional feature. so_try_find_addr_rx()
   * returns 0 instead, which is what the check below is actually for. */
#define R(var, name)                                                          \
  do {                                                                        \
    var = (void *)so_try_find_addr_rx(&il2cpp_mod, name);                     \
    if (!var) { debugPrintf("[ni] missing export %s\n", name); return 0; }    \
  } while (0)
  R(p_domain_get,                 "il2cpp_domain_get");
  R(p_domain_assembly_open,       "il2cpp_domain_assembly_open");
  R(p_assembly_get_image,         "il2cpp_assembly_get_image");
  R(p_class_from_name,            "il2cpp_class_from_name");
  R(p_class_get_method_from_name, "il2cpp_class_get_method_from_name");
  R(p_runtime_invoke,             "il2cpp_runtime_invoke");
  R(p_string_new,                 "il2cpp_string_new");
  R(p_object_unbox,               "il2cpp_object_unbox");
  R(p_thread_current,             "il2cpp_thread_current");
  R(p_thread_attach,              "il2cpp_thread_attach");
  R(p_resolve_icall,              "il2cpp_resolve_icall");
#undef R
  return 1;
}

/* Create the Touchscreen device exactly as managed code would.
 * Returns 1 once g_ni_ready is set. Safe to call repeatedly: the InputSystem is
 * initialised from RuntimeInitializeOnLoad, so the first few attempts can fail
 * before the managed side is up, and we simply try again next frame. */
/* ROUND 158 -- WHERE THIS RUNS IS THE WHOLE POINT.
 *
 * Round 157 called this from the render loop on frame 0 and died instantly:
 *
 *   pc=libil2cpp+0x91947c  ldrb w8,[x0,#0x132]  x0 = 0  far = 0x132
 *
 * that is Class::Init(NULL) inside the il2cpp runtime -- reached before the
 * managed world was ready to answer any of these queries. Retrying "until it
 * works" cannot fix that, because the first attempt is already fatal.
 *
 * So this is no longer called from the render loop at all. It is called from
 * kb_lean_get_touch_count(), i.e. from inside a managed Update(), which
 * guarantees every precondition by construction: the runtime is initialised,
 * this thread is attached and executing managed code, and the InputSystem
 * assembly is loaded (LeanTouch is what called us, and it references it).
 *
 * Each step logs BEFORE it runs, once. If this still faults, the last [ni]
 * line in the log names the exact call that did it -- one test, not five. */
static int nx_ni_try_init(void) {
  if (g_ni_ready)  return 1;
  if (g_ni_failed) return 0;

  static unsigned attempts = 0;
  const int first = (attempts == 0);
#define NI_STEP(msg) do { if (first) debugPrintf("[ni] init: " msg "\n"); } while (0)
  if (++attempts > 600) {          /* ~10 s of managed frames, then stop */
    g_ni_failed = 1;
    debugPrintf("[ni] *** giving up after %u attempts -- the new input system "
                "was never reachable; nothing is being fed to it ***\n", attempts);
    return 0;
  }

  NI_STEP("resolving il2cpp exports");
  if (!p_domain_get && !resolve_api()) { g_ni_failed = 1; return 0; }

  NI_STEP("il2cpp_domain_get");
  Il2CppDomain dom = p_domain_get();
  if (!dom) return 0;

  NI_STEP("il2cpp_thread_current / attach");
  if (!p_thread_current()) p_thread_attach(dom);

  NI_STEP("il2cpp_domain_assembly_open(\"Unity.InputSystem\")");
  Il2CppAssembly asm_ = p_domain_assembly_open(dom, "Unity.InputSystem");
  if (!asm_) return 0;                       /* not loaded yet */
  NI_STEP("il2cpp_assembly_get_image");
  Il2CppImage img = p_assembly_get_image(asm_);
  if (!img) return 0;

  NI_STEP("il2cpp_class_from_name(InputSystem / InputDevice)");
  Il2CppClass cls_is = p_class_from_name(img, "UnityEngine.InputSystem", "InputSystem");
  Il2CppClass cls_dev = p_class_from_name(img, "UnityEngine.InputSystem", "InputDevice");
  if (!cls_is || !cls_dev) return 0;

  /* AddDevice(string layout, string name, string variants) -- the 3-arg form is
   * the real signature; the C# defaults are compile-time only. */
  NI_STEP("il2cpp_class_get_method_from_name(AddDevice,3 / get_deviceId,0)");
  Il2CppMethod m_add = p_class_get_method_from_name(cls_is, "AddDevice", 3);
  Il2CppMethod m_id  = p_class_get_method_from_name(cls_dev, "get_deviceId", 0);
  if (!m_add || !m_id) {
    debugPrintf("[ni] AddDevice/get_deviceId not found -- layout differs from "
                "the dump; not proceeding\n");
    g_ni_failed = 1;
    return 0;
  }

  NI_STEP("il2cpp_string_new(\"Touchscreen\")");
  Il2CppObject exc = NULL;
  void *args[3];
  args[0] = p_string_new("Touchscreen");
  args[1] = NULL;
  args[2] = NULL;
  NI_STEP("il2cpp_runtime_invoke(InputSystem.AddDevice)");
  Il2CppObject dev = p_runtime_invoke(m_add, NULL, args, &exc);
  if (exc || !dev) {
    debugPrintf("[ni] InputSystem.AddDevice(\"Touchscreen\") returned %p exc=%p "
                "-- retrying\n", dev, exc);
    return 0;
  }

  exc = NULL;
  Il2CppObject boxed = p_runtime_invoke(m_id, dev, NULL, &exc);
  if (exc || !boxed) { debugPrintf("[ni] get_deviceId failed\n"); return 0; }
  g_ni_device_id = *(int *)p_object_unbox(boxed);

  NI_STEP("il2cpp_resolve_icall(QueueInputEvent)");
  p_queue_input_event = (void (*)(void *))p_resolve_icall(
      "UnityEngineInternal.Input.NativeInputSystem::QueueInputEvent(System.IntPtr)");
  p_current_time = (double (*)(void))p_resolve_icall(
      "UnityEngineInternal.Input.NativeInputSystem::get_currentTime()");
  if (!p_current_time)
    debugPrintf("[ni] WARNING: get_currentTime icall did not resolve -- events "
                "will carry time=0. If the InputSystem drops events outside its "
                "update window, that is the first thing to suspect.\n");
  if (!p_queue_input_event) {
    debugPrintf("[ni] *** QueueInputEvent icall did not resolve -- the device "
                "exists but nothing can be queued to it ***\n");
    g_ni_failed = 1;
    return 0;
  }

#undef NI_STEP
  g_ni_ready = 1;
  debugPrintf("[ni] Touchscreen device created: deviceId=%d queue=%p time=%p\n",
              g_ni_device_id, (void *)p_queue_input_event, (void *)p_current_time);
  return 1;
}

/* ---- event construction --------------------------------------------------- */
#define NI_FOURCC(a,b,c,d) (((unsigned)(a)<<24)|((unsigned)(b)<<16)|((unsigned)(c)<<8)|(unsigned)(d))
#define NI_STATE_EVENT     NI_FOURCC('S','T','A','T')   /* 1398030676 */
#define NI_TOUCHSCREEN_FMT NI_FOURCC('T','S','C','R')

#define NI_TOUCHSTATE_SIZE 56
#define NI_MAX_TOUCHES     10
#define NI_TOUCHDATA_OFF   56
#define NI_TSCR_SIZE       (NI_TOUCHDATA_OFF + NI_TOUCHSTATE_SIZE * NI_MAX_TOUCHES)  /* 616 */
#define NI_HDR_SIZE        24                              /* 20 + FourCC */
#define NI_EVENT_SIZE      (NI_HDR_SIZE + NI_TSCR_SIZE)    /* 640 */

enum { NI_PHASE_NONE = 0, NI_PHASE_BEGAN = 1, NI_PHASE_MOVED = 2,
       NI_PHASE_ENDED = 3, NI_PHASE_CANCELED = 4, NI_PHASE_STATIONARY = 5 };

static void ni_write_touch(uint8_t *p, int id, float x, float y,
                           float dx, float dy, int phase, double t,
                           float sx, float sy) {
  memset(p, 0, NI_TOUCHSTATE_SIZE);
  *(int32_t *)(p + 0x00) = id;
  *(float *)  (p + 0x04) = x;      *(float *)(p + 0x08) = y;
  *(float *)  (p + 0x0C) = dx;     *(float *)(p + 0x10) = dy;
  *(float *)  (p + 0x14) = 1.0f;                       /* pressure */
  *(float *)  (p + 0x18) = 8.0f;   *(float *)(p + 0x1C) = 8.0f;   /* radius */
  p[0x20] = (uint8_t)phase;
  p[0x21] = 1;                                          /* tapCount */
  p[0x22] = 0;                                          /* displayIndex */
  p[0x23] = 0;                                          /* flags */
  *(double *)(p + 0x28) = t;                            /* startTime */
  *(float *) (p + 0x30) = sx;      *(float *)(p + 0x34) = sy;
}

/* One full-device state event per feed. The Android backend does the same: it
 * republishes the whole TouchscreenState rather than per-touch deltas, which
 * also means a touch that stops being listed is correctly seen as gone. */
/* Called from managed context (the LeanInput replacement). Safe place, and the
 * only place, to touch the il2cpp runtime API. */
void nx_newinput_managed_tick(void) { (void)nx_ni_try_init(); }

/* Called from the render loop. Queues only; never initialises. */
void nx_newinput_feed(void) {
  if (!g_ni_ready) return;

  extern int nx_touch_count(void);
  extern int nx_touch_get(int i, int *id, float *x, float *y, int *set);

  /* Persistent touch table. ROUND 158 AUDIT: the first version derived phase
   * purely from what the snapshot happened to contain this feed, which had two
   * defects worth more than the code they saved.
   *
   * (1) A pointer that vanished from the snapshot without ever being reported
   *     with set==0 would never get an Ended, and the InputSystem would hold it
   *     down forever. The snapshot keeps a released pointer for exactly one
   *     feed, so any hiccup that skipped a feed leaked a stuck finger.
   * (2) A DOWN and its UP arriving in the SAME feed put two entries with the
   *     same id in the snapshot, and the old loop wrote both into different
   *     touch slots -- one Began and one Ended for one finger, in one event.
   *     Measurements say taps span 4-11 frames so it is rare, but "rare and
   *     silently malformed" is the exact failure profile this port keeps
   *     getting bitten by.
   *
   * So: own the state here. Dedupe the snapshot by id (first wins), diff it
   * against the table, and derive Ended from a pointer's ABSENCE rather than
   * from being told. */
  static uint8_t ev[NI_EVENT_SIZE];
  static int     tb_live[NI_MAX_TOUCHES], tb_id[NI_MAX_TOUCHES];
  static float   tb_x[NI_MAX_TOUCHES], tb_y[NI_MAX_TOUCHES];
  static float   tb_sx[NI_MAX_TOUCHES], tb_sy[NI_MAX_TOUCHES];
  static double  tb_st[NI_MAX_TOUCHES];
  static int     had_touches = 0;

  /* ---- gather this feed's pointers, deduped ---- */
  int cur_id[NI_MAX_TOUCHES], cur_n = 0;
  float cur_x[NI_MAX_TOUCHES], cur_y[NI_MAX_TOUCHES];
  const int n = nx_touch_count();
  for (int i = 0; i < n && cur_n < NI_MAX_TOUCHES; i++) {
    int id = 0, set = 0; float x = 0.0f, y = 0.0f;
    if (!nx_touch_get(i, &id, &x, &y, &set)) continue;
    if (!set) continue;                       /* released: handled by absence */
    int dup = 0;
    for (int k = 0; k < cur_n; k++) if (cur_id[k] == id) { dup = 1; break; }
    if (dup) continue;
    cur_id[cur_n] = id; cur_x[cur_n] = x; cur_y[cur_n] = y; cur_n++;
  }

  const double now = p_current_time ? p_current_time() : 0.0;

  memset(ev, 0, sizeof ev);
  *(int32_t  *)(ev + 0x00) = (int32_t)NI_STATE_EVENT;
  *(uint16_t *)(ev + 0x04) = (uint16_t)NI_EVENT_SIZE;
  *(uint16_t *)(ev + 0x06) = (uint16_t)g_ni_device_id;
  *(double   *)(ev + 0x08) = now;
  *(int32_t  *)(ev + 0x10) = 0;                     /* eventId: runtime fills */
  *(int32_t  *)(ev + 0x14) = (int32_t)NI_TOUCHSCREEN_FMT;

  uint8_t *state   = ev + NI_HDR_SIZE;
  uint8_t *primary = state;
  uint8_t *touches = state + NI_TOUCHDATA_OFF;

  int wrote = 0;

  /* ---- pointers still down: Began / Moved / Stationary ---- */
  for (int i = 0; i < cur_n && wrote < NI_MAX_TOUCHES; i++) {
    int slot = -1;
    for (int k = 0; k < NI_MAX_TOUCHES; k++)
      if (tb_live[k] && tb_id[k] == cur_id[i]) { slot = k; break; }

    int phase;
    if (slot < 0) {
      for (int k = 0; k < NI_MAX_TOUCHES; k++) if (!tb_live[k]) { slot = k; break; }
      if (slot < 0) continue;                 /* table full: drop, do not corrupt */
      phase = NI_PHASE_BEGAN;
      tb_live[slot] = 1; tb_id[slot] = cur_id[i];
      tb_x[slot] = cur_x[i]; tb_y[slot] = cur_y[i];
      tb_sx[slot] = cur_x[i]; tb_sy[slot] = cur_y[i]; tb_st[slot] = now;
    } else {
      phase = (cur_x[i] != tb_x[slot] || cur_y[i] != tb_y[slot])
                ? NI_PHASE_MOVED : NI_PHASE_STATIONARY;
    }
    const float dx = cur_x[i] - tb_x[slot], dy = cur_y[i] - tb_y[slot];
    tb_x[slot] = cur_x[i]; tb_y[slot] = cur_y[i];

    ni_write_touch(touches + wrote * NI_TOUCHSTATE_SIZE, cur_id[i],
                   cur_x[i], cur_y[i], dx, dy, phase,
                   tb_st[slot], tb_sx[slot], tb_sy[slot]);
    if (wrote == 0)
      ni_write_touch(primary, cur_id[i], cur_x[i], cur_y[i], dx, dy, phase,
                     tb_st[slot], tb_sx[slot], tb_sy[slot]);
    wrote++;
  }

  /* ---- pointers that are gone: Ended, derived from absence ---- */
  for (int k = 0; k < NI_MAX_TOUCHES && wrote < NI_MAX_TOUCHES; k++) {
    if (!tb_live[k]) continue;
    int still = 0;
    for (int i = 0; i < cur_n; i++) if (cur_id[i] == tb_id[k]) { still = 1; break; }
    if (still) continue;
    ni_write_touch(touches + wrote * NI_TOUCHSTATE_SIZE, tb_id[k],
                   tb_x[k], tb_y[k], 0.0f, 0.0f, NI_PHASE_ENDED,
                   tb_st[k], tb_sx[k], tb_sy[k]);
    if (wrote == 0)
      ni_write_touch(primary, tb_id[k], tb_x[k], tb_y[k], 0.0f, 0.0f,
                     NI_PHASE_ENDED, tb_st[k], tb_sx[k], tb_sy[k]);
    tb_live[k] = 0;
    wrote++;
  }

  if (!wrote && !had_touches) return;   /* idle: nothing to publish */
  had_touches = (cur_n > 0);

  { static unsigned logged = 0;
    if (logged < 30) { logged++;
      debugPrintf("[ni] queue STAT dev=%d slots=%d t=%.3f  t0: id=%d (%.0f,%.0f) "
                  "phase=%d\n", g_ni_device_id, wrote, now,
                  *(int32_t *)(primary + 0x00),
                  (double)*(float *)(primary + 0x04),
                  (double)*(float *)(primary + 0x08),
                  (int)primary[0x20]); } }

  p_queue_input_event(ev);
}

int nx_newinput_ready(void) { return g_ni_ready; }

#else   /* !KB_NEW_INPUT_SYSTEM */

void nx_newinput_feed(void) { }
void nx_newinput_managed_tick(void) { }
int  nx_newinput_ready(void) { return 0; }

#endif
