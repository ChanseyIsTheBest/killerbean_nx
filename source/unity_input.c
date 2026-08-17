/* unity_input.c -- fake MotionEvent / KeyEvent backing nativeInjectEvent.
 * See unity_input.h for the method surface (taken from libunity.so) and wiring.
 * Style mirrors unity_jni.c. Host-compilable plain C (no libnx). */

#include <string.h>
#include <time.h>
#include "unity_input.h"

struct FakeID { uint32_t tag; char cls[96]; char name[64]; char sig[160]; };

enum { UI_TAG = 0x55494531 /*'UIE1'*/, KIND_MOTION, KIND_KEY };
/* Separate tag for the fake InputDevice handed back by InputEvent.getDevice().
 * See unity_inputdevice() below for why a non-null device matters. */
enum { UID_TAG = 0x55494431 /*'UID1'*/ };
typedef struct { uint32_t tag; } UDevice;
static UDevice g_dev = { UID_TAG };

typedef struct {
  uint32_t tag; int kind;
  int   action;                 /* raw action (masked | ptrindex<<8)        */
  int   count;
  int   ids[UI_MAX_POINTERS];
  float xs [UI_MAX_POINTERS];
  float ys [UI_MAX_POINTERS];
  int   keycode;                /* KeyEvent                                 */
  int64_t time_ms;
  int64_t down_ms;   /* time of the gesture's ACTION_DOWN (see getDownTime) */
} UEvent;

/* single reused handle -- injection is synchronous */
static UEvent g_ev;

/* ---- touch diagnostics (see unity_input.h) ---- */
int (*input_log_fn)(char *fmt, ...) = 0;
int   input_log_budget = 0;
#define ILOG(...) do { if (input_log_fn && input_log_budget > 0) { \
                         input_log_budget--; input_log_fn(__VA_ARGS__); } } while (0)

static int64_t now_ms(void){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
  return (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}
static int  has(const char *s,const char *sub){ return strstr(s,sub)!=NULL; }
static int  is_ev(void *p,int kind){ UEvent*e=p; return e && e->tag==UI_TAG && e->kind==kind; }

/* ---- fake android.view.InputDevice --------------------------------------
 * Unity's nativeInjectEvent calls InputEvent.getDevice() BEFORE it reads any
 * coordinates. Returning null there was survivable on the 2022.3 lineage but
 * is not here: the 2021.3 inject path wraps the whole read in a setjmp scope
 * (libunity nativeInjectEvent +0x30 `bl setjmp`, +0x34 `cbz w0` -> process),
 * so anything that trips AndroidJNISafe longjmps straight back out and the
 * function returns false -- the event is dropped without a single getX/getY.
 *
 * Observed in debug.log: the engine resolved getDevice, obtain, getSource,
 * getFlags, FLAG_WINDOW_IS_OBSCURED and recycle, and NEVER resolved getX,
 * getY, getPointerCount or getActionMasked. Method ids are resolved lazily on
 * first use, so "never resolved" means "never called, not once, in the whole
 * session" -- the reader was never reached.
 *
 * So we hand back a real (tagged, non-null) device and answer the queries a
 * touchscreen device would. */
void *unity_inputdevice(void){ return &g_dev; }
int   input_is_device(const void *recv){
  const UDevice *d = recv; return d && d->tag==UID_TAG;
}
/* int-returning InputDevice getters. Values describe a single built-in
 * touchscreen: id 0, sources = TOUCHSCREEN, no vendor/product (built-in
 * hardware reports 0 on real devices too), non-alphabetic keyboard. */
static uint64_t device_int(const char *m){
  if (has(m,"getSources"))        return (uint64_t)AINPUT_SOURCE_TOUCHSCREEN;
  if (has(m,"supportsSource"))    return 1;
  if (has(m,"getId"))             return 0;
  if (has(m,"getVendorId"))       return 0;
  if (has(m,"getProductId"))      return 0;
  if (has(m,"getControllerNumber")) return 0;
  if (has(m,"getKeyboardType"))   return 0;   /* KEYBOARD_TYPE_NONE */
  if (has(m,"isVirtual"))         return 0;
  if (has(m,"isEnabled"))         return 1;
  return 0;
}

/* ---- constructors ------------------------------------------------------- */
void *unity_motionevent(int action,int count,const int *ids,const float *xs,const float *ys){
  UEvent *e=&g_ev; memset(e,0,sizeof *e);
  static int64_t s_down_ms = 0;
  const int64_t nowms = now_ms();
  if ((action & AMOTION_ACTION_MASK) == AMOTION_ACTION_DOWN) s_down_ms = nowms;
  e->tag=UI_TAG; e->kind=KIND_MOTION; e->action=action;
  e->time_ms=nowms; e->down_ms=s_down_ms ? s_down_ms : nowms;
  if (count>UI_MAX_POINTERS) count=UI_MAX_POINTERS;
  e->count=count;
  for (int i=0;i<count;i++){ e->ids[i]=ids?ids[i]:i; e->xs[i]=xs?xs[i]:0; e->ys[i]=ys?ys[i]:0; }
  return e;
}
void *unity_keyevent(int action,int keycode){
  UEvent *e=&g_ev; memset(e,0,sizeof *e);
  e->tag=UI_TAG; e->kind=KIND_KEY; e->action=action; e->keycode=keycode; e->time_ms=now_ms();
  return e;
}

/* MotionEvent.obtain(MotionEvent src): Android's copy factory. nativeInjectEvent
 * copies our injected event into one IT owns and reads that copy *after* inject
 * returns (across frames), so we must hand back a real, separate UEvent copy --
 * not g_ev, which the next frame overwrites. A small ring keeps several in-flight
 * copies alive until the engine finishes reading them. */
/* Sized 32, not 16: the pointer layer can now emit several events in one frame
 * (a POINTER_DOWN, a batched MOVE and a POINTER_UP can all land together with
 * multitouch), so a 16-slot ring would recycle a copy after only four or five
 * frames instead of sixteen. 32 UEvents is a few KB. */
/* 128, not 32: round 96 went from one cursor to two, and each emits its own
 * DOWN/MOVE/UP alongside real touch. That roughly tripled the events per
 * frame and put 32 slots back inside the "recycled after four or five
 * frames" window the note above warns about -- a copy the engine still holds
 * gets overwritten by a later event. 128 UEvents is about 15 KB. */
static UEvent   g_ev_copies[128];
static unsigned g_ev_copy_i;
void *unity_motionevent_obtain(void *src){
  UEvent *s = src;
  if (!s || s->tag!=UI_TAG) return src;          /* not ours -> passthrough     */
  /* atomic: obtain() is called from whichever thread the engine is running
   * its input on, and two callers must never be handed the same slot. */
  const unsigned slot = __atomic_fetch_add(&g_ev_copy_i, 1u, __ATOMIC_RELAXED);
  UEvent *d = &g_ev_copies[slot & 127u];
  *d = *s;
  return d;
}

/* ---- ownership ---------------------------------------------------------- */
int input_owns_class(const char *cls){
  return has(cls,"view/MotionEvent") || has(cls,"view/KeyEvent") ||
         has(cls,"view/InputEvent")  || has(cls,"view/InputDevice");
}
/* Route by receiver, not class name: GetObjectClass() on our event handle
 * reports java/lang/Object (jni_fake only special-cases Bitmap), so class-name
 * routing misses every getter the engine resolves via GetObjectClass(event).
 * The tag is unique to our UEvent handle, so this is exact. */
int input_owns_recv(const void *recv){
  const UEvent *e = recv;
  return (e && e->tag==UI_TAG) || input_is_device(recv);
}
/* For instanceof classification by nativeInjectEvent: true if our handle is a
 * MotionEvent (vs KeyEvent). Caller must have checked input_owns_recv first. */
int input_recv_is_motion(const void *recv){
  const UEvent *e = recv; return e && e->tag==UI_TAG && e->kind==KIND_MOTION;
}

/* getX/getY/getPressure/... come as ()F or (I)F -- pull the pointer index when
 * the signature carries one. */
/* Index supplied by the JNI "A" (jvalue array) path, where there ARE no varargs.
 * -1 = not set. See input_set_a_index(). */
static int s_a_index = -1;
void input_set_a_index(int idx){ s_a_index = idx; }

static int ptr_index(const struct FakeID *id, va_list va){
  /* MotionEvent.getX/getY arrive through CallFloatMethodA, whose jvalue array
   * used to be discarded -- so va_arg here read an empty list, the garbage was
   * clamped to 0, and EVERY pointer reported pointer 0's coordinates. One finger
   * worked (index 0 is right by luck); two collapsed onto the first. Same class
   * of bug as round 24, in the input path this time.
   *
   * When the A path supplied the index, use it and clear it. */
  if (s_a_index >= 0) { const int i = s_a_index; s_a_index = -1; return i; }
  if (strstr(id->sig,"(I)")) { int idx=va_arg(va,int); return idx; }
  return 0;
}

/* ---- int / long getters ------------------------------------------------- */
uint64_t input_dispatch_int(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  UEvent *e = recv; const char *m=id->name;
  ILOG("    [in.i] %s  (cls=%s)\n", m, id->cls);
  if (input_is_device(recv)) return device_int(m);   /* InputDevice getters */
  if (!e || e->tag!=UI_TAG) return 0;

  /* shared InputEvent base */
  if (has(m,"getDeviceId")) return 0;
  if (has(m,"getSource"))   return (uint64_t)(e->kind==KIND_MOTION?AINPUT_SOURCE_TOUCHSCREEN:AINPUT_SOURCE_KEYBOARD);
  /* Android semantics: getEventTime() is THIS event's time, getDownTime() is
   * the time of the gesture's initial ACTION_DOWN. Returning the same value for
   * both told the engine that every MOVE and UP happened at the instant the
   * finger landed -- eventTime - downTime == 0 for the entire gesture, so a tap
   * has no duration and a hold has no age. Track the gesture start separately. */
  if (has(m,"getDownTime"))  return (uint64_t)e->down_ms;
  if (has(m,"getEventTime")) return (uint64_t)e->time_ms;
  if (has(m,"getMetaState")) return 0;
  if (has(m,"getFlags"))     return 0;

  if (e->kind==KIND_MOTION){
    if (has(m,"getActionMasked")) return (uint64_t)(e->action & AMOTION_ACTION_MASK);
    if (has(m,"getActionIndex"))  return (uint64_t)((e->action>>AMOTION_ACTION_PTR_IDX_SHIFT)&0xff);
    if (has(m,"getAction"))       return (uint64_t)e->action;
    if (has(m,"getPointerCount")) return (uint64_t)e->count;
    /* via ptr_index so the jvalue-array path supplies the index too -- reading
     * va_arg directly here had the same empty-va_list problem as getX/getY. */
    if (has(m,"getPointerId")){ int i=ptr_index(id, va); return (uint64_t)((i>=0&&i<e->count)?e->ids[i]:0); }
    if (has(m,"getToolType"))     return AMOTION_TOOL_TYPE_FINGER;
    if (has(m,"getButtonState"))  return 0;
    if (has(m,"getHistorySize"))  return 0;     /* no batched history -> engine skips getHistorical* */
    /* findPointerIndex(id) -> the ARRAY INDEX holding that pointer id, or -1
     * when the id is not down. Falling through to 0 meant "index 0" for every
     * query: harmless for one finger by luck, but it collapses multi-touch, and
     * for an id that is NOT present it points the engine at a live slot instead
     * of telling it the pointer is gone. */
    if (has(m,"findPointerIndex")) {
      const int want = ptr_index(id, va);   /* first int arg == the pointer id */
      for (int k=0;k<e->count;k++) if (e->ids[k]==want) return (uint64_t)k;
      return (uint64_t)(int64_t)-1;
    }
    /* isFromSource(source) -> does this event come from that source class?
     * strstr never matched the getSource test above ("isFromSource" does not
     * contain "getSource"), so this fell through to 0 == false. An engine that
     * gates touch handling on isFromSource(SOURCE_TOUCHSCREEN) would reject
     * every event. */
    if (has(m,"isFromSource")) {
      const int want = ptr_index(id, va);   /* first int arg == the source mask */
      const int mine = AINPUT_SOURCE_TOUCHSCREEN;
      return (uint64_t)((want == 0) || ((mine & want) == want) ? 1 : 0);
    }
    return 0;
  }
  /* KeyEvent */
  if (has(m,"getKeyCode"))     return (uint64_t)e->keycode;
  if (has(m,"getAction"))      return (uint64_t)e->action;
  if (has(m,"getRepeatCount")) return 0;
  if (has(m,"getUnicodeChar")||has(m,"GetUnicodeChar")) return 0;
  return 0;
}

/* ---- float getters ------------------------------------------------------ */
float input_dispatch_float(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  UEvent *e = recv; const char *m=id->name;
  ILOG("    [in.f] %s  (cls=%s)\n", m, id->cls);
  if (!e || e->tag!=UI_TAG || e->kind!=KIND_MOTION) return 0.0f;

  /* PRECISION FIRST. has() is strstr, and "getXPrecision" CONTAINS "getX" --
   * tested after the coordinate accessors below it would return the pointer's
   * X COORDINATE as the precision. Unity treats precision as a scale factor,
   * so a ~974 there silently rescales everything and a 0.0 risks inf/NaN.
   * Android reports 1.0 for a touchscreen. Consumes no argument. */
  if (has(m,"getXPrecision") || has(m,"getYPrecision")) return 1.0f;

  /* getAxisValue(axis[,pointerIndex]) -- the axis-generic spelling of
   * getX/getY, referenced by libunity. Answering 0.0 would put every touch at
   * the origin while getX/getY still looked perfectly correct.
   *
   * The FIRST int argument here is the AXIS, not a pointer index, so it must
   * be read before anything else touches the argument list: ptr_index()
   * consumes exactly one int (from the jvalue array on the A path, or va_arg
   * on the V path), and jni_fake only forwards the first one. Pointer index is
   * therefore taken as 0 -- correct for single touch, and the multi-pointer
   * spelling would need jni_fake to forward a second argument. */
  if (has(m,"getAxisValue")) {
    const int axis = ptr_index(id, va);
    switch (axis) {
      case AMOTION_AXIS_X:        return e->count ? e->xs[0] : 0.0f;
      case AMOTION_AXIS_Y:        return e->count ? e->ys[0] : 0.0f;
      case AMOTION_AXIS_PRESSURE: return 1.0f;
      case AMOTION_AXIS_SIZE:     return 0.1f;
      default:                    return 0.0f;
    }
  }

  int i = ptr_index(id, va);
  if (i<0 || i>=e->count) i=0;
  if (has(m,"getRawX")||(has(m,"getX"))) return e->count? e->xs[i] : 0.0f;
  if (has(m,"getRawY")||(has(m,"getY"))) return e->count? e->ys[i] : 0.0f;
  if (has(m,"getPressure"))   return 1.0f;
  if (has(m,"getSize"))       return 0.1f;
  if (has(m,"getOrientation"))return 0.0f;
  /* Contact-ellipse sizes. 0.0 is a degenerate ellipse that size-derived maths
   * can divide by; a small positive value is both truer and safer. */
  if (has(m,"getTouchMajor")||has(m,"getTouchMinor")||
      has(m,"getToolMajor") ||has(m,"getToolMinor"))  return 8.0f;
  return 0.0f;
}
