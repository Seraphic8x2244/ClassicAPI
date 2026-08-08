// This file is part of ClassicAPI.
//
// ClassicAPI is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// ClassicAPI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// ClassicAPI. If not, see <https://www.gnu.org/licenses/>.

// `GetCursorInfo()` — backport of the modern WoW global. Surfaces
// whatever the player has picked up (item, money, spell, macro,
// merchant slot) as a tuple. Read from the same cursor-state globals
// the engine's `CursorHasItem` / `CursorHasSpell` / `GetCursorMoney`
// consult — see the cursor type table in [[src/Offsets.h]].
//
// Return shape per type (matches modern Lua API where it makes sense
// for 1.12's actual storage):
//
//   nothing on cursor       — nil
//   "item", itemID, link    — bag item (type 1; link is the full
//                             per-instance form via the engine's link
//                             builder, including enchantID and
//                             random-suffix decoration)
//   "item", itemID          — drag-source item (types 5/6/7 trade /
//                             mail / action bar; type 9 equipment
//                             slot) — only the itemID is in cursor
//                             storage, no live CGItem to feed the
//                             link builder
//   "money", amount         — type 2; amount in copper
//   "spell", slot, bookType, spellID
//                           — type 3; bookType is "spell" or "pet"
//                             (1.12 only writes player-spell IDs to
//                             this slot, so bookType is always
//                             "spell" here; the cursor's "pet
//                             action" path is a distinct type)
//   "macro", index          — type 8; 1-based macro index
//   "merchant", index       — type 10; 1-based merchant-roster index
//
// Type 4 (pet action) is not surfaced — doesn't map cleanly to a
// modern `GetCursorInfo` string. Consumers can fall back to
// `CursorHasItem` / engine `PickupX` calls when they need to detect
// that.

#include "cursor/Info.h"

#include "Game.h"
#include "Offsets.h"
#include "item/ID.h"
#include "item/Link.h"
#include "item/Location.h"
#include "item/QualityColor.h"
#include "item/Record.h"
#include "spell/Lookup.h"

#include <cstdint>
#include <cstdio>

namespace Cursor::Info {

namespace {

// Engine signature is `__thiscall(this = typeFilter, guidLo, guidHi,
// unused)`. The "this" param is just a small integer filter — Ghidra
// shows it cast as `(void *)0x2` at every call site for items. The
// function reads only the first two stack args (`[ebp+8]` and
// `[ebp+0xC]`) but cleans 12 bytes via `ret 0xc`, so the caller must
// push a third dword for stack balance — engine callers push a
// scratch dword (`0x90`, `0x1C13`, etc.) for this. Declaring a 4th
// arg here makes MSVC emit the matching push; without it, stack
// imbalance of -4 bytes silently corrupts the caller's frame and
// (downstream) wedges things like chat input.
using ObjectFromGUID_t = uint8_t *(__thiscall *)(uintptr_t typeFilter,
                                                  uint32_t guidLo, uint32_t guidHi,
                                                  uint32_t unused);

constexpr uint32_t TYPE_EMPTY = 0;
constexpr uint32_t TYPE_BAG_ITEM = 1;
constexpr uint32_t TYPE_MONEY = 2;
constexpr uint32_t TYPE_SPELL = 3;
// Type 5 = merchant pickup. Written by `Script_PickupMerchantItem`
// (`0x004FB760`) via `FUN_004950F0(itemID, ?, slot0Based, 5)`. The
// itemID lands in `VAR_CURSOR_GENERIC_SLOT` and the 0-based merchant
// slot in `VAR_CURSOR_GENERIC_SOURCE` — we push slot+1 to Lua to
// match modern's 1-based merchant convention.
constexpr uint32_t TYPE_MERCHANT = 5;
// Types 6/7/9 — drag-source items from various pickup paths. All
// share the same storage trio. Type 7 is action-bar item pickup
// (`FUN_004E6130`), type 9 is equipment-slot pickup (`FUN_004C7300`),
// and type 6 is another drag-source path (mail or similar). In all
// cases the itemID is at `VAR_CURSOR_GENERIC_SLOT`. For modern API
// compat we surface them as `"item"`.
constexpr uint32_t TYPE_GENERIC_ITEM_LO = 6;
constexpr uint32_t TYPE_GENERIC_ITEM_HI = 7;
constexpr uint32_t TYPE_MACRO = 8;
constexpr uint32_t TYPE_INVENTORY_ITEM = 9;
// Type 10 is stable-pet pickup (`FUN_00495010`, the source of the
// earlier misidentification as "merchant"). Modern WoW has no
// equivalent `GetCursorInfo` return for stable pets, so we leave it
// as nil.

constexpr uint32_t GUID_TYPE_FILTER_ITEM = 0x2;

uint32_t ReadVar(uintptr_t va) {
    return *reinterpret_cast<const uint32_t *>(va);
}

int PushItem(void *L, const uint8_t *cgItem) {
    if (cgItem == nullptr)
        return 0;
    const int itemID = Item::ID::FromCGItem(cgItem);
    if (itemID <= 0)
        return 0;
    Game::Lua::PushString(L, "item");
    Game::Lua::PushNumber(L, static_cast<double>(itemID));
    // Engine link builder works fine on cursored items — the cursor
    // leaves the instance block at `+0x08` and the m_objectFields
    // descriptor at `+0x114` intact, which is all the builder reads
    // (plus a virtual call into the sub-object at `+0x110`, whose
    // vftable is also untouched while held on the cursor). Earlier
    // crash repros were the stack-imbalance bug in the GUID resolver
    // — see Offsets::FUN_OBJECT_FROM_GUID.
    const char *link = Item::Link::FromCGItem(cgItem);
    if (link != nullptr && *link != '\0') {
        Game::Lua::PushString(L, link);
        return 3;
    }
    return 2;
}

int PushBagItemCursor(void *L) {
    const uint32_t lo = ReadVar(Offsets::VAR_CURSOR_ITEM_GUID_LO);
    const uint32_t hi = ReadVar(Offsets::VAR_CURSOR_ITEM_GUID_HI);
    if (lo == 0 && hi == 0)
        return 0;
    auto resolver = reinterpret_cast<ObjectFromGUID_t>(Offsets::FUN_OBJECT_FROM_GUID);
    const uint8_t *cgItem = resolver(GUID_TYPE_FILTER_ITEM, lo, hi, 0);
    return PushItem(L, cgItem);
}

// Builds a basic `"|cffRRGGBB|Hitem:N:0:0:0|h[Name]|h|r"` link from
// the cached ItemStats record for `itemID`. No enchant / random-
// suffix decoration — the cursor only stores the bare itemID for
// drag-source items (types 6/7/9) with no live `CGItem *` to read
// instance state from. Returns true and writes into `out` (size
// `outSize`) on success; returns false if the item isn't cached.
bool BuildBasicItemLink(uint32_t itemID, char *out, size_t outSize) {
    const uint8_t *record = Item::PeekRecord(itemID);
    if (record == nullptr) return false;
    const char *name = *reinterpret_cast<const char *const *>(
        record + Offsets::OFF_ITEMSTATS_NAME);
    if (name == nullptr || *name == '\0') return false;
    const uint32_t quality = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_QUALITY);
    const int n = std::snprintf(out, outSize,
        "%s|Hitem:%u:0:0:0|h[%s]|h|r",
        Item::QualityColor::Prefix(static_cast<int>(quality)),
        itemID, name);
    return n > 0 && static_cast<size_t>(n) < outSize;
}

// Shared path for types 6/7 (action bar / mail drag) and type 9
// (equipment slot). All three write the itemID at
// `VAR_CURSOR_GENERIC_SLOT` — see the type-table notes in
// [[src/Offsets.h]]. The cursor doesn't preserve a GUID for these,
// so the live CGItem isn't directly addressable here. To recover
// enchant / random-suffix decoration we walk the player's
// equipment + bags looking for the first CGItem with a matching
// itemID — same trick the action-bar tooltip uses to display
// enchanted bracers correctly when they're on an action bar. Falls
// back to a basic ID-only link when no instance is found (item was
// deleted, or it's a vendor-roster reference with no local copy).
int PushGenericItemCursor(void *L) {
    const uint32_t itemID = ReadVar(Offsets::VAR_CURSOR_GENERIC_SLOT);
    if (itemID == 0)
        return 0;
    Game::Lua::PushString(L, "item");
    Game::Lua::PushNumber(L, static_cast<double>(itemID));

    Item::Location::ByGUIDResult found;
    if (Item::Location::FindByItemID(L, static_cast<int>(itemID), &found)) {
        const char *link = Item::Link::FromCGItem(found.item);
        if (link != nullptr && *link != '\0') {
            Game::Lua::PushString(L, link);
            return 3;
        }
    }

    char linkBuf[256];
    if (BuildBasicItemLink(itemID, linkBuf, sizeof(linkBuf))) {
        Game::Lua::PushString(L, linkBuf);
        return 3;
    }
    return 2;
}

int PushMoneyCursor(void *L) {
    const uint32_t copper = ReadVar(Offsets::VAR_CURSOR_MONEY_COPPER);
    Game::Lua::PushString(L, "money");
    Game::Lua::PushNumber(L, static_cast<double>(copper));
    return 2;
}

int PushSpellCursor(void *L) {
    const uint32_t spellID = ReadVar(Offsets::VAR_CURSOR_SPELL_ID);
    if (spellID == 0)
        return 0;
    int bookType = 0;
    const int slot = Spell::Lookup::FindSpellbookSlot(
        static_cast<int>(spellID), &bookType);
    Game::Lua::PushString(L, "spell");
    // Slot can be 0 (rare — talent-passive auras the player has but
    // that have no spellbook entry). Push it anyway; the modern API's
    // contract is "slot may be 0 when there's no book row".
    Game::Lua::PushNumber(L, static_cast<double>(slot));
    Game::Lua::PushString(L, bookType == 1 ? "pet" : "spell");
    Game::Lua::PushNumber(L, static_cast<double>(spellID));
    return 4;
}

int PushMacroCursor(void *L) {
    const uint32_t index = ReadVar(Offsets::VAR_CURSOR_MACRO_INDEX);
    if (index == 0)
        return 0;
    Game::Lua::PushString(L, "macro");
    Game::Lua::PushNumber(L, static_cast<double>(index));
    return 2;
}

int PushMerchantCursor(void *L) {
    // Engine stores the 0-based merchant slot at
    // `VAR_CURSOR_GENERIC_SOURCE`. Convert to 1-based for the modern
    // Lua-API contract (`MerchantItemButton`s and friends are
    // 1-indexed).
    const uint32_t slot0 = ReadVar(Offsets::VAR_CURSOR_GENERIC_SOURCE);
    Game::Lua::PushString(L, "merchant");
    Game::Lua::PushNumber(L, static_cast<double>(slot0 + 1));
    return 2;
}

int __fastcall Script_GetCursorInfo(void *L) {
    const uint32_t type = ReadVar(Offsets::VAR_CURSOR_TYPE);
    if (type >= TYPE_GENERIC_ITEM_LO && type <= TYPE_GENERIC_ITEM_HI)
        return PushGenericItemCursor(L);
    switch (type) {
        case TYPE_BAG_ITEM:       return PushBagItemCursor(L);
        case TYPE_MERCHANT:       return PushMerchantCursor(L);
        case TYPE_INVENTORY_ITEM: return PushGenericItemCursor(L);
        case TYPE_MONEY:          return PushMoneyCursor(L);
        case TYPE_SPELL:          return PushSpellCursor(L);
        case TYPE_MACRO:          return PushMacroCursor(L);
        case TYPE_EMPTY:
        default:                  return 0;
    }
}

void RegisterLuaFunctions() {
    Game::Lua::RegisterGlobalFunction("GetCursorInfo", &Script_GetCursorInfo);
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace

bool HasItem() {
    return ReadVar(Offsets::VAR_CURSOR_ITEM_GUID_LO) != 0 ||
           ReadVar(Offsets::VAR_CURSOR_ITEM_GUID_HI) != 0;
}

} // namespace Cursor::Info
