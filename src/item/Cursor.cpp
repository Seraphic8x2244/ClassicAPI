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

#include "item/Cursor.h"

#include "Game.h"
#include "Offsets.h"
#include "cursor/Info.h"
#include "item/CGItem.h"
#include "item/Location.h"

#include <cstdint>

namespace Item::Cursor {

namespace {

using PickupInventorySlot_t = void(__fastcall *)(uint32_t slot0Based);
using CursorPickupItem_t = void(__fastcall *)(uint32_t linearSlot, int count,
                                              uint32_t itemGuidLo, uint32_t itemGuidHi,
                                              uint32_t containerGuidLo,
                                              uint32_t containerGuidHi, int flag);
using LockByGuid_t = void(__stdcall *)(uint32_t guidLo, uint32_t guidHi);

// The container object stores its own GUID inline at +0x08/+0x0C (unlike a
// CGItem, whose GUID sits behind the +0x08 instance-block pointer). This
// is the "source container" the engine records for the eventual drop.
constexpr int OFF_CONTAINER_GUID_LO = 0x08;
constexpr int OFF_CONTAINER_GUID_HI = 0x0C;

bool ItemLocked(const uint8_t *item) {
    return (*reinterpret_cast<const uint32_t *>(item + Offsets::OFF_ITEM_CLIENT_LOCK) & 1) != 0;
}

// Reads a CGItem's 64-bit instance GUID into lo/hi. False if the instance
// block pointer is absent.
bool ReadItemGuid(const uint8_t *item, uint32_t *lo, uint32_t *hi) {
    auto *instance = Item::InstanceBlock(item);
    if (instance == nullptr)
        return false;
    *lo = *reinterpret_cast<const uint32_t *>(instance + Offsets::OFF_INSTANCE_BLOCK_GUID);
    *hi = *reinterpret_cast<const uint32_t *>(instance + Offsets::OFF_INSTANCE_BLOCK_GUID + 4);
    return true;
}

} // namespace

void PickupEquipmentSlot(int equipmentSlotIndex) {
    // The primitive takes a 0-based slot; modern equipmentSlotIndex is 1-based.
    reinterpret_cast<PickupInventorySlot_t>(Offsets::FUN_PICKUP_INVENTORY_SLOT)(
        static_cast<uint32_t>(equipmentSlotIndex - 1));
}

bool PickupBagItem(void *L, int bagID, int slotIndex) {
    Item::Location::BagSlotDetail d;
    if (!Item::Location::ResolveBagSlotDetail(L, bagID, slotIndex, &d) || d.item == nullptr)
        return false;

    // Guards the Script_ wrapper applies that the raw primitive omits: only
    // pick up onto an empty cursor, and never a locked (mid-transaction) item.
    // `::Cursor` — unqualified `Cursor` would bind to `Item::Cursor` here.
    if (::Cursor::Info::HasItem() || ItemLocked(d.item))
        return false;

    uint32_t itemLo = 0, itemHi = 0;
    if (!ReadItemGuid(d.item, &itemLo, &itemHi))
        return false;
    const uint32_t containerLo =
        *reinterpret_cast<const uint32_t *>(d.container + OFF_CONTAINER_GUID_LO);
    const uint32_t containerHi =
        *reinterpret_cast<const uint32_t *>(d.container + OFF_CONTAINER_GUID_HI);

    reinterpret_cast<CursorPickupItem_t>(Offsets::FUN_CURSOR_PICKUP_ITEM)(
        static_cast<uint32_t>(d.linearSlot), 1, itemLo, itemHi, containerLo, containerHi, 0);
    // Visually lock the source item while it's held — the Script pickup
    // does this immediately after the cursor primitive.
    reinterpret_cast<LockByGuid_t>(Offsets::FUN_ITEM_LOCK_BY_GUID)(itemLo, itemHi);
    return true;
}

} // namespace Item::Cursor
