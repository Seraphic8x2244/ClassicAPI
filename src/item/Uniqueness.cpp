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

// `C_Item.GetItemUniqueness(itemLocation)` and the `*ByID` variant
// — uniqueness cap. Both read `ItemStats_C.m_maxCount` from the
// item's cached record; that's the only uniqueness signal vanilla
// carries. `1` = unique-equipped (one at a time); higher values
// cap the inventory count; `0` = unlimited.
//
// Modern WoW (Classic Era 1.15.x) gives the two functions **different
// signatures**, verified against `_classic_` BlizzardInterfaceCode's
// `ItemDocumentation.lua`:
//
//   GetItemUniqueness(itemLocation) →
//       limitCategory (number, non-nilable),
//       limitMax      (number, non-nilable)
//
//   GetItemUniquenessByID(itemInfo) →
//       isUnique           (bool, non-nilable),
//       limitCategoryName  (string, nilable),
//       limitCategoryCount (number, nilable),
//       limitCategoryID    (number, nilable)
//
// Vanilla has no `LimitCategory.dbc` system (TBC+ addition for
// "Unique-Equipped: Brewfest Mug"-style cross-item category caps),
// so the category fields are always nil/0. `isUnique` collapses to
// `m_maxCount > 0`; `limitMax` and `limitCategoryCount` both surface
// the raw `m_maxCount`.

#include "Game.h"
#include "Offsets.h"
#include "item/Data.h"
#include "item/ID.h"
#include "item/Location.h"
#include "item/Record.h"

#include <cstdint>

namespace Item::Uniqueness {

namespace {

// Returns the item's `m_maxCount` or -1 if the record isn't cached.
// `-1` is a "warm-and-bail" sentinel; caller pushes nothing on -1
// so the Lua side sees the modern "MayReturnNothing" behaviour.
int ReadMaxCount(uint32_t itemID) {
    const uint8_t *record = Item::PeekRecord(itemID);
    if (record == nullptr) {
        Item::Data::WarmCache(itemID);
        return -1;
    }
    return *reinterpret_cast<const int *>(
        record + Offsets::OFF_ITEMSTATS_MAX_COUNT);
}

} // namespace

// `GetItemUniqueness(itemLocation)` shape: `(limitCategory, limitMax)`.
static int __fastcall Script_GetItemUniqueness(void *L) {
    if (!Item::Location::IsLocationArg(L, 1))
        return 0;
    auto *item = Item::Location::Resolve(L, 1);
    if (item == nullptr)
        return 0;
    const int itemID = Item::ID::FromCGItem(item);
    if (itemID <= 0)
        return 0;
    const int maxCount = ReadMaxCount(static_cast<uint32_t>(itemID));
    if (maxCount < 0)
        return 0; // cache miss — modern API documents "MayReturnNothing"
    Game::Lua::PushNumber(L, 0.0); // limitCategory (vanilla has no LimitCategory.dbc)
    Game::Lua::PushNumber(L, static_cast<double>(maxCount));
    return 2;
}

// `GetItemUniquenessByID(itemID)` shape:
// `(isUnique, limitCategoryName, limitCategoryCount, limitCategoryID)`.
static int __fastcall Script_GetItemUniquenessByID(void *L) {
    if (!Game::Lua::IsNumber(L, 1))
        return 0;
    const int itemID = static_cast<int>(Game::Lua::ToNumber(L, 1));
    if (itemID <= 0)
        return 0;
    const int maxCount = ReadMaxCount(static_cast<uint32_t>(itemID));
    if (maxCount < 0)
        return 0;
    Game::Lua::PushBool(L, maxCount > 0);
    Game::Lua::PushNil(L);          // limitCategoryName (no categories in vanilla)
    if (maxCount > 0)
        Game::Lua::PushNumber(L, static_cast<double>(maxCount));
    else
        Game::Lua::PushNil(L);      // limitCategoryCount — nil when not limited
    Game::Lua::PushNil(L);          // limitCategoryID
    return 4;
}

static void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_Item", "GetItemUniqueness",
                                     &Script_GetItemUniqueness);
    Game::Lua::RegisterTableFunction("C_Item", "GetItemUniquenessByID",
                                     &Script_GetItemUniquenessByID);
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace Item::Uniqueness
