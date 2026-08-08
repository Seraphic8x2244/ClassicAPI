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

// `GetItemSpell(item)` — modern API returning `(spellName, spellID)`
// for items with an on-use spell effect (potions, trinkets, scrolls,
// hearthstone, food/drink with auras, etc.), or `nil` for items
// without one (gear with passive on-equip procs is excluded — those
// have trigger=1, not the on-use trigger=0 we look for).
//
// Vanilla 1.12 doesn't expose this; addons that needed it (e.g. to
// detect what spell a clicked trinket would fire, or to filter
// usable consumables) scraped the tooltip and parsed
// `ITEM_SPELL_TRIGGER_ONUSE` localized strings — locale-fragile and
// slow. This reads it directly off the cached `ItemStats_C` record.

#include "item/Spell.h"

#include "Game.h"
#include "Offsets.h"
#include "item/Arg.h"
#include "item/Data.h"
#include "item/Record.h"
#include "spell/Lookup.h"

#include <cstdint>

namespace Item::Spell {

namespace {

const char *SpellNameForID(int spellID) {
    const uint8_t *record = ::Spell::Lookup::RecordForID(spellID);
    if (record == nullptr)
        return nullptr;
    const int locale = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_LOCALE_INDEX));
    auto *const *names = reinterpret_cast<const char *const *>(
        record + Offsets::OFF_SPELL_NAMES);
    const char *name = names[locale];
    return (name != nullptr && name[0] != '\0') ? name : nullptr;
}

// `GetItemSpell(itemID | "item:NNN" | itemLink)` — returns
// `(spellName, spellID)` for the on-use spell, or nothing (nil) for
// items without one. Auto-warms the cache on miss like
// `GetItemFamily` does — first call returns nil, subsequent calls
// after `GET_ITEM_INFO_RECEIVED` succeed.
int __fastcall Script_GetItemSpell(void *L) {
    const int itemID = Item::Arg::ResolveItemID(L, 1);
    if (itemID <= 0)
        return 0;
    const uint8_t *record = Item::PeekRecord(static_cast<uint32_t>(itemID));
    if (record == nullptr) {
        Item::Data::WarmCache(static_cast<uint32_t>(itemID));
        return 0;
    }
    const int spellID = FindOnUseSpellIDInRecord(record);
    if (spellID <= 0)
        return 0;
    const char *name = SpellNameForID(spellID);
    if (name == nullptr)
        return 0;
    Game::Lua::PushString(L, name);
    Game::Lua::PushNumber(L, static_cast<double>(spellID));
    return 2;
}

} // namespace

// Walks the item's 5 spell slots looking for the first ON_USE
// (trigger=0) entry. Modern `GetItemSpell` returns only this kind;
// passive-on-equip procs (trigger=1), weapon procs (trigger=2),
// soulstones (trigger=4), and recipe-learn entries (trigger=6) all
// stay invisible to the API.
//
// Vanilla server data on Turtle WoW has been observed to put the
// ON_USE entry at slot index 0 for most items, but a few customized
// items use later slots, so we scan all 5.
int FindOnUseSpellIDInRecord(const uint8_t *record) {
    auto *spellIDs = reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_SPELL_ID);
    auto *triggers = reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_SPELL_TRIGGER);
    for (int i = 0; i < Offsets::ITEMSTATS_SPELL_SLOT_COUNT; i++) {
        if (spellIDs[i] != 0 &&
            triggers[i] == Offsets::ITEM_SPELLTRIGGER_ON_USE) {
            return static_cast<int>(spellIDs[i]);
        }
    }
    return 0;
}

int OnUseSpellIDForItemID(uint32_t itemID) {
    if (itemID == 0)
        return 0;
    const uint8_t *record = Item::PeekRecord(itemID);
    if (record == nullptr)
        return 0;
    return FindOnUseSpellIDInRecord(record);
}

static void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_Item", "GetItemSpell", &Script_GetItemSpell);
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace Item::Spell
