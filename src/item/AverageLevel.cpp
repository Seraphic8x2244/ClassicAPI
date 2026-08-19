// This file is part of ClassicAPI.
//
// ClassicAPI is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// ClassicAPI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.

// `GetAverageItemLevel()` → `(avgItemLevel, avgItemLevelEquipped)` —
// polyfills the modern 2.4+ global with the same 2-tuple shape as
// 4.3.4's `Script_GetAverageItemLevel` (`FUN_0097EE60` in the
// Proudmoore client).
//
//   avgItemLevelEquipped — mean of `m_itemLevel` over currently-worn
//                          slots 1..19 (skipping shirt and tabard).
//                          Empty slots count as 0, denominator is
//                          fixed at the considered-slot count, so
//                          removing gear always lowers this value.
//
//   avgItemLevel         — same denominator, but each per-slot ilvl
//                          is built up by greedy single-assignment:
//                          equipped items seed each slot, then
//                          bag + bank items are sorted by ilvl
//                          descending and each gets assigned to ONE
//                          candidate slot (the one with the lowest
//                          current best). A trinket in your bag
//                          fills one trinket slot, not both —
//                          without that, an item with a multi-slot
//                          invType would double-count and drag the
//                          mean down when moved off the body.
//
// Bank IS walked. Vanilla's `GetItemBySlot` gates bank slots on a
// banker-GUID that's only set while the bank window is open, but
// the underlying GUID array at `invMgr + OFF_INVMGR_GUID_ARRAY` is
// populated from server data at login. We bypass the gate the same
// way `C_Item.GetItemCount` does — read the GUID array directly,
// resolve each via `FUN_OBJECT_RESOLVE_BY_GUID`. Works without the
// bank window ever being opened.
//
// Max-of-two-divisors fairness. A 2H-weapon wielder has an
// intentionally empty offhand (slot 17); the fixed-denominator
// approach would penalize them. We compute a second average
// excluding slot 17 from both numerator and denominator, and
// return whichever is higher — same trick the 4.3.4 client uses
// at `FUN_0097E0F0`'s tail.
//
// Slot-mask table below mirrors the one in
// `GetInventoryItemsForSlot` — vanilla rules: 2H/MAINHAND confined
// to slot 16, no Titan's Grip.

#include "Game.h"
#include "Offsets.h"
#include "item/Data.h"
#include "item/ID.h"
#include "item/Location.h"
#include "item/Record.h"
#include "object/Resolve.h"
#include "unit/Identity.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Item::AverageLevel {

namespace {

// 1-based slot range used by both walks.
constexpr int kFirstSlot = 1;
constexpr int kLastSlot = 19;

// Excluded from the average — shirt (4) and tabard (19) have ilvl 1
// and would drag the mean. Modern `GetAverageItemLevel` excludes
// these too.
bool IsExcludedSlot(int slot) {
    return slot == 4 || slot == 19;
}

// invType → bitmask of equipment slots the item can occupy (bit N =
// 1-based slot N+1). Mirrors `kSlotMaskByInvType` in
// `InventoryItemsForSlot.cpp`; kept private here to avoid coupling
// two modules through a header that only carries one table.
constexpr uint32_t kSlotMaskByInvType[29] = {
    0x00000000, // 0  NON_EQUIP
    0x00000001, // 1  HEAD         → slot 1
    0x00000002, // 2  NECK         → slot 2
    0x00000004, // 3  SHOULDER     → slot 3
    0x00000008, // 4  BODY (shirt)
    0x00000010, // 5  CHEST        → slot 5
    0x00000020, // 6  WAIST        → slot 6
    0x00000040, // 7  LEGS         → slot 7
    0x00000080, // 8  FEET         → slot 8
    0x00000100, // 9  WRIST        → slot 9
    0x00000200, // 10 HANDS        → slot 10
    0x00000C00, // 11 FINGER       → slots 11, 12
    0x00003000, // 12 TRINKET      → slots 13, 14
    0x00018000, // 13 WEAPON (1H)  → slots 16, 17
    0x00010000, // 14 SHIELD       → slot 17
    0x00020000, // 15 RANGED       → slot 18
    0x00004000, // 16 CLOAK        → slot 15
    0x00008000, // 17 2HWEAPON     → slot 16 (vanilla — no Titan's Grip)
    0x00780000, // 18 BAG          → slots 20..23
    0x00040000, // 19 TABARD
    0x00000010, // 20 ROBE         → slot 5
    0x00008000, // 21 WEAPONMAINHAND
    0x00010000, // 22 WEAPONOFFHAND
    0x00010000, // 23 HOLDABLE
    0x00000000, // 24 AMMO
    0x00020000, // 25 THROWN       → slot 18
    0x00020000, // 26 RANGEDRIGHT  → slot 18
    0x00000000, // 27 QUIVER
    0x00020000, // 28 RELIC        → slot 18
};

constexpr int kInvTypeMax = static_cast<int>(sizeof(kSlotMaskByInvType) /
                                              sizeof(kSlotMaskByInvType[0])) - 1;

// Returns (itemLevel, inventoryType) for the CGItem, or (0, 0) if
// the itemID is unresolvable or the cache record hasn't loaded.
struct ItemInfo {
    uint32_t itemLevel;
    uint32_t invType;
};

ItemInfo ReadItemInfo(const uint8_t *cgItem) {
    if (cgItem == nullptr)
        return {0, 0};
    const int itemID = Item::ID::FromCGItem(cgItem);
    if (itemID <= 0)
        return {0, 0};
    const uint8_t *record = Item::PeekRecord(static_cast<uint32_t>(itemID));
    if (record == nullptr) {
        // Equipped or bagged item with no cache record yet — queue a
        // warmup so the next call gets populated data. Cheap on cache
        // hit; fires CMSG_ITEM_QUERY_SINGLE on miss.
        Item::Data::WarmCache(static_cast<uint32_t>(itemID));
        return {0, 0};
    }
    return {Game::Read<uint32_t>(record, Offsets::OFF_ITEMSTATS_ITEM_LEVEL),
            Game::Read<uint32_t>(record, Offsets::OFF_ITEMSTATS_INVENTORY_TYPE)};
}

// Bank-walk helpers — bypass the bank-window gate on `GetItemBySlot`
// by reading the player's invMgr GUID array directly. Same shape as
// `C_Item.GetItemCount`'s bank path.
const uint8_t *ResolveByGuid(int type, uint64_t guid) {
    if (guid == 0)
        return nullptr;
    return static_cast<const uint8_t *>(
        Object::ByGuid(type, guid, "AverageLevel", 0x172));
}

// Collects a single CGItem into `candidates` if it could equip in
// any non-excluded slot. Each item is added once and assigned to a
// single slot later (greedy fit) — keeps trinkets/fingers/1H
// weapons from double-counting across their two candidate slots.
struct Candidate {
    uint32_t ilvl;
    uint32_t mask; // bitmask of candidate slots (1-based bit positions)
};

void CollectBagOrBankItem(const uint8_t *cgItem, std::vector<Candidate> &candidates) {
    const ItemInfo info = ReadItemInfo(cgItem);
    if (info.itemLevel == 0)
        return;
    if (info.invType > static_cast<uint32_t>(kInvTypeMax))
        return;
    uint32_t mask = kSlotMaskByInvType[info.invType];
    // Strip excluded slots (shirt, tabard) from the mask up front.
    for (int s = kFirstSlot; s <= kLastSlot; ++s) {
        if (IsExcludedSlot(s))
            mask &= ~(1u << (s - 1));
    }
    if (mask == 0)
        return;
    candidates.push_back({info.itemLevel, mask});
}

// Walks a contiguous GUID-array range and feeds each resolved item
// through `CollectBagOrBankItem`.
void WalkGuidArrayRange(const uint8_t *invMgr, int firstSlot, int lastSlot,
                        std::vector<Candidate> &candidates) {
    if (invMgr == nullptr)
        return;
    auto *guidArray = Game::Read<const uint64_t *>(
        invMgr, Offsets::OFF_INVMGR_GUID_ARRAY);
    if (guidArray == nullptr)
        return;
    for (int slot = firstSlot; slot <= lastSlot; ++slot) {
        const uint8_t *item = ResolveByGuid(Offsets::TYPEMASK_ITEM, guidArray[slot]);
        if (item == nullptr)
            continue;
        CollectBagOrBankItem(item, candidates);
    }
}

// Walks each equipped bank bag's contents (slots 63..68 in the
// player invMgr hold the bank bags themselves; each bag has its own
// invMgr accessed via the bag CGContainer's vtable+0x10 method).
void WalkBankBags(std::vector<Candidate> &candidates) {
    auto *playerInvMgr = Unit::Identity::PlayerInventoryManager();
    if (playerInvMgr == nullptr)
        return;
    auto *playerGuidArray = Game::Read<const uint64_t *>(
        playerInvMgr, Offsets::OFF_INVMGR_GUID_ARRAY);
    if (playerGuidArray == nullptr)
        return;
    for (int slot = Offsets::INVMGR_BANK_BAG_FIRST_SLOT;
         slot <= Offsets::INVMGR_BANK_BAG_LAST_SLOT; ++slot) {
        const uint8_t *bag = ResolveByGuid(Offsets::TYPEMASK_CONTAINER,
                                            playerGuidArray[slot]);
        if (bag == nullptr)
            continue;
        auto *bagInvMgr =
            static_cast<const uint8_t *>(Item::Location::ContainerInventory(bag));
        if (bagInvMgr == nullptr)
            continue;
        const int bagSlots =
            static_cast<int>(*reinterpret_cast<const uint32_t *>(bagInvMgr));
        if (bagSlots <= 0)
            continue;
        WalkGuidArrayRange(bagInvMgr, 0, bagSlots - 1, candidates);
    }
}

static int __fastcall Script_GetAverageItemLevel(void *L) {
    // Per-slot best ilvl found anywhere in the player's inventory.
    // Equipped slot ilvls are tracked separately so the second return
    // value can sum only the equipped path.
    uint32_t bestPerSlot[20] = {0};
    uint32_t equippedIlvl[20] = {0};

    // First pass: equipped items. Direct path via `ResolveEquipmentSlot`
    // — no Lua stack interaction.
    for (int slot = kFirstSlot; slot <= kLastSlot; ++slot) {
        if (IsExcludedSlot(slot))
            continue;
        const uint8_t *cgItem = Item::Location::ResolveEquipmentSlot(slot);
        if (cgItem == nullptr)
            continue;
        const ItemInfo info = ReadItemInfo(cgItem);
        if (info.itemLevel == 0)
            continue;
        equippedIlvl[slot] = info.itemLevel;
        bestPerSlot[slot] = info.itemLevel;
    }

    // Second pass: collect bag + bank items as candidates. `ResolveBag`
    // stomps the Lua stack on every call — we don't have anything on
    // the stack we need to keep yet (return-value pushes happen at
    // the very end).
    std::vector<Candidate> candidates;
    for (int bag = 0; bag <= 4; ++bag) {
        const int slotCount = Item::Location::GetBagSlotCount(bag);
        for (int bs = 1; bs <= slotCount; ++bs) {
            CollectBagOrBankItem(Item::Location::ResolveBag(L, bag, bs), candidates);
        }
    }
    // Bank — direct GUID-array reads bypass the bank-window gate so
    // this works without the player ever having opened the bank this
    // session — GUIDs are populated from server data at login. Main
    // bank first (linear slots 39..62 in the player invMgr), then
    // each equipped bank bag's contents via the per-bag CGContainer.
    const uint8_t *playerInvMgr = Unit::Identity::PlayerInventoryManager();
    WalkGuidArrayRange(playerInvMgr,
                       Offsets::INVMGR_BANK_MAIN_FIRST_SLOT,
                       Offsets::INVMGR_BANK_MAIN_LAST_SLOT,
                       candidates);
    WalkBankBags(candidates);

    // Third pass: greedy assignment. Sort candidates by ilvl
    // descending; for each, find its candidate slot with the LOWEST
    // current `bestPerSlot` value and update if the candidate beats
    // it. Each item is assigned at most once — a trinket in the bag
    // fills one trinket slot, not both.
    //
    // Note on weapons: a 2H weapon (`invType=17`) is masked to slot
    // 16 only, while a 1H (`invType=13`) gets both slots 16 and 17.
    // If the player has a 2H equipped and a 1H in the bag, the 1H
    // will land in slot 17 — effectively "available offhand". Modern
    // WoW's full assignment includes a 2H-vs-1H+OH comparison; we
    // skip that optimization (vanilla had no Titan's Grip and the
    // rare cases where it'd matter aren't worth the complexity).
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
                  return a.ilvl > b.ilvl;
              });
    for (const auto &c : candidates) {
        int bestSlot = -1;
        uint32_t bestSlotVal = 0;
        for (int s = kFirstSlot; s <= kLastSlot; ++s) {
            if ((c.mask & (1u << (s - 1))) == 0)
                continue;
            if (bestSlot < 0 || bestPerSlot[s] < bestSlotVal) {
                bestSlot = s;
                bestSlotVal = bestPerSlot[s];
            }
        }
        if (bestSlot >= 0 && c.ilvl > bestSlotVal)
            bestPerSlot[bestSlot] = c.ilvl;
    }

    // Fixed denominator = all considered equipment slots (1..19 minus
    // shirt + tabard = 17 slots). Empty slots contribute 0 to the
    // sum but still count toward the denominator — matches modern's
    // fixed-slot-count behavior (verified against retail Wow.exe:
    // `552.4375 * 16 = 8839` exactly, regardless of populated count).
    //
    // Max-of-two-divisors fairness trick: a 2H-weapon wielder has an
    // intentionally empty offhand (slot 17). Counting that empty
    // slot as 0 in a 17-divisor sum unfairly penalizes them vs a
    // sword+shield wielder. We compute a second candidate excluding
    // slot 17 from both numerator and denominator (16 slots), and
    // return the max — same trick the 4.3.4 client uses at
    // `FUN_0097E0F0`'s tail. For sword+shield the OH ilvl is
    // typically near average so the all-17 path wins; for 2H wielders
    // the no-OH path wins.
    uint64_t equippedSum = 0;
    uint64_t overallSum = 0;
    int slotCount = 0;
    for (int s = kFirstSlot; s <= kLastSlot; ++s) {
        if (IsExcludedSlot(s))
            continue;
        equippedSum += equippedIlvl[s];
        overallSum += bestPerSlot[s];
        ++slotCount;
    }

    constexpr int kOffhandSlot = 17;

    auto bestAvg = [&](uint64_t sumAll, uint32_t ohContribution) {
        if (slotCount <= 0)
            return 0.0;
        const double allAvg = static_cast<double>(sumAll) / slotCount;
        const int reducedCount = slotCount - 1;
        if (reducedCount <= 0)
            return allAvg;
        const double noOhAvg =
            static_cast<double>(sumAll - ohContribution) / reducedCount;
        return (noOhAvg > allAvg) ? noOhAvg : allAvg;
    };

    const double equippedAvg = bestAvg(equippedSum, equippedIlvl[kOffhandSlot]);
    const double overallAvg = bestAvg(overallSum, bestPerSlot[kOffhandSlot]);

    Game::Lua::SetTop(L, 0);
    Game::Lua::PushNumber(L, overallAvg);
    Game::Lua::PushNumber(L, equippedAvg);
    return 2;
}

} // namespace

static void RegisterLuaFunctions() {
    Game::Lua::RegisterGlobalFunction("GetAverageItemLevel", &Script_GetAverageItemLevel);
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace Item::AverageLevel
