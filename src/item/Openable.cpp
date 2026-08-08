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

// `C_Item.IsItemOpenable(itemLocation)` — backport of the modern WoW
// accessor that says "right-clicking this item triggers an open/loot
// interaction" (clams, sacks, simple chests, quest boxes, lockboxes).
// Returns two values: `(isOpenable, canOpen)` — the first is the
// item-type-intrinsic openable flag, the second is whether the
// player can right-click *this specific instance* right now (lock
// is gone or never existed).
//
// Vanilla 1.12's tooltip builder gates the `<Right Click to Open>`
// string (`ITEM_OPENABLE`) on `ItemStats.Flags & 0x4` AND
// (`LockID == 0` OR `ITEM_FIELD_FLAGS & 0x4`). We mirror both checks
// — see `Offsets::ITEMSTATS_FLAG_OPENABLE`, `ITEMSTATS_LOCK_ID`, and
// `ITEM_FLAG_UNLOCKED`.

#include "Game.h"
#include "Offsets.h"
#include "item/CGItem.h"
#include "item/Data.h"
#include "item/ID.h"
#include "item/Location.h"
#include "item/Openable.h"
#include "item/Record.h"

#include <cstdint>

namespace Item::Openable {

namespace {

// Reads `ITEM_FIELD_FLAGS` from the instance descriptor at +0x114.
// Defaults to 0 if `cgItem` is null or the descriptor pointer hasn't
// been populated yet.
uint32_t InstanceFlags(const uint8_t *cgItem) {
    if (cgItem == nullptr)
        return 0;
    auto *descriptor = Item::ObjectFields(cgItem);
    if (descriptor == nullptr)
        return 0;
    return *reinterpret_cast<const uint32_t *>(
        descriptor + Offsets::OFF_DESCRIPTOR_FLAGS);
}

} // namespace

int PushIsItemOpenable(void *L, const uint8_t *cgItem) {
    const int itemID = Item::ID::FromCGItem(cgItem);
    if (itemID <= 0)
        return 0;
    const uint8_t *record = Item::PeekRecord(static_cast<uint32_t>(itemID));
    if (record == nullptr) {
        Item::Data::WarmCache(static_cast<uint32_t>(itemID));
        return 0;
    }
    const uint32_t typeFlags = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_FLAGS);
    const uint32_t lockID = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_LOCK_ID);
    const bool isOpenable = (typeFlags & Offsets::ITEMSTATS_FLAG_OPENABLE) != 0;
    const uint32_t instFlags = InstanceFlags(cgItem);
    const bool canOpen = isOpenable && (lockID == 0 ||
                                        (instFlags & Offsets::ITEM_FLAG_UNLOCKED) != 0);
    Game::Lua::PushBool(L, isOpenable);
    Game::Lua::PushBool(L, canOpen);
    return 2;
}

namespace {

int __fastcall Script_C_Item_IsItemOpenable(void *L) {
    if (!Item::Location::IsLocationArg(L, 1)) {
        Game::Lua::Error(L, "Usage: C_Item.IsItemOpenable(itemLocation)");
        return 0;
    }
    return PushIsItemOpenable(L, Item::Location::Resolve(L, 1));
}

void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_Item", "IsItemOpenable",
                                     &Script_C_Item_IsItemOpenable);
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace

} // namespace Item::Openable
