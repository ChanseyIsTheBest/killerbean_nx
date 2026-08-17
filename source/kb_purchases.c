/* kb_purchases.c -- restore entitlements the player already owns.
 *
 * Killer Bean's two paid items are Google Play IAPs. There is no Play billing on
 * this console, so the store can never initialise here: the very first managed
 * exception in every log is
 *
 *   NullReferenceException
 *     at UnityEngine.Purchasing.Models.GoogleBillingClient..ctor ()
 *     at ... StandardPurchasingModule.InstantiateGoogleStore ()
 *     at WeaponStore_IAP.InitializePurchasing ()
 *     at WeaponStore_IAP.Start ()
 *
 * which kills Start(), leaves the component's fields null, and is why pressing
 * either buy button used to throw. The entitlement check is not being defeated
 * here -- it is unreachable on this platform, and the player's Play receipts
 * cannot be presented to anything.
 *
 * So this reads <data_root>/purchases.txt and, for each item the player has
 * uncommented, calls THE GAME'S OWN grant method rather than inventing one:
 *
 *   WeaponStore_IAP.Owned_Weapons_Pack(self, bool is_new_purchase)   +0xA0CC3C
 *   WeaponStore_IAP.Owned_Unlimited_Ammo(self, bool is_new_purchase) +0xA0D3B8
 *
 * That matters. Those methods are what unlock the weapons, set the counts and
 * drive the game's own save -- reproducing any of that by poking fields would
 * be guesswork that drifts the moment the save format changes. is_new_purchase
 * is passed FALSE, which is the "you already own this, restore it quietly" path:
 * no cha-ching sound, no purchase popup.
 *
 * The hook sits on Purchase_Weapons_Pack / Purchase_Unlimited_Ammo, the two
 * buy-button handlers, because those are the only places a live
 * WeaponStore_IAP instance is handed to us. Pressing buy therefore restores the
 * item instead of contacting a store that is not there. Nothing is charged;
 * there is nothing on this platform to charge.
 *
 * An item left commented out in purchases.txt does nothing at all -- the button
 * press is swallowed and logged. That is deliberate: the file is the statement
 * of what you own, and the default state is owning nothing.
 */

#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "nx_patch_killerbean.h"   /* KB_IL2_Owned_* -- derived offsets */

#if KB_OFFLINE_PURCHASES

/* self, is_new_purchase, MethodInfo* -- the il2cpp calling convention for an
 * instance method with one bool argument. */
typedef void (*fn_owned)(void *self, uint8_t is_new_purchase, void *method);

static fn_owned g_owned_weapons;
static fn_owned g_owned_ammo;
static int      g_have_weapons;   /* uncommented in purchases.txt */
static int      g_have_ammo;
static int      g_done_weapons;   /* already granted this session */
static int      g_done_ammo;

#define KBP_NAME "purchases.txt"

static void kbp_write_default(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) { debugPrintf("[iap] could not create %s\n", path); return; }
  fprintf(f,
    "# Killer Bean Unleashed -- entitlements you already own.\n"
    "#\n"
    "# There is no Google Play billing on this console, so the game cannot ask\n"
    "# Play what you own and cannot sell you anything. This file is where you\n"
    "# say it. Remove the '#' from a line to restore that item.\n"
    "#\n"
    "# Only uncomment what you actually bought on your Google Play account.\n"
    "#\n"
    "# After editing, open the in-game weapon store and press the buy button\n"
    "# for the item once. It is restored using the game's own \"already owned\"\n"
    "# path -- no purchase sound, no popup -- and saved with the game's normal\n"
    "# save, so you only need to do it once.\n"
    "\n"
    "#weapons_pack\n"
    "#unlimited_ammo\n");
  fclose(f);
  debugPrintf("[iap] wrote default %s (both items commented out)\n", path);
}

/* A line counts as owning the item when it is the bare name: no leading '#',
 * leading blanks ignored. Anything else -- including "# weapons_pack" -- is a
 * comment, because the file's whole job is to be unambiguous about what the
 * player is asserting. */
static void kbp_parse(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) { kbp_write_default(path); return; }

  char line[160];
  while (fgets(line, sizeof line, f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;
    char *e = p;
    while (*e && *e != '\n' && *e != '\r' && *e != ' ' && *e != '\t') e++;
    *e = 0;
    if      (!strcmp(p, "weapons_pack"))   g_have_weapons = 1;
    else if (!strcmp(p, "unlimited_ammo")) g_have_ammo = 1;
    else debugPrintf("[iap] %s: unknown item \"%s\" ignored\n", KBP_NAME, p);
  }
  fclose(f);
  debugPrintf("[iap] %s: weapons_pack=%s unlimited_ammo=%s\n", KBP_NAME,
              g_have_weapons ? "OWNED" : "not claimed",
              g_have_ammo    ? "OWNED" : "not claimed");
}

void kb_purchases_init(const char *data_root, uintptr_t il2cpp_base) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", data_root ? data_root : ".", KBP_NAME);
  kbp_parse(path);
  g_owned_weapons = (fn_owned)(il2cpp_base + KB_IL2_Owned_Weapons_Pack);
  g_owned_ammo    = (fn_owned)(il2cpp_base + KB_IL2_Owned_Unlimited_Ammo);
}

/* Replacements for the two buy-button handlers. Signature is
 * (WeaponStore_IAP *self, MethodInfo *) -- a public void instance method. */
void kb_purchase_weapons_pack(void *self, void *method) {
  (void)method;
  if (!g_have_weapons) {
    debugPrintf("[iap] weapons_pack is commented out in %s -- not restoring\n", KBP_NAME);
    return;
  }
  if (g_done_weapons) return;
  g_done_weapons = 1;
  debugPrintf("[iap] restoring weapons_pack via Owned_Weapons_Pack(self=%p, new=false)\n", self);
  g_owned_weapons(self, 0, NULL);
}

void kb_purchase_unlimited_ammo(void *self, void *method) {
  (void)method;
  if (!g_have_ammo) {
    debugPrintf("[iap] unlimited_ammo is commented out in %s -- not restoring\n", KBP_NAME);
    return;
  }
  if (g_done_ammo) return;
  g_done_ammo = 1;
  debugPrintf("[iap] restoring unlimited_ammo via Owned_Unlimited_Ammo(self=%p, new=false)\n", self);
  g_owned_ammo(self, 0, NULL);
}

#endif /* KB_OFFLINE_PURCHASES */
