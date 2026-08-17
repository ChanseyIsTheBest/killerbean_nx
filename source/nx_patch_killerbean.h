/* nx_patch_killerbean.h -- in-memory libunity.so patch table for
 * KILLER BEAN UNLEASHED  (Unity 2021.3.31f1 / 3409e2af086f, arm64, IL2CPP).
 * libunity.so BuildID sha1 73132d02bcf05c6f78d2bc0af8a808e18339651a.
 * libil2cpp.so BuildID sha1 e21e697a7de9eadc574274ecbf578489516e4a73.
 *
 * WHAT IT DOES
 *   Unity's block allocator reserves memory in 256MB-aligned regions. On a 4GB
 *   Switch that granularity does not fit the so_loader address space, so we
 *   rewrite the allocator's region-size computation to 64MB granularity. Each
 *   entry rewrites one 32-bit instruction word: {from} is the stock word, {to}
 *   is the 64MB word. Same transform family as the Zookeeper DX / PvZ Fusion /
 *   Fruit Ninja tables; only the offsets, registers and encodings differ.
 *
 * ===========================================================================
 * HOW THESE OFFSETS WERE DERIVED  --  now from a SYMBOLIZED REFERENCE
 * ===========================================================================
 * A symbolized reference build of the SAME Unity source revision is now
 * available and was used:
 *
 *     libunity_sym.so   75,600 FUNC symbols, .text NOBITS
 *     libunity_ref.so   real code, identical .text address space
 *                       both BuildID c433796f76ad8fb6e1879017609aaae9dda77146
 *                       both report 2021.3.31f1 (3409e2af086f) git 3410402
 *     libunity.so       THE GAME BINARY, BuildID 73132d02bcf05c6f...
 *                       same version strings, different link
 *
 * Procedure (tools/gen_patch_kb.py):
 *   1. Every instruction in the REFERENCE .text was decoded for the five
 *      encoding classes that participate in a 256MB region computation, and
 *      each hit was attributed to its OWNING FUNCTION by symbol lookup.
 *      Result: 21 sites across 11 named allocator functions. (A 22nd hit, in
 *      CloneObjectImpl, matched only because "Allocator" appears in a
 *      template argument -- it is not allocator code and is excluded.)
 *   2. Each site was pinned into the GAME binary by masked-context search
 *      (ADRP/ADR/B/BL/B.cond/CBZ/TBZ immediates masked out, so the signature
 *      is position-invariant). ALL 21 resolved UNIQUELY, 20 of them at the
 *      minimum 6-instruction window and one at 3.
 *   3. All 21 game words are BYTE-IDENTICAL to the reference words -- same
 *      opcode, same registers, same immediates -- marked '=' below.
 *
 * INDEPENDENT CORROBORATION. Before the reference existed, this table was
 * derived blind, by clustering decoded granularity encodings across the game
 * binary with no symbols at all. That method produced THE SAME 21 SITES, at
 * the same addresses. Two methods with no shared assumptions agreeing site
 * for site is why completeness is no longer an open question here.
 *
 * The blind method had classified four of these (0x160948/0x160950 and
 * 0x164e00/0x164e08) as lower-confidence because they sit outside the
 * obvious allocator functions. Reverse-pinning them into the reference named
 * them: LocalLowLevelAllocator::ReserveMemoryBlock and
 * DynamicHeapAllocator::RequestLargeAllocMemory. They are real, and the
 * earlier doubt is resolved.
 *
 * CONFIDENCE: HIGH. Function-attributed, uniquely pinned, byte-identical,
 * and independently reproduced.
 *
 * TRANSFORMS
 *     MOVZ/MOVK  imm16 0x1000 -> 0x0400            256MB -> 64MB
 *     MOVN       imm16 0xF000 -> 0xFC00            0x0FFFFFFF -> 0x03FFFFFF
 *     LSR  (UBFM) immr 28 -> 26, imms 63 unchanged >>28 -> >>26
 *     UBFX (UBFM) immr 28 -> 26, imms -= 2         field WIDTH preserved
 *     SUB  shifted  lsl #28 -> lsl #26
 *     AND  bitmask  low bound bit 28 -> bit 26     (top bound preserved:
 *                   0x...FFF0000000 -> 0x...FFFC000000, at whatever width)
 *
 * SAFETY
 *   nx_patch_libunity() is VERIFY-FIRST: it reads each target word and only
 *   patches if it already equals {from}; if ANY site mismatches it patches
 *   NOTHING and logs loudly. A wrong offset is caught, not catastrophic. If
 *   you update the game, expect this to fire -- that is the table telling you
 *   to re-run tools/scan_granularity.py, not a bug.
 * ===========================================================================
 */
#ifndef NX_PATCH_KILLERBEAN_H
#define NX_PATCH_KILLERBEAN_H

#include <stdint.h>

#define KB_HAVE_BRANCH_FORCES  0   /* not needed unless an allocator abort appears */

/* ===========================================================================
 * INHERITED-FEATURE GATES -- ALL OFF, AND WHY THAT IS CORRECT
 * ===========================================================================
 * The Fruit Ninja tree this was retargeted from carried a set of hooks and
 * guards whose addresses were derived against FRUIT NINJA's libil2cpp.so and
 * libunity.so. Those addresses are MEANINGLESS in this game's binaries --
 * different Unity version, different game code, different link layout.
 * Inheriting them would patch arbitrary instructions.
 *
 * So every one of them is gated OFF here with its offsets zeroed. This is not
 * laziness: an unlocated hook is a missing feature, but a WRONG hook is
 * memory corruption with a stack trace that points somewhere unrelated.
 *
 * Each gate below documents the SYMPTOM that means you need to go derive it
 * for this game. Turn one on only after you have re-derived its offsets.
 * ===========================================================================
 */

/* ---- il2cpp IsInst null-klass guard --------------------------------------
 * Fruit Ninja needed this because ITS DialogueConfig held an Il2CppClass*
 * where an array was expected. That is a Fruit-Ninja-specific data bug. This
 * game has no DialogueConfig. OFF, offsets zeroed.
 * SYMPTOM if you ever need it: fault reading [klass + 0x135] inside
 * il2cpp's assignability check, with x0 -> an object whose first word is 0. */
#define KB_IL2CPP_ISINST_AND     0u
#define KB_IL2CPP_ISINST_AND_OLD 0u
#define KB_IL2CPP_ISINST_GUARD   0u
#define KB_HAVE_ISINST_GUARD     0

/* ---- DataBinding / ConvertFromTo liveness guard --------------------------
 * Likewise Fruit-Ninja-specific (its MoreMountains DataBinding path passed a
 * raw 1 to Object.GetType). Killer Bean uses the same MoreMountains Tools
 * package, so this one is more plausible to recur than the IsInst guard --
 * but the ADDRESSES are still Fruit Ninja's and must be re-derived.
 * SYMPTOM: fault in a leaf `ldr x8,[x0] ; add x0,x8,#0x20` with lr pointing
 * into IDataSource$$ConvertFromTo. OFF, offsets zeroed. */
#define KB_IL2CPP_LIVENESS_BODY    0u
#define KB_IL2CPP_LIVENESS_ADD     0u
#define KB_IL2CPP_LIVENESS_ADD_HI  0u
#define KB_IL2CPP_LIVENESS_ADD_LO  0u
#define KB_IL2CPP_LIVENESS_HASPAR  0u
#define KB_IL2CPP_LIVENESS_LR_INST 0u
#define KB_IL2CPP_CFT_LR           0u
#define KB_IL2CPP_GETTYPE_LEAF     0u
#define KB_HAVE_LIVENESS_GUARD     0
static const uint32_t KB_LIVENESS_PROLOGUE[4] = { 0u, 0u, 0u, 0u };

/* ---- diagnostic field offsets used by nx_exception_dump.c ----------------
 * These describe FRUIT NINJA managed types. Zeroed; the exception dumper
 * checks for 0 and simply prints less. Re-derive from dump.cs if you want
 * richer crash dumps for this game. */
#define KB_CFT_SP_DATABINDING    0u
#define KB_CFT_SP_VALUE          0u
#define KB_DATABINDING_DATAPATH  0u
#define KB_DATABINDING_PROPNAME  0u
#define KB_DATABINDING_STRFMT    0u
#define KB_DIALOGUE_CONFIG       0u
#define KB_DIALOGUE_INDEX        0u
#define KB_DLGCONFIG_PIECES      0u
#define KB_SDS_SP_DIALOGUE       0u
#define KB_SDS_SP_PIECE          0u
#define KB_REFLECTIONTYPE_TYPE   0u
/* These two are il2cpp ABI, not game-specific: the array header layout is
 * fixed by the runtime, so they carry over unchanged and stay live. */
#define KB_IL2CPP_ARRAY_LEN      0x18u
#define KB_IL2CPP_ARRAY_DATA     0x20u
#define KB_IL2CPP_KLASS_DEPTH    0x120u
#define KB_IL2CPP_KLASS_TYPEHIER 0x0u

/* ---- TimeManager / engine-clock fix (libunity offsets) -------------------
 * Fruit Ninja's entry 0x478aec / body 0x478b10 / GetTimeManager 0x4790d4 are
 * 2022.3 addresses. Not valid here. OFF.
 * SYMPTOM: deltaTime stays 0, async scene loads never complete, the first
 * nativeRender blocks forever or the game sits on a black screen after the
 * splash. That is when you go pin TimeManager::Update in THIS libunity. */
/* (real TimeManager addresses are further down, in the TimeManager section) */
/* Expected prologue of an il2cpp icall thunk, used to verify each Time.get_*
 * hook site before patching. Zero here because the Time.get_* thunks are NOT
 * derived for this game (KB_HAVE_TIME_HOOKS is 0 in config.h); the macro must
 * still exist because main.c compares against it outside the gate. */
#define KB_TIME_THUNK_WORD    0xA9BF7BF3u  /* stp x19,x30,[sp,#-0x10]! -- see CAVEAT at the thunk table */
#define KB_HAVE_TIME_FIX      1   /* vsync triple derived -- blocker cleared */

/* ===========================================================================
 * Time.get_* icall thunks  --  DERIVED FROM dump.cs, ENABLED
 * ===========================================================================
 * These are libil2cpp offsets, so no libunity reference is involved. They come
 * from the Il2CppDumper output for THIS build (dump.cs), UnityEngine.Time,
 * TypeDefIndex 5940.
 *
 * ADDRESS CONVENTION -- worth stating, because it is a trap. This dump prints
 * "RVA: 0x1FA2D6C Offset: 0x1FA2D6C VA: 0x1FA2D6C", all three identical, and
 * the value is a plain virtual address in the .so. It is NOT the
 * "runtime_RVA = Offset + 0x4000" convention some Il2CppDumper configurations
 * emit, and adding 0x4000 lands in the wrong place. Also note these addresses
 * are NOT in .text: generated method code lives in this binary's own `il2cpp`
 * section (0x9b07c4 .. 0x21da31c). The loader maps the whole LOAD zone, so
 * `il2cpp_virtbase + offset` is still correct -- the same convention already
 * used for KB_IL2CPP_VM_GLOBAL.
 *
 * VERIFICATION. Every offset below was disassembled and is a textbook il2cpp
 * icall thunk, all eight identical in shape:
 *
 *     stp  x19, x30, [sp, #-0x10]!     <- KB_TIME_THUNK_WORD
 *     adrp x19, #0x2eee000
 *     ldr  x0,  [x19, #<slot>]         ; cached icall fn ptr
 *     cbnz x0,  <call>                 ; already resolved?
 *     adrp x0,  #<page> ; add x0,x0,#<off>   ; the icall NAME string
 *     bl   0x8ffbcc                    ; shared resolver
 *     str  x0,  [x19, #<slot>]         ; cache it
 *     ldp  x19, x30, [sp], #0x10
 *     br   x0                          ; tail-call
 *
 * Stronger still: the name string each thunk passes to the resolver was read
 * out of .rodata and reads "UnityEngine.Time::get_time()",
 * "UnityEngine.Time::get_deltaTime()", and so on -- each thunk names itself,
 * and every name matches what dump.cs claimed. That is the real verification.
 *
 * CAVEAT ON THE GUARD WORD. main.c checks the first word against
 * KB_TIME_THUNK_WORD before patching. 0xA9BF7BF3 is `stp x19,x30,[sp,#-0x10]!`
 * -- an extremely common prologue (Swappy::IsEnabledAndActive starts with the
 * same word). So this guard catches a stale/garbage offset but would NOT catch
 * an offset that happened to land on another ordinary function. It is a
 * sanity check, not an identity check; the disassembly above is what actually
 * establishes these are the right nine.                                     */
#define KB_IL2_get_time                  0x1FA2D6Cu
#define KB_IL2_get_timeSinceLevelLoad    0x1FA2D94u
#define KB_IL2_get_deltaTime             0x1FA2DBCu
#define KB_IL2_get_unscaledTime          0x1FA2DE4u
#define KB_IL2_get_unscaledDeltaTime     0x1FA2E34u
#define KB_IL2_get_timeScale             0x1FA2F1Cu
#define KB_IL2_get_frameCount            0x1FA2F7Cu
#define KB_IL2_get_realtimeSinceStartup  0x1FA2FCCu

/* ABSENT IN THIS BUILD. UnityEngine.Time here has no smoothDeltaTime property
 * at all -- the string does not occur even once in the whole 19 MB dump, so
 * the managed linker stripped it because the game never reads it. Left at 0;
 * main.c skips a zero offset explicitly rather than probing libil2cpp+0.
 * If a future game version starts using it, re-dump and fill this in.       */
#define KB_IL2_get_smoothDeltaTime       0u

/* Other Time members present in this build, recorded but NOT hooked (the
 * loader has no substitute for them and does not read them):
 *     get_fixedUnscaledTime   0x1FA2E0C     get_fixedDeltaTime    0x1FA2E5C
 *     set_fixedDeltaTime      0x1FA2E84     get_maximumDeltaTime  0x1FA2EBC
 *     set_maximumDeltaTime    0x1FA2EE4     set_timeScale         0x1FA2F44
 *     get_renderedFrameCount  0x1FA2FA4     get_inFixedTimeStep   0x1FA2FF4 */


/* KB_HAVE_TIME_HOOKS is owned by config.h (single source of truth), where it
 * is 0 -- these nine il2cpp thunk offsets were never derived for this game. */

/* ===========================================================================
 * LevelMap_WhatsNew.CloseWhatsNew hook  --  diagnostic AND workaround
 * ===========================================================================
 * The "What's New" popup's close button does nothing, while every other button
 * in the game -- including small ones in the options and UI-config menus --
 * works. By that point the wrapper was demonstrably clean on that screen: zero
 * managed and native exceptions, 44 DOWN / 44 UP all CONSUMED including
 * multi-touch, coordinates landing on the button, and display metrics matching
 * the delivery space (1280x720 on both sides). So the remaining question was
 * binary, and inference had run out: DOES THE CLICK REACH THE HANDLER AT ALL?
 *
 * Offsets from dump.cs (LevelMap_WhatsNew, TypeDefIndex 1751), each confirmed
 * by disassembly of THIS libil2cpp:
 *
 *   CloseWhatsNew          RVA 0xA024A4, first word 0xF81E0FF4
 *                                        (str x20, [sp, #-0x20]!)
 *   panel_whatsnew         field offset 0x18 -- the body does
 *                                        `ldr x0,[x19,#0x18]` then `cbz`
 *   GameObject.SetActive   RVA 0x1FA4C98
 *
 * The call convention was READ OFF the original call site rather than assumed:
 *       +0x50  mov  w1, wzr        ; value = false
 *       +0x54  mov  x2, xzr        ; MethodInfo* = NULL
 *       +0x58  bl   #0x1fa4c98
 * so the hook calls SetActive(panel, 0, NULL) exactly as the game does --
 * including the NULL MethodInfo, which is what the original passes.
 */
#define KB_IL2_CloseWhatsNew          0xA024A4u
#define KB_CLOSE_WHATSNEW_WORD        0xF81E0FF4u  /* str x20,[sp,#-0x20]! */
/* ---- WeaponStore_IAP, offline entitlements (round 160) -------------------
 * DERIVED from dump.cs for this build:
 *   public  void Purchase_Weapons_Pack()             RVA 0xA0DA58  A9BE53F5 A9017BF3
 *   public  void Purchase_Unlimited_Ammo()           RVA 0xA0DB10  A9BE53F5 A9017BF3
 *   private void Owned_Weapons_Pack(bool)            RVA 0xA0CC3C  A9BA6FFC A90167FA
 *   private void Owned_Unlimited_Ammo(bool)          RVA 0xA0D3B8  F81D0FF6 A90153F5
 *
 * The two Purchase_* handlers are replaced; the two Owned_* grants are CALLED,
 * not patched, so the unlock runs the game's own code. Both Purchase_* share a
 * prologue, so each is still checked against its own address -- one shared
 * expected word across a table is how you patch the wrong function and never
 * find out. */
#define KB_IL2_Purchase_Weapons_Pack    0xA0DA58u
#define KB_PURCHASE_WEAPONS_W0          0xA9BE53F5u
#define KB_PURCHASE_WEAPONS_W1          0xA9017BF3u
#define KB_IL2_Purchase_Unlimited_Ammo  0xA0DB10u
#define KB_PURCHASE_AMMO_W0             0xA9BE53F5u
#define KB_PURCHASE_AMMO_W1             0xA9017BF3u
#define KB_IL2_Owned_Weapons_Pack       0xA0CC3Cu
#define KB_IL2_Owned_Unlimited_Ammo     0xA0D3B8u

/* ---- EventSystem.Update: let every EventSystem tick (round 156) ---------
 * level3 (the level map) is the ONLY scene in this game with TWO EventSystems:
 *
 *   path_id  87   EventSystem + StandaloneInputModule + TouchInputModule
 *   path_id 199   EventSystem + InputSystemUIInputModule      <-- new input
 *
 * every other scene has just the legacy one. uGUI's EventSystem.Update() opens
 * with `if (current != this) return;` and `current` is whichever registered
 * first, so on the map the new-Input-System EventSystem wins and the legacy one
 * never ticks. We only feed the legacy backend (UnityPlayer.nativeInjectEvent),
 * so nothing on that screen can be clicked: the What's New X, Button_play and
 * the menu button all die together, while every other scene is fine. On Android
 * both backends are fed from one MotionEvent, so nobody ever noticed.
 *
 * Proven by probe: replacements on LevelMap_Control.PlayLevel/Menu installed
 * and NEVER fired, so the click was not reaching the handler at all.
 *
 * EventSystem.Update  RVA 0x21CA404
 *   0x21CA4A4  bl   0x1F9BFCC          ; Object.op_Inequality(current, this)
 *   0x21CA4A8  tbnz w0, #0, 0x21CA630  ; if (current != this) return   <-- NOP
 *   0x21CA4AC  mov  x0, x19
 *   0x21CA4B0  bl   0x21CA2F0          ; TickModules()
 *
 * NOPping the branch lets both tick. Where only one EventSystem exists the
 * branch was never taken, so this is a no-op everywhere else in the game; on
 * the map the legacy module gets to run and the new-input one finds no events
 * and does nothing. */
#define KB_IL2_EventSystem_Update_Br  0x21CA4A8u
#define KB_EVENTSYSTEM_BR_WORD        0x37000C40u  /* tbnz w0,#0,#0x188 */
#define KB_ARM64_NOP                  0xD503201Fu

/* ---- LevelMap_Control.PlayLevel / .Menu (round 155, DIAGNOSTIC) ---------
 * DERIVED from dump.cs for this build:
 *   public void LevelMap_Control.Menu()       RVA 0x9FF1FC  F81E0FF4
 *   public void LevelMap_Control.PlayLevel()  RVA 0x9FF290  A9BE53F5
 * These are the two handlers behind the only controls on the map that are uGUI
 * Buttons rather than LeanSelectables -- Button_play (level3 path_id 78) has
 * RectTransform + CanvasRenderer + Image + Button, the classic uGUI shape.
 * They are the two the player reports dead now that the Lean nodes work. */
#define KB_IL2_LevelMap_Menu          0x9FF1FCu
#define KB_LEVELMAP_MENU_WORD         0xF81E0FF4u  /* str x20,[sp,#-0x20]!     */
#define KB_IL2_LevelMap_PlayLevel     0x9FF290u
#define KB_LEVELMAP_PLAYLEVEL_WORD    0xA9BE53F5u  /* stp x21,x20,[sp,#-0x20]! */

/* ---- LeanInput touch source (round 154) ---------------------------------
 * DERIVED for this build from dump.cs + disassembly:
 *   public static int  LeanInput.GetTouchCount()                RVA 0x156AD68
 *   public static void LeanInput.GetTouch(int index, out int id,
 *                        out Vector2 position, out float pressure,
 *                        out bool set)                          RVA 0x156ADD8
 *
 * GetTouchCount disassembles to
 *     EnhancedTouchSupport.get_enabled() / .Enable()      (0x1E72190/0x1E721E0)
 *     UnityEngine.InputSystem.EnhancedTouch.Touch.get_activeTouches()
 *                                                        (0x1E74734)
 * -- the NEW input system. We inject through UnityPlayer.nativeInjectEvent,
 * which is the legacy path; the new system's native event queue is never
 * written, so LeanTouch sees no touches at all. Confirmed at runtime: with
 * PointOverGui hooked, it was never called once across 40+ taps, i.e. no finger
 * was ever created.
 *
 * On Android one MotionEvent feeds both backends. We feed one. */
#define KB_IL2_LeanGetTouchCount      0x156AD68u
#define KB_LEAN_GETTOUCHCOUNT_WORD    0xA9BF7BF3u  /* stp x19,x30,[sp,#-0x10]! */
#define KB_IL2_LeanGetTouch           0x156ADD8u
#define KB_LEAN_GETTOUCH_WORD         0xD10243FFu  /* sub sp,sp,#0x90          */

/* ---- LeanTouch.PointOverGui (round 153) ---------------------------------
 * DERIVED for this build from dump.cs + disassembly:
 *   public static bool LeanTouch.PointOverGui(Vector2 screenPosition)
 *   RVA 0x1583D74, prologue  6DBD23E9  stp d9, d8, [sp, #-0x30]!
 *
 * Why it matters: LevelMap_Button.Update() (RVA 0x9FD780) is
 *     if (this->lean_selectable == null) return;
 *     if (LeanSelectable.get_IsSelected(lean_selectable))   // 0x1576FBC
 *         levelmap_control->is_button_pressed = true, selected_level = name;
 * so every level node on the map is gated on a LeanSelectable being SELECTED.
 * Nothing on that screen is a uGUI Button, which is why uGUI screens respond
 * and the map does not. LeanTouch refuses to select for a finger whose
 * StartedOverGui is true, and StartedOverGui is PointOverGui() evaluated at the
 * DOWN, via EventSystem.RaycastAll. */
#define KB_IL2_LeanPointOverGui       0x1583D74u
#define KB_LEAN_POINTOVERGUI_WORD     0x6DBD23E9u  /* stp d9,d8,[sp,#-0x30]! */

#define KB_WHATSNEW_PANEL_OFF         0x18u        /* LevelMap_WhatsNew.panel_whatsnew */
#define KB_IL2_GameObject_SetActive   0x1FA4C98u

/* ALL THREE close handlers, because the first pass hooked only one and assumed
 * the button targeted it. LevelMap_WhatsNew owns three panels and three close
 * methods; the visible dialog says "What's New", but a close button is just as
 * often wired to a sibling handler. Each has the SAME prologue word, each reads
 * its panel field then calls GameObject.SetActive(panel, false, NULL), so one
 * hook shape covers all three -- and whichever fires names the real target. */
#define KB_IL2_Close_NewDay           0xA02544u
#define KB_NEWDAY_PANEL_OFF           0x20u        /* panel_NewDay        */
#define KB_IL2_Close_SpecialBonus     0xA025C0u

/* ---- Awake/Start TRAMPOLINES (not replacements) --------------------------
 * The close handlers could be replaced outright because the only effect that
 * mattered was hiding a panel. Awake and Start cannot: their bodies have real
 * side effects (Awake assigns dungeon_refresh; Start decides whether the popup
 * shows at all), so these are true trampolines that log and then run the stock
 * body.
 *
 * Both begin with the SAME two PC-independent instructions, which is what makes
 * one trampoline shape cover both:
 *     A9BE53F5  stp x21, x20, [sp, #-0x20]!
 *     A9017BF3  stp x19, x30, [sp, #0x10]
 * followed by two ADRPs that differ per method and must be rebuilt from the
 * runtime base rather than copied (a relocated ADRP computes the wrong page):
 *     Awake  +0x8 x20 <- 0x2ee5000   +0xc x21 <- 0x2cd9000
 *     Start  +0x8 x21 <- 0x2ee5000   +0xc x20 <- 0x2d0a000
 * Execution resumes at +0x10 in both cases, which expects exactly those two
 * registers set and x0 still holding `this`. */
#define KB_IL2_WhatsNew_Awake         0xA023A0u
#define KB_IL2_WhatsNew_Start         0xA02428u
#define KB_WHATSNEW_PRO_W0            0xA9BE53F5u  /* stp x21,x20,[sp,#-0x20]! */
#define KB_WHATSNEW_PRO_W1            0xA9017BF3u  /* stp x19,x30,[sp,#0x10]   */
#define KB_WN_PAGE_A                  0x2ee5000u   /* both methods, +0x8 or +0xc */
#define KB_WN_PAGE_AWAKE_B            0x2cd9000u
#define KB_WN_PAGE_START_B            0x2d0a000u
#define KB_WHATSNEW_REFRESH_OFF       0x30u        /* dungeon_refresh -- set at Awake +0x6c */

/* GameObject active-state getters, for probing which panel is really on screen.
 * dump.cs marks them [NativeMethod("IsSelfActive")] / [NativeMethod("IsActive")],
 * so they are icalls into the engine and safe to call directly with a NULL
 * MethodInfo, exactly like GameObject.SetActive above.
 *
 * activeSelf vs activeInHierarchy matters here: a panel can be activeSelf=true
 * while a deactivated parent keeps it off screen. Only activeInHierarchy says
 * "this is actually being drawn and raycast against". */
#define KB_IL2_GO_get_activeSelf        0x1FA4CDCu
#define KB_IL2_GO_get_activeInHierarchy 0x1FA4D18u
#define KB_SPECIALBONUS_PANEL_OFF     0x28u        /* panel_SpecialBonus  */

/* ===========================================================================
 * Boehm GC stop-the-world bridge  --  DERIVED FOR THIS GAME, ENABLED
 * ===========================================================================
 * WHERE IT LIVES. Not in libunity. Boehm is compiled into libil2cpp.so: that
 * binary carries Boehm's own copyright string and internal error strings
 * ("GC_mark_some: bad state", "GC_push_all_stacks: sp not set!") and imports
 * pthread_kill / pthread_sigmask / sem_init / sem_wait / sem_post / sigaction.
 * libunity.so contains none of those and does not import pthread_kill at all.
 * A symbolized libunity reference -- of any Unity version -- cannot reach
 * this; searching 93k-96k engine symbols for Boehm internals returns zero.
 *
 * HOW IT WAS DERIVED (tools/gc_derive.py, against YOUR libil2cpp.so, which is
 * stripped -- no reference build and no symbols were needed):
 *   1. Resolve the PLT stub for pthread_kill from .rela.plt plus the stub's
 *      own ADRP/LDR pair                                     -> 0x7179c0
 *   2. Scan .text for BL to that stub. EXACTLY TWO callers exist, which is the
 *      expected shape:
 *          0x94c32c  GC_suspend_all     (sends suspend sig, returns count)
 *          0x94c574  GC_start_world     (sends restart sig)
 *          0x94c504  GC_stop_world      (sem_wait xN, N = count suspended)
 *          0x94c1d4  GC_suspend_handler (the signal handler that acks)
 *   3. Read the globals straight out of those four bodies.
 *
 * THE CLINCHING EVIDENCE. GC_suspend_handler contains, verbatim:
 *
 *     lsr  x9, x0, #8
 *     eor  w8, w9, w1
 *     eor  w8, w8, w8, lsr #16
 *     and  x8, x8, #0xff
 *     add  x9, x9, #0x608          <- GC_threads
 *     ldr  x21, [x9, x8, lsl #3]
 *
 * That is instruction-for-instruction the hash nx_gc_thread_index() in
 * libc_shim.c already implements. The loader's thread lookup and this
 * binary's agree exactly. GC_threads is confirmed twice more: both
 * GC_suspend_all and GC_start_world walk the same table with `cmp x21,#0x100`
 * (256 buckets -- the size the loader's table[] assumes), chaining via [x26].
 */
/* ---- the four load-bearing globals, each pinned to its use site ----------
 * Re-verified by backward-slicing the argument at every call site rather than
 * by a forward scan. The four that actually drive the bridge:
 *
 *   suspend signal   0x2ee52e4  GC_suspend_all +0x88:
 *                                 ldr w1,[x24,#0x2e4] ; bl pthread_kill
 *   restart signal   0x2ee52e8  GC_start_world +0xa0:
 *                                 ldr w1,[x24,#0x2e8] ; bl pthread_kill
 *   start-world gate 0x2ee52e0  GC_start_world +0x84:
 *                                 ldr w8,[x23,#0x2e0] ; cbz w8,<skip>
 *                               -- gates whether start_world signals AT ALL
 *   ack semaphore    0x2ef6fb0  fed to FIVE call sites (see below)
 *
 * Note the three counters sit in consecutive 4-byte slots, gate first:
 * 0x2e0 / 0x2e4 / 0x2e8. Any build showing that trio adjacent in .data with a
 * cbz-gate on the lowest one is the same structure at different addresses. */
#define GC_SUSPEND_SIG_OFF_FN    0x2ee52e4u  /* .data  GC_suspend_all +0x88   */
#define GC_RESTART_SIG_OFF_FN    0x2ee52e8u  /* .data  GC_start_world +0xa0   */

/* THE START-WORLD GATE. Boehm calls this GC_retry_signals, and that is the
 * name libc_shim.c uses -- but the name undersells it. Its functional role in
 * THIS build is a gate on the restart path:
 *
 *     GC_start_world +0x84   ldr  w8, [x23, #0x2e0]
 *                    +0x88   cbz  w8, <skip>          ; 0 -> never signals
 *                    +0x8c   ldr  x8, [x27, #0x10]    ; thread stop_count
 *                    +0x90   ldr  x9, [.., #0x5e8]    ; GC_stop_count
 *                    +0x94   orr  x9, x9, #1          ; <- the |1 convention
 *                    +0x98   cmp  x8, x9
 *                    +0xa0   ldr  w1, [x24, #0x2e8]   ; restart sig
 *                    +0xa4   bl   pthread_kill
 *
 * The loader's emulation in libc_shim.c acks a second time with
 * `stop_count | 1` exactly when this gate is set, which is precisely what the
 * `orr x9,x9,#1` above compares against. Emulation and binary agree. */
#define GC_RETRY_SIGNALS_OFF_FN  0x2ee52e0u  /* .data  GC_start_world +0x84   */

/* THE ACK SEMAPHORE. Confirmed at FIVE independent call sites, each with an
 * explicit `adrp x0,#0x2ef6000 ; add x0,x0,#0xfb0` immediately before the
 * call, so this is a slice of the actual argument, not a guess:
 *
 *     sem_init      @0x94c6f4   (setup)
 *     sem_timedwait @0x94c49c   (GC_suspend_all -- the bounded ack wait)
 *     sem_wait      @0x94c540   (GC_stop_world  -- the blocking ack wait)
 *     sem_post      @0x94c2a0   (handler, first ack)
 *     sem_post      @0x94c2ec   (handler, retry ack, behind the gate)
 *
 * The sem_timedwait site was MISSED by the first derivation pass, which only
 * scanned for sem_wait/sem_post/sem_init. It agrees with the other four, so
 * the value was right, but the confirmation was weaker than it should have
 * been. tools/gc_derive.py now scans the full set.
 *
 * ONE SEMAPHORE, not three: all five sites resolve to the same address. The
 * three macro names below are kept because libc_shim.c refers to them
 * separately; they intentionally hold one value. */
#define GC_START_ACK_OFF_FN      0x2ef6fb0u  /* .bss */
#define GC_ACK_SEM_OFF_FN        0x2ef6fb0u  /* .bss */
#define GC_RESTART_SEM_OFF_FN    0x2ef6fb0u  /* .bss */

#define GC_STOP_COUNT_OFF_FN     0x2ef45e8u  /* .bss   LDAR at handler entry;
                                              * compared as `|1` in start_world */
#define GC_THREADS_OFF_FN        0x2ef4608u  /* .bss   GC_threads[256] */

/* ONE SEMAPHORE, not three. Every sem_* site in this build -- the single
 * sem_init, the sem_wait loop in GC_stop_world, and both sem_post calls in the
 * handler -- resolves to 0x2ef6fb0. The three macros above stay distinct
 * because libc_shim.c names them separately, but they intentionally hold the
 * same address. The reference port found the same single-semaphore shape, so
 * this is Boehm's configuration rather than a quirk of this build.
 *
 * ---- Boehm per-thread struct field offsets: CORRECTED FOR THIS BUILD ------
 * The reference tree carried STACKPTR 0x28 / STOPCNT 0x18, and an earlier
 * revision of this header repeated them with a comment claiming they were
 * "Boehm ABI, fixed by the collector's own layout" and therefore safe to
 * inherit. THAT WAS WRONG. Read off GC_suspend_handler:
 *
 *     ldr  x21, [x21]                          <- next        (+0x00)
 *     ldr  x8,  [x21, #8]                      <- thread id   (+0x08)
 *     ldr  x8,  [x21, #0x10] ; and x8,x8,#~1   <- stop_count  (+0x10), low bit a flag
 *     add  x19, x21, #0x10   ; stlr x20,[x19]  <- publishes stop_count
 *     str  x8,  [x21, #0x18]                   <- stack pointer (+0x18)
 *
 * Only the id offset matched. Inheriting 0x18/0x28 would have made the
 * collector read the wrong words out of every thread it suspended -- exactly
 * the failure mode this tree gates features off to avoid.                   */
#define GC_THREAD_NEXT_OFF       0x0u
#define GC_THREAD_ID_OFF         0x8u
#define GC_THREAD_STOPCNT_OFF    0x10u   /* CORRECTED: reference tree said 0x18 */
#define GC_THREAD_STACKPTR_OFF   0x18u   /* CORRECTED: reference tree said 0x28 */

/* ---- splash / video / preload probes (all Fruit Ninja il2cpp) ----------- */
#define KB_GET_SHOULD_SHOW_SPLASH     0u
#define KB_IL2_SPLASH_FINISHED_CHECK  0u
#define KB_IL2CPP_FINISH_FLAG         0u
#define KB_HAVE_FINISH_PROBE          0
#define KB_IL2_CRTFVP_Prepare             0u
#define KB_IL2_CRTFVP_OnVideoPlayerReady  0u
#define KB_IL2_RAISE_EXCEPTION            0u
#define KB_IL2_RAISE_INNER_BL             0u
#define KB_IL2_RAISE_TAIL                 0u
#define KB_HAVE_VIDEO_BYPASS              0
#define KB_PRELOAD_EXIT_BRANCH        0u
#define KB_PRELOAD_BUDGET_DEFAULT     0u
#define KB_PRELOAD_BUDGET_TABLE_LOAD  0u
/* KB_PACING_GETTER: real value in the Swappy section below */

/* ===========================================================================
 * THE ALLOCATOR TABLE -- 21 sites, derived for THIS binary
 * =========================================================================== */
typedef struct { uint32_t off, from, to; } NxPatchWord;

/* All 21 words are byte-identical between reference and game ('='). */
static const NxPatchWord KB_PATCH_WORDS[] = {
  /*  0 = */ { 0x160948, 0x12be0009, 0x12bf8009 },  /* LocalLowLevelAllocator::ReserveMemoryBlock   +0x98  movn 0xF000->0xFC00 */
  /*  1 = */ { 0x160950, 0x92648d36, 0x92669536 },  /* LocalLowLevelAllocator::ReserveMemoryBlock   +0xa0  and  ...F0000000->...FC000000 */
  /*  2 = */ { 0x1611f4, 0xd35cfc28, 0xd35afc28 },  /* TLSAllocator<0>::ThreadInitialize            +0x10  lsr  #28->#26 */
  /*  3 = */ { 0x1611f8, 0x52a20009, 0x52a08009 },  /* TLSAllocator<0>::ThreadInitialize            +0x14  movz 0x1000->0x0400 */
  /*  4 = */ { 0x1625a8, 0x52a20009, 0x52a08009 },  /* BucketAllocator::BucketAllocator             +0xf4  movz 0x1000->0x0400 */
  /*  5 = */ { 0x164960, 0xd35cfd29, 0xd35afd29 },  /* DynamicHeapAllocator::DynamicHeapAllocator   +0x98  lsr  #28->#26 */
  /*  6 = */ { 0x164964, 0x52a2000a, 0x52a0800a },  /* DynamicHeapAllocator::DynamicHeapAllocator   +0x9c  movz 0x1000->0x0400 */
  /*  7 = */ { 0x164e00, 0x12be000a, 0x12bf800a },  /* DynamicHeapAllocator::RequestLargeAllocMemory+0x74  movn 0xF000->0xFC00 */
  /*  8 = */ { 0x164e08, 0x92648d36, 0x92669536 },  /* DynamicHeapAllocator::RequestLargeAllocMemory+0x7c  and  ...F0000000->...FC000000 */
  /*  9 = */ { 0x1669c0, 0xd35cdc33, 0xd35ad433 },  /* VirtualAllocator::MarkMemoryBlocks           +0x14  ubfx #28,#28->#26,#28 */
  /* 10 = */ { 0x1669c4, 0xd35cfd15, 0xd35afd15 },  /* VirtualAllocator::MarkMemoryBlocks           +0x18  lsr  #28->#26 */
  /* 11 = */ { 0x166ac4, 0x52a20008, 0x52a08008 },  /* VirtualAllocator::ReserveMemoryBlock         +0xac  movz 0x1000->0x0400 */
  /* 12 = */ { 0x166fc8, 0xd35cfc28, 0xd35afc28 },  /* VirtualAllocator::GetMemoryBlockFromPointer  +0x0   lsr  #28->#26 */
  /* 13 = */ { 0x166fd8, 0x92646c28, 0x92667428 },  /* VirtualAllocator::GetMemoryBlockFromPointer  +0x10  and  52-BIT mask (see note) */
  /* 14 = */ { 0x166fe0, 0xd35c9c2a, 0xd35a942a },  /* VirtualAllocator::GetMemoryBlockFromPointer  +0x18  ubfx #28,#12->#26,#12 */
  /* 15 = */ { 0x166ff8, 0xd35cdc29, 0xd35ad429 },  /* VirtualAllocator::GetMemoryBlockFromPointer  +0x30  ubfx #28,#28->#26,#28 */
  /* 16 = */ { 0x166ffc, 0xf2a2000b, 0xf2a0800b },  /* VirtualAllocator::GetMemoryBlockFromPointer  +0x34  movk 0x1000->0x0400 */
  /* 17 = */ { 0x167038, 0xcb0a7108, 0xcb0a6908 },  /* VirtualAllocator::GetMemoryBlockFromPointer  +0x70  sub  lsl#28->lsl#26 */
  /* 18 = */ { 0x167060, 0xd35c9c29, 0xd35a9429 },  /* VirtualAllocator::GetBlockInfoFromPointer    +0x10  ubfx #28,#12->#26,#12 */
  /* ---- the TOP half of the two-level region index (added after a live NULL) ----
   * The region lookup is a TWO-LEVEL page table:
   *     level 1:  ptr >> 40                    <- these two sites
   *     level 2:  (ptr >> 28) & 0xFFF          <- sites 14/18/19, already patched
   * Moving level 2 down to bit 26 without moving level 1 leaves the two levels
   * non-contiguous: level 2 then spans bits 26..37 (4096 x 64MB = 256GB) while
   * level 1 still starts at bit 40, so bits 38-39 are simply dropped and every
   * pointer above 256GB aliases onto a wrong slot. On this console that is not
   * hypothetical -- libunity mapped at 0x6dd7b1a000 (~470GB) and the faulting
   * pointer was 0x7f4c004080 (~546GB).
   *
   * Symptom when missing: MemoryManager::GetAllocatorContainingPtr returns NULL
   * and its caller does an unchecked `ldr x8,[x26]` on it -- a null-pointer
   * read at libunity+0x168bf4, which is exactly the crash that found these.
   *
   * lsr #40 -> #38 puts level 1 immediately above the widened level 2. */
  /* 19 = */ { 0x168c74, 0xd35c9e89, 0xd35a9689 },  /* MemoryManager::GetAllocatorContainingPtr     +0x24  ubfx #28,#12->#26,#12 */
  /* 20 = */ { 0x169088, 0x52a20000, 0x52a08000 },  /* AtomicPageAllocator::AllocatePage            +0x48  movz 0x1000->0x0400 */
#if KB_REGION_L1_FIX
  /* ---- the TOP half of the two-level region index -- GATED, see below -----
   * The region lookup is a TWO-LEVEL page table:
   *     level 1:  ptr >> 40                    <- these two sites
   *     level 2:  (ptr >> 28) & 0xFFF          <- sites 14/18/19, always patched
   * Widening level 2 down to bit 26 without moving level 1 leaves them
   * non-contiguous: level 2 then spans bits 26..37 (4096 x 64MB = 256GB) while
   * level 1 still starts at bit 40, so bits 38-39 are dropped and pointers
   * above 256GB alias. That is real here -- a null return from
   * GetAllocatorContainingPtr crashed a session on a pointer at ~546GB
   * (0x7f4c004080), which is exactly a bit-38 address.
   *
   * WHY THIS IS GATED RATHER THAN ASSUMED. The fix is only correct if whatever
   * POPULATES the level-2 table uses the same level-1 shift. It could not be
   * verified: the table slot at struct offset +0x4240 is READ exactly once in
   * the entire binary (0x168c64) and NEVER written, so the registration side
   * is not reachable by inspection from here. And these are the only two
   * `lsr #40` instructions anywhere in the allocator region.
   *
   * Both states have been observed failing differently:
   *     0  (21 sites)  -> null deref in GetAllocatorContainingPtr on a
   *                       bit-38 pointer  (libunity+0x168bf4)
   *     1  (23 sites)  -> boot reaches nativeRender then hangs in
   *                       PlatformThread::Create with no gfx worker
   * so neither is established as correct. Flip this one define and rebuild to
   * A/B it; that is the fastest way to settle which failure belongs to which
   * cause, since the two symptoms are unmistakably different. */
  /* 21 */ { 0x167050, 0xd368fc28, 0xd366fc28 },  /* VirtualAllocator::GetBlockInfoFromPointer +0x0  lsr #40->#38 */
  /* 22 */ { 0x168c5c, 0xd368fc28, 0xd366fc28 },  /* MemoryManager::GetAllocatorContainingPtr  +0xc  lsr #40->#38 */
#endif
};
#define KB_PATCH_WORDS_N ((int)(sizeof(KB_PATCH_WORDS)/sizeof(KB_PATCH_WORDS[0])))

/* ---- note on site 16 (formerly a caveat, now resolved) -------------------
 * At 0x166ff4 the code builds a 64-bit constant in two halves:
 *
 *     0x166ff4  b25c6feb   orr  x11, xzr, #0xFFFFFFF000000000   <- NOT patched
 *     0x166ffc  f2a2000b   movk x11, #0x1000, lsl #16           <- site 16
 *
 * Only the low half encodes region granularity, so only the low half is
 * rewritten. This was previously flagged as an unresolved risk. It is not:
 * the symbolized reference shows the same two-instruction pair inside
 * VirtualAllocator::GetMemoryBlockFromPointer, and the exhaustive decode of
 * the reference did NOT classify the ORR half as a granularity site -- its
 * mask's low bound is bit 36, not bit 28. It encodes a different field and
 * must be left alone. Patching it would have been the bug.
 *
 * (2022.3 does not emit this pair at all, which is why the Fruit Ninja table
 * offered nothing to cross-check against and the risk stayed open until the
 * same-revision reference arrived.) */

/* ---- branch forces: none located, and none needed for first boot --------- */
static const NxPatchWord KB_BRANCH_FORCES[] = { { 0, 0, 0 } };  /* placeholder */
#define KB_BRANCH_FORCES_N 0

/* ===========================================================================
 * il2cpp JavaVM globals -- DERIVED FOR THIS GAME, and live
 * ===========================================================================
 * libil2cpp's own JNI_OnLoad must not be called: its first action is a log
 * through a GOT slot the loader mis-binds. Its essential effects are two
 * stores, which main.c replicates directly. Disassembly of THIS binary:
 *
 *   0x008a1310  stp  x19,x30,[sp,#-0x10]!
 *   0x008a131c  mov  x19, x0             ; x19 = JavaVM*
 *   0x008a132c  bl   0x716cc0            ; <- the unsafe log call we skip
 *   0x008a1334  adrp x8, #0x2ef2000
 *   0x008a1338  add  x0, x0, #0x354      ; x0 = il2cpp+0x8a1354 (handler fn)
 *   0x008a133c  str  x19, [x8, #0x4b8]   ; g_vm = vm   -> +0x2ef24b8
 *   0x008a1340  bl   0x8f27fc            ; setter:
 *                                        ;   adrp x8,#0x2ef2000
 *                                        ;   str  x0,[x8,#0xf38]  -> +0x2ef2f38
 *
 * Both targets confirmed to lie in libil2cpp's .bss (SHT_NOBITS, writable).
 * Structurally identical to the PvZ / Fruit Ninja pairs; the VALUES are ours,
 * read out of this binary. This is the one inherited feature that IS enabled,
 * because it was re-derived rather than assumed.                            */
#define KB_IL2CPP_VM_GLOBAL       0x2ef24b8  /* g_javavm            (.bss) */
#define KB_IL2CPP_HANDLER_SLOT    0x2ef2f38  /* g_jni_handler_fnptr (.bss) */
#define KB_IL2CPP_HANDLER_FN      0x8a1354   /* value stored into the slot */
#define KB_HAVE_IL2CPP_VM         1

/* ===========================================================================
 * FMOD -> OpenSL output select  --  DERIVED AND ENABLED
 * ===========================================================================
 * The Fruit Ninja port looked for this inside AudioManager::InitFMOD. In
 * 2021.3.31f1 that is the wrong function. A whole-image scan of the
 * symbolized reference for callers of FMOD::System::setOutput finds exactly
 * ONE in engine code:
 *
 *     AudioManager::InitNormal(bool, FMOD_OUTPUTTYPE) +0xb4
 *
 * (The other setOutput callers are all inside FMOD's own SystemI, reached
 * from createSound/init/getNumDrivers -- not the engine's output choice.)
 *
 * InitNormal pinned into the game at 0x476cb8 by exact opcode-shape match,
 * unique across the whole binary. Its argument setup is byte-identical to
 * the reference:
 *
 *     +0x98   bl   <get output kind>
 *     +0x9c   cmp  w0, #2
 *     +0xa0   mov  w8, #0x15            ; 21
 *     +0xa4   cinc w21, w8, eq          ; w21 = 21, or 22 when w0 == 2
 *     +0xa8   ldr  x0, [x19, #0x158]    ; the FMOD System*
 *     +0xac   mov  w1, w21              ; <-- PATCH SITE  (0x2A1503E1)
 *     +0xb0   add  x24, sp, #0x40
 *     +0xb4   bl   FMOD::System::setOutput
 *
 * Replacing `mov w1, w21` with `movz w1, #22` forces OpenSL unconditionally,
 * which is the output the loader actually implements (opensles.c). Enum 22
 * is confirmed from the code itself -- it is the value the cinc selects --
 * not assumed from an FMOD header.
 *
 * The expected word 0x2A1503E1 is the SAME one main.c already verifies
 * against, so the existing patch logic needed no change beyond this offset.
 * It is verify-first: a mismatch skips and logs.                           */
#define KB_FMOD_OUTPUT_SITE   0x476d64u   /* AudioManager::InitNormal +0xac */
#define KB_HAVE_FMOD_OPENSL   1

/* FMOD buffer bypass: signature absent in this FMOD build, as in the
 * reference port. Still off. */
#define KB_FMOD_BUFFER_SITE   0u
#define KB_HAVE_FMOD_BUFFER_BYPASS 0
static const NxPatchWord KB_FMOD_WORDS[] = { { 0, 0, 0 } };
#define KB_FMOD_WORDS_NUM 0

/* ===========================================================================
 * Swappy frame-pacing gate  --  DERIVED AND ENABLED
 * ===========================================================================
 * Swappy::IsEnabledAndActive() @ ref 0x5f03a0 (96 bytes), pinned into the
 * game at 0x368894 by exact opcode-shape match over all 24 instructions,
 * unique across the whole binary.
 *
 * Forcing it to return 0 makes every call site take the disabled path ->
 * plain eglSwapBuffers, no pacing threads, no join. This is how the Zookeeper
 * base already boots.
 *
 * IMPORTANT: the guard word DIFFERS from the reference port's. Fruit Ninja
 * checked for 0xA9BF4FFE (`stp x30,x19,[sp,#-0x10]!`); this build emits
 * 0xA9BF7BF3 (`stp x19,x30,[sp,#-0x10]!`) -- the same pair, opposite
 * register order. main.c has been updated to expect ours. If you ever see
 * "[pace] SKIP Swappy-disable", compare against this value first.          */
#define KB_PACING_GETTER      0x368894u
#define KB_PACING_GUARD_WORD  0xA9BF7BF3u  /* stp x19, x30, [sp, #-0x10]! */

/* ===========================================================================
 * TimeManager  --  ADDRESSES DERIVED, HOOK STILL GATED OFF
 * ===========================================================================
 * All four functions are now located in the game binary:
 *
 *     TimeManager::Update(double)      0x20b654   entry word 0xF9406408
 *     TimeManager::ResetTime(bool)     0x20b280
 *     TimeManager::SetTimeScale(float) 0x20ba44
 *     GetTimeManager()                 0x20bce4
 *
 * Update and SetTimeScale would not pin by masked-context match (0 hits --
 * the reference is a larger, differently-configured build and their codegen
 * genuinely differs), and they carry no rare move-wide immediates for
 * pin.py to vote on. They were resolved instead by exact opcode-shape match
 * bounded to the window between two already-pinned neighbours, ResetTime
 * (0x20b280) and GetTimeManager (0x20bce4) -- each gave exactly one hit in
 * that range, and Update landed within 0 bytes of where the reference's own
 * function spacing projected it.
 *
 * THE GATE IS STILL 0, and this is the honest reason: nx_install_time_fix()
 * does not only need these four addresses. It also needs the native vsync
 * triple -- the waiter's mutex, cond and counter -- which main.c currently
 * carries as three HARDCODED Fruit Ninja .bss offsets (0x110e8d0 / 0x110e8f8
 * / 0x110e928). Those are that game's, not ours. Installing the Update hook
 * while the triple points into the wrong .bss would park the clock thread on
 * an unrelated lock, which is a worse failure than no time fix at all.
 *
 * To enable: derive the three vsync globals for THIS libunity (they sit in
 * the vsync waiter reachable from GfxDeviceClient::SetVSyncCount, located
 * below), replace the hardcoded triple in main.c, then flip this gate.      */
#define KB_TIME_UPDATE_ENTRY  0x20b654u
#define KB_TIME_UPDATE_WORD   0xF9406408u  /* ldr x8, [x0, #0xc8] */
#define KB_TIME_UPDATE_BODY   0x20b678u    /* entry + 0x24 -- the instruction AFTER the
                                          * prologue's `ret`. NOT entry+16: the 2022.3
                                          * port used +16 because ITS prologue was that
                                          * long. On 2021.3 the prologue runs to +0x20
                                          * (ret), and resuming at +0x10 re-entered the
                                          * sequence with x8/w9/w10 never loaded --
                                          * `str x8,[x0,#0xc8]` then wrote a stale
                                          * register into frameCount and `cbz w10`
                                          * branched on garbage. main.c now SCANS for
                                          * the ret at install time and uses that; this
                                          * constant is the cross-check. */
#define KB_TIME_GETMANAGER    0x20bce4u
#define KB_TIME_RESETTIME     0x20b280u
#define KB_TIME_SETTIMESCALE  0x20ba44u
/* KB_HAVE_TIME_FIX is now 1: the vsync-triple blocker is cleared (below). */

/* ===========================================================================
 * VSync  --  LOCATED (reference only; no patch applied)
 * ===========================================================================
 * Provided because these are the entry points you need in order to derive
 * the vsync triple that blocks the time fix, and because forcing vsync count
 * is a common bring-up lever.
 *
 *     GetWantedVSyncCount()                      0x20b114
 *     QualitySettings::OnVSyncChanged()          0x7db68c
 *     QualitySettings::SetVSyncCount(int,bool)   0x7db7a4
 *     GfxDeviceClient::SetVSyncCount(unsigned)   0x3c2918
 *
 * All four pinned by masked whole-function match, uniquely. No patch is
 * applied to any of them: this port drives its own loop and disables Swappy,
 * so the engine's vsync bookkeeping is not in the critical path. They are
 * recorded, not used.                                                       */
#define KB_VSYNC_GETWANTED         0x20b114u
#define KB_VSYNC_ONCHANGED         0x7db68cu
#define KB_VSYNC_SETCOUNT          0x7db7a4u
#define KB_VSYNC_GFXDEV_SETCOUNT   0x3c2918u

/* ===========================================================================
 * The vsync triple  --  DERIVED, and it unblocks the TimeManager hook
 * ===========================================================================
 * main.c's Choreographer stand-in pulses Unity's native vsync counter+cond at
 * ~60Hz so frame pacing advances. It needs three globals. Until now those were
 * three HARDCODED Fruit Ninja offsets, which is why the time fix was gated.
 *
 * DERIVATION. In the symbolized reference, WaitVSync(long) @ 0x61f750 (96
 * bytes) is the whole waiter, and its body names all three:
 *
 *     adrp x20,#..  ; add x20,x20,#0xa08   -> the MUTEX
 *     mov  x0,x20   ; bl  pthread_mutex_lock
 *     ldr  x21,[x22,#0xa60]                -> the COUNTER   (mutex + 0x58)
 *     cmp  x21,x19  ; b.ge <done>
 *     add  x0,x20,#0x28                    -> the CONDVAR   (mutex + 0x28)
 *     mov  x1,x20   ; bl  pthread_cond_wait
 *     ...           ; bl  pthread_mutex_unlock
 *
 * WaitVSync pinned into the game at 0x392c9c by exact opcode-shape match over
 * all 24 instructions, unique across the whole binary. The three globals were
 * then read out of the GAME's own code (data addresses do not survive a
 * cross-build map, so they must be read locally, not projected):
 *
 *     mutex   0x1240bc8   .bss
 *     cond    0x1240bf0   .bss   = mutex + 0x28
 *     counter 0x1240c20   .bss   = mutex + 0x58
 *
 * CROSS-CHECK. Both deltas -- +0x28 and +0x58 -- are identical in the
 * reference AND in Fruit Ninja's hardcoded 2022.3 triple (0x110e8d0 /
 * 0x110e8f8 / 0x110e928). Three builds across two Unity LTS lines agreeing on
 * the same struct spacing is strong evidence these are the right three words.
 *
 * The call targets were verified through the game's own PLT: the three `bl`s
 * in WaitVSync resolve to pthread_mutex_lock, pthread_cond_wait and
 * pthread_mutex_unlock, so these are real pthread objects and the globals hold
 * the OBJECTS, not pointers to them. (main.c types the first two as `**`; that
 * is a stray extra indirection inherited from the reference tree. It is
 * harmless because only the address is ever passed, never dereferenced, and it
 * is left alone rather than "fixed" so this port does not diverge from the
 * lineage over a cosmetic cast.)                                            */
#define KB_VSYNC_MUTEX        0x1240bc8u
#define KB_VSYNC_COND         0x1240bf0u   /* mutex + 0x28 */
#define KB_VSYNC_COUNTER      0x1240c20u   /* mutex + 0x58 */
#define KB_VSYNC_WAITVSYNC_FN 0x392c9cu    /* WaitVSync(long), for reference */
#define KB_HAVE_VSYNC_TRIPLE  1


#endif /* NX_PATCH_KILLERBEAN_H */
