/* unity_entrypoints.h -- UnityPlayer native methods recovered from
 * libunity.so's JNI_OnLoad  (KILLER BEAN UNLEASHED, Unity 2021.3.31f1,
 * build 3409e2af086f, arm64 / IL2CPP).
 *
 * Auto-extracted from THIS build's libunity.so with
 *   python3 tools/extract_entrypoints.py libunity.so
 * JNI_OnLoad is at 0x393b10 and makes 10 bl calls, 9 of which are
 * RegisterNatives helpers. Helper #1 registers the 25 UnityPlayer natives
 * below (the drive surface); the other 8 register Choreographer/Swappy frame
 * pacing (ignored -- we run our own loop) and stub SDK classes (ARCore /
 * Camera2 / HFP / audio-volume / orientation-lock / status-query) that this
 * port never invokes.
 *
 * IMPORTANT: these offsets are LINK-TIME addresses for THIS EXACT libunity.so
 * (BuildID sha1 73132d02bcf05c6f78d2bc0af8a808e18339651a, 18,672,432 bytes).
 * If the game is updated, re-run the extractor and paste the new offsets.
 *
 * Runtime address = unity_mod.load_virtbase + offset (the .so links at base 0).
 *
 * ===========================================================================
 * VERSION NOTE -- READ THIS, IT IS THE BIGGEST DELTA IN THE PORT
 * ===========================================================================
 * The loader core this tree came from targets Unity 2022.3 (Fruit Ninja
 * Classic +, itself forked from PvZ Fusion / Zookeeper DX). This engine is
 * 2021.3.31f1 -- a WHOLE LTS LINE EARLIER, not a patch-release difference.
 * Two consequences, both handled below:
 *
 *   1. nativeInjectEvent has a DIFFERENT SIGNATURE.
 *          2022.3:  (Landroid/view/InputEvent;I)Z   <- event + deviceId
 *          2021.3:  (Landroid/view/InputEvent;)Z    <- event only
 *      The recovered signature is read straight out of this binary's
 *      JNINativeMethod table, so it is fact, not inference.
 *
 *      The loader still calls it through the 4-argument `fn_inject` typedef.
 *      That is SAFE and deliberate: under AAPCS64 the caller places the extra
 *      int in w3, the callee never reads w3, and there is no stack argument to
 *      clean up. Passing one extra register argument to a function that takes
 *      fewer is well-defined here. Keeping the 4-arg shape avoids touching
 *      android_native_unity.c's whole pointer pipeline for no behavioural
 *      gain. See KB_INJECT_TAKES_DEVICE_ID.
 *
 *   2. nativeGetNoWindowMode does NOT EXIST in this build. 2022.3 registers
 *      it; 2021.3 does not. It is resolved to 0 and gated (see below).
 *
 * Everything else in the 25-method set maps 1:1 onto what the loader drives.
 * ===========================================================================
 */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>
#include "so_util.h"

/* ---- UnityPlayer native method offsets (link-time vaddr) ---------------- */
/* JNI_OnLoad */
#define OFF_JNI_OnLoad                    0x393b10 /* (JavaVM*,reserved)->jint  caches VM, registers natives */

/* drive-critical */
#define OFF_initJni                       0x392f38 /* (env,thiz,Context)                 */
#define OFF_nativeRecreateGfxState        0x393184 /* (env,thiz,int,Surface)  set surface*/
#define OFF_nativeSendSurfaceChangedEvent 0x3931e8 /* (env,thiz)                         */
#define OFF_nativeRender                  0x393234 /* (env,thiz)->Z   per-frame; false=stop */
#define OFF_nativeInjectEvent             0x393288 /* (env,thiz,InputEvent)->Z  NO deviceId */
#define OFF_nativePause                   0x393020 /* (env,thiz)->Z                      */
#define OFF_nativeResume                  0x393078 /* (env,thiz)                         */
#define OFF_nativeFocusChanged            0x393134 /* (env,thiz,Z)                       */
#define OFF_nativeDone                    0x392fa0 /* (env,thiz)->Z   shutdown           */
#define OFF_nativeApplicationUnload       0x3930f0 /* (env,thiz)                         */
#define OFF_nativeLowMemory               0x3930b4 /* (env,thiz)                         */
#define OFF_nativeOrientationChanged      0x393abc /* (env,thiz,int,int)                 */

/* secondary / usually unused for a port */
#define OFF_nativeUnitySendMessage        0x3936e8 /* (env,thiz,String,String,byte[])    */
#define OFF_nativeMuteMasterAudio         0x3938e8 /* (env,thiz,Z)                       */
#define OFF_nativeIsAutorotationOn        0x39388c /* (env,thiz)->Z                      */
#define OFF_nativeSetLaunchURL            0x39397c /* (env,thiz,String)                  */
#define OFF_nativeRestartActivityIndicator 0x393940 /* (env,thiz)                        */

/* soft keyboard (route via SoftInputProvider stub; not needed for first boot) */
#define OFF_nativeSetInputArea            0x393410 /* (env,thiz,I,I,I,I)                 */
#define OFF_nativeSetKeyboardIsVisible    0x393484
#define OFF_nativeSetInputString          0x3934d8
#define OFF_nativeSetInputSelection       0x393584
#define OFF_nativeSoftInputClosed         0x3936a4
#define OFF_nativeSoftInputCanceled       0x3935e0
#define OFF_nativeSoftInputLostFocus      0x393624
#define OFF_nativeReportKeyboardConfigChanged 0x393668

/* ---- 2021.3.31f1 compatibility shims ----------------------------------- */
/* This engine registers nativeSendSurfaceChangedEvent, not the later
 * nativeSendSurfaceChanged. The core only ever calls the *Event form; the
 * alias keeps any stray reference compiling. */
#define OFF_nativeSendSurfaceChanged      OFF_nativeSendSurfaceChangedEvent

/* nativeHidePreservedContent does not exist in 2021.3. The loader core
 * declares it but never calls it. Resolving to 0 makes an accidental call
 * fail loudly instead of jumping into the middle of an unrelated function. */
#define OFF_nativeHidePreservedContent    0x0      /* ABSENT -- do not call */
#define KB_HAVE_HIDE_PRESERVED_CONTENT    0

/* nativeGetNoWindowMode is likewise ABSENT in 2021.3 (2022.3 has it). Same
 * treatment: resolve to 0 and gate. Nothing in the loader calls it, but the
 * gate is here so a future edit that does call it fails loudly rather than
 * jumping to libunity_base+0. */
#define OFF_nativeGetNoWindowMode         0x0      /* ABSENT -- do not call */
#define KB_HAVE_NO_WINDOW_MODE            0

/* nativeInjectEvent's real arity, for anyone auditing the call. 0 == the
 * 2021.3 one-argument form. The loader intentionally calls through the
 * 4-argument typedef anyway; see the VERSION NOTE for why that is safe. */
#define KB_INJECT_TAKES_DEVICE_ID         0

/* ---- JNI native signatures: ret (*)(JNIEnv*, jobject thiz, args...) ----- */
typedef void     (*fn_initJni)(void*,void*,void*);
typedef void     (*fn_gfxstate)(void*,void*,int32_t,void*);
typedef void     (*fn_v)(void*,void*);
typedef uint8_t  (*fn_z)(void*,void*);
typedef void     (*fn_vz)(void*,void*,int32_t);
typedef uint8_t  (*fn_inject)(void*,void*,void*,int32_t); /* 4th arg ignored by 2021.3 */
typedef void     (*fn_orient)(void*,void*,int32_t,int32_t);

#define UNITY_RESOLVE(mod, off) ((void*)((uintptr_t)(mod).load_virtbase + (off)))

/* ===========================================================================
 * Drive sequence (what the Java UnityPlayer does; main.c does it here):
 *
 *   initJni(env, thiz, fake_context);                  // early init
 *   nativeRecreateGfxState(env, thiz, 0, fake_surface);// give it the surface
 *   nativeSendSurfaceChangedEvent(env, thiz);          // engine builds GL state
 *   for (;;) {
 *       // input: nativeInjectEvent(env, thiz, motionEvent);
 *       if (!nativeRender(env, thiz)) break;           // false == engine wants out
 *   }
 *   nativeApplicationUnload(env, thiz);  nativeDone(env, thiz);
 *
 * NOTE on input: nativeInjectEvent takes a Java InputEvent/MotionEvent jobject,
 * which the engine then queries back via JNI (getActionMasked/getX/getY/
 * getPointerId/getPointerCount...). unity_input.c fabricates that object.
 *
 * Killer Bean Unleashed drives gameplay through Corgi Engine's on-screen
 * MMTouchButton / MMTouchJoystick widgets, which are ordinary uGUI elements
 * fed by this same MotionEvent path. So this entry point is as load-bearing
 * here as it was for Fruit Ninja -- but the requirement differs: Fruit Ninja
 * needed sub-frame swipe sampling for blade trails, whereas this game needs
 * accurate, *sustained* press state on small on-screen widgets. One pointer
 * sample per frame is fine; a dropped ACTION_UP is not, because a stuck
 * "fire" or "move right" is unrecoverable without another press.
 * See PORTING_KILLERBEAN.md sec 6.
 * =========================================================================== */

/* ---- Non-UnityPlayer native tables also present in this build (FYI) -------
 * We do NOT register/drive these; listed only so nobody re-hunts them.
 *   choreographer   nOnChoreographer                     @0x98d6f4
 *   swappy          nOnRefreshPeriodChanged              @0x987558
 *                   nSetSupportedRefreshPeriods          @0x987378
 *   ARCore          initializeARCore/pause/resume        @0x3924f4/0x392558/0x3925a0
 *   Camera2         initCamera2Jni/deinit                @0x362d94/0x362de0
 *                   nativeFrameReady/SurfaceTextureReady @0x369d34/0x369be0
 *   HFP audio       initHFPStatusJni/deinit              @0x38d26c/0x38d2b8
 *   audio volume    onAudioVolumeChanged                 @0x38f83c
 *   query status    nativeStatusQueryResult              @0x378ae0
 *   orient lock     nativeUpdateOrientationLockState     @0x36c9f8
 * -------------------------------------------------------------------------- */

#endif /* UNITY_ENTRYPOINTS_H */
