/* Offline entitlement restore. See kb_purchases.c. */
#ifndef KB_PURCHASES_H
#define KB_PURCHASES_H
#include <stdint.h>
void kb_purchases_init(const char *data_root, uintptr_t il2cpp_base);
void kb_purchase_weapons_pack(void *self, void *method);
void kb_purchase_unlimited_ammo(void *self, void *method);
#endif
