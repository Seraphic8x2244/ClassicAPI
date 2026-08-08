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

#include "item/Swap.h"

#include "Game.h"
#include "Offsets.h"
#include "item/CGItem.h"
#include "item/Location.h"
#include "unit/Identity.h"

#include <cstdint>

namespace Item::Swap {

namespace {

using SwapFn_t = void(__thiscall *)(
    void *player,
    uint32_t srcItemLo, uint32_t srcItemHi,
    uint32_t srcContainerLo, uint32_t srcContainerHi,
    uint32_t srcLinearSlot,
    uint32_t dstContainerLo, uint32_t dstContainerHi,
    uint32_t dstLinearSlot,
    int flag);

// Reads the 64-bit GUID stored at instance-block offset 0 of any
// CGObject-derived pointer (CGItem, CGPlayer, CGContainer all use
// the same +0x8 → instance block → +0x00 GUID layout).
bool ReadGuid(const void *cgObject, uint32_t *lo, uint32_t *hi) {
    if (cgObject == nullptr)
        return false;
    auto *base = static_cast<const uint8_t *>(cgObject);
    auto *inst = Item::InstanceBlock(base);
    if (inst == nullptr)
        return false;
    *lo = *reinterpret_cast<const uint32_t *>(inst + Offsets::OFF_INSTANCE_BLOCK_GUID);
    *hi = *reinterpret_cast<const uint32_t *>(inst + Offsets::OFF_INSTANCE_BLOCK_GUID + 4);
    return true;
}

// Bag-container linear-slot range — the four equipped-bag slots
// the engine treats as "directly in the player's invMgr" for the
// purposes of opcode 0x10D's same-container check. Just here so
// the magic +19 in FromBag is explained at the call site.
constexpr int FIRST_BAG_CONTAINER_LINEAR = 19;

// Backpack contents (bag 0) live at linear slots 23..38 in the
// player invMgr — i.e. 1-based bag-0 slot S maps to linear 22+S.
constexpr int BACKPACK_LINEAR_OFFSET = 22;

// Main bank contents (bagID -1) live at linear slots 39..62 in the
// player invMgr — i.e. 1-based bank slot S maps to linear 38+S.
// Same encoding shape as the backpack (container is the player
// itself), different base offset. Range valid only when the bank
// window has been opened in the current session.
constexpr int BANK_MAIN_LINEAR_OFFSET = 38;
constexpr int BANK_MAIN_NUM_SLOTS = 24;

// Bank bag containers occupy 1-based equipment slots 64..69 (linear
// 63..68). Addressed by bagID 5..10 — bagID 5 = first bank bag.
// Requires bank window open; otherwise the engine's CGContainer for
// the bag is unsynced and `ResolveEquipmentSlot` returns null.
constexpr int FIRST_BANK_BAG_EQ_SLOT = 64;
constexpr int FIRST_BANK_BAG_ID = 5;
constexpr int LAST_BANK_BAG_ID = 10;

// (bagID, 1-based slot) → (containerGUID, engine linear slot). Returns
// false for unequipped bags or out-of-range slots; does NOT validate
// that the slot is occupied (caller checks separately).
//
//   bagID  0    → container = player, linear = 22 + slot (backpack)
//   bagID  1..4 → container = equipped bag's CGContainer, linear = slot - 1
//   bagID -1    → container = player, linear = 38 + slot (bank main)
//   bagID  5..10 → container = bank bag's CGContainer, linear = slot - 1
//
// Bank-side encodings only resolve once the bank window has been
// opened in the current session (the engine doesn't sync the bank
// CGContainers or bank-slot inventory entries before then).
bool EncodeBagSlot(int bagID, int slotInBag,
                   uint32_t *containerLo, uint32_t *containerHi,
                   uint32_t *linearSlot) {
    if (slotInBag < 1)
        return false;
    if (bagID == 0) {
        void *player = const_cast<uint8_t *>(Unit::Identity::PlayerObject());
        if (player == nullptr)
            return false;
        if (!ReadGuid(player, containerLo, containerHi))
            return false;
        *linearSlot = static_cast<uint32_t>(BACKPACK_LINEAR_OFFSET + slotInBag);
        return true;
    }
    if (bagID >= 1 && bagID <= 4) {
        const uint8_t *bag = Item::Location::ResolveEquipmentSlot(
            FIRST_BAG_CONTAINER_LINEAR + bagID);
        if (bag == nullptr)
            return false;
        if (!ReadGuid(bag, containerLo, containerHi))
            return false;
        *linearSlot = static_cast<uint32_t>(slotInBag - 1);
        return true;
    }
    if (bagID == -1) {
        if (slotInBag > BANK_MAIN_NUM_SLOTS)
            return false;
        void *player = const_cast<uint8_t *>(Unit::Identity::PlayerObject());
        if (player == nullptr)
            return false;
        if (!ReadGuid(player, containerLo, containerHi))
            return false;
        *linearSlot = static_cast<uint32_t>(BANK_MAIN_LINEAR_OFFSET + slotInBag);
        return true;
    }
    if (bagID >= FIRST_BANK_BAG_ID && bagID <= LAST_BANK_BAG_ID) {
        const uint8_t *bag = Item::Location::ResolveEquipmentSlot(
            FIRST_BANK_BAG_EQ_SLOT + (bagID - FIRST_BANK_BAG_ID));
        if (bag == nullptr)
            return false;
        if (!ReadGuid(bag, containerLo, containerHi))
            return false;
        *linearSlot = static_cast<uint32_t>(slotInBag - 1);
        return true;
    }
    return false;
}

// Common send path. All paperdoll-side state (player ptr, player
// GUID) is resolved here; caller supplies the source side
// (already-encoded linear slot + the container GUID it lives in).
bool SendSwap(const void *srcItem,
              uint32_t srcContainerLo, uint32_t srcContainerHi,
              uint32_t srcLinearSlot,
              int dstPaperdollSlot) {
    if (dstPaperdollSlot < 1 || dstPaperdollSlot > 19)
        return false;
    if (srcItem == nullptr)
        return false;

    void *player = const_cast<uint8_t *>(Unit::Identity::PlayerObject());
    if (player == nullptr)
        return false;

    uint32_t playerLo = 0, playerHi = 0;
    if (!ReadGuid(player, &playerLo, &playerHi))
        return false;

    uint32_t itemLo = 0, itemHi = 0;
    if (!ReadGuid(srcItem, &itemLo, &itemHi))
        return false;

    auto fn = reinterpret_cast<SwapFn_t>(Offsets::FUN_INVENTORY_SWAP);
    fn(player,
       itemLo, itemHi,
       srcContainerLo, srcContainerHi, srcLinearSlot,
       playerLo, playerHi,
       static_cast<uint32_t>(dstPaperdollSlot - 1),
       0);
    return true;
}

} // namespace

bool FromBag(const void *cgItem, int bagID, int slotInBag, int dstPaperdollSlot) {
    uint32_t containerLo = 0, containerHi = 0, linearSlot = 0;
    if (!EncodeBagSlot(bagID, slotInBag, &containerLo, &containerHi, &linearSlot))
        return false;
    return SendSwap(cgItem, containerLo, containerHi, linearSlot, dstPaperdollSlot);
}

bool FromPaperdoll(const void *cgItem, int srcPaperdollSlot, int dstPaperdollSlot) {
    if (srcPaperdollSlot < 1 || srcPaperdollSlot > 19)
        return false;

    void *player = const_cast<uint8_t *>(Unit::Identity::PlayerObject());
    if (player == nullptr)
        return false;
    uint32_t playerLo = 0, playerHi = 0;
    if (!ReadGuid(player, &playerLo, &playerHi))
        return false;
    return SendSwap(cgItem,
                    playerLo, playerHi,
                    static_cast<uint32_t>(srcPaperdollSlot - 1),
                    dstPaperdollSlot);
}

bool ToBag(const void *cgItem, int srcPaperdollSlot, int dstBagID, int dstSlotInBag) {
    if (srcPaperdollSlot < 1 || srcPaperdollSlot > 19)
        return false;
    if (cgItem == nullptr)
        return false;

    void *player = const_cast<uint8_t *>(Unit::Identity::PlayerObject());
    if (player == nullptr)
        return false;
    uint32_t playerLo = 0, playerHi = 0;
    if (!ReadGuid(player, &playerLo, &playerHi))
        return false;

    uint32_t dstContainerLo = 0, dstContainerHi = 0, dstLinear = 0;
    if (!EncodeBagSlot(dstBagID, dstSlotInBag,
                       &dstContainerLo, &dstContainerHi, &dstLinear))
        return false;

    uint32_t itemLo = 0, itemHi = 0;
    if (!ReadGuid(cgItem, &itemLo, &itemHi))
        return false;

    auto fn = reinterpret_cast<SwapFn_t>(Offsets::FUN_INVENTORY_SWAP);
    fn(player,
       itemLo, itemHi,
       playerLo, playerHi,
       static_cast<uint32_t>(srcPaperdollSlot - 1),
       dstContainerLo, dstContainerHi, dstLinear,
       0);
    return true;
}

bool MoveCount(void *L, int srcBag, int srcSlot, int dstBag, int dstSlot, int count) {
    // Vanilla protocol writes count as a single byte. Clamp at 255;
    // anything larger would either truncate silently (255 sent) or
    // wrap (0 = no-op). Refuse zero outright too.
    if (count < 1 || count > 255)
        return false;

    uint32_t srcContainerLo = 0, srcContainerHi = 0, srcLinear = 0;
    uint32_t dstContainerLo = 0, dstContainerHi = 0, dstLinear = 0;
    if (!EncodeBagSlot(srcBag, srcSlot,
                       &srcContainerLo, &srcContainerHi, &srcLinear))
        return false;
    if (!EncodeBagSlot(dstBag, dstSlot,
                       &dstContainerLo, &dstContainerHi, &dstLinear))
        return false;

    // Confirm source is occupied — surfacing "empty source" as a clean
    // local `false` rather than relying on the server reject + log
    // SMSG_INVENTORY_CHANGE_FAILURE noise.
    const uint8_t *srcItem = Item::Location::ResolveBag(L, srcBag, srcSlot);
    if (srcItem == nullptr)
        return false;

    // Vanilla server rejects `CMSG_SPLIT_ITEM` when count == srcStack
    // (`EQUIP_ERR_COULDNT_SPLIT_ITEMS`) — conceptually "split N off,
    // leave residual", and splitting the entire stack would leave
    // source empty. The drag-and-drop UI's "drop whole stack on
    // matching stack" goes through CMSG_SWAP_ITEM instead, which the
    // server treats as a merge for same-item destinations (up to
    // maxStack). Route there for count == srcStack so callers don't
    // have to special-case "move everything".
    auto *srcDescriptor = Item::ObjectFields(srcItem);
    const int srcStack = srcDescriptor == nullptr ? 0 :
        static_cast<int>(*reinterpret_cast<const uint32_t *>(
            srcDescriptor + Offsets::OFF_DESCRIPTOR_STACK_COUNT));
    if (count > srcStack)
        return false; // server would reject anyway; fail fast and locally
    if (count == srcStack)
        return Containers(L, srcBag, srcSlot, dstBag, dstSlot);

    void *player = const_cast<uint8_t *>(Unit::Identity::PlayerObject());
    if (player == nullptr)
        return false;

    using SplitFn_t = void(__thiscall *)(
        void *thisPlayer,
        uint32_t unused1, uint32_t unused2,
        uint32_t srcContainerLo, uint32_t srcContainerHi, uint32_t srcSlot,
        uint32_t dstContainerLo, uint32_t dstContainerHi, uint32_t dstSlot,
        uint32_t count);
    auto fn = reinterpret_cast<SplitFn_t>(Offsets::FUN_INVENTORY_SPLIT);
    fn(player,
       0, 0,
       srcContainerLo, srcContainerHi, srcLinear,
       dstContainerLo, dstContainerHi, dstLinear,
       static_cast<uint32_t>(count));
    return true;
}

bool Containers(void *L, int srcBag, int srcSlot, int dstBag, int dstSlot) {
    uint32_t srcContainerLo = 0, srcContainerHi = 0, srcLinear = 0;
    uint32_t dstContainerLo = 0, dstContainerHi = 0, dstLinear = 0;
    if (!EncodeBagSlot(srcBag, srcSlot,
                       &srcContainerLo, &srcContainerHi, &srcLinear))
        return false;
    if (!EncodeBagSlot(dstBag, dstSlot,
                       &dstContainerLo, &dstContainerHi, &dstLinear))
        return false;

    // Look up the source CGItem so we can put its GUID on the wire.
    // This stomps the Lua stack — caller must have already read every
    // arg it cares about.
    const uint8_t *srcItem = Item::Location::ResolveBag(L, srcBag, srcSlot);
    if (srcItem == nullptr)
        return false; // empty source slot

    void *player = const_cast<uint8_t *>(Unit::Identity::PlayerObject());
    if (player == nullptr)
        return false;

    uint32_t itemLo = 0, itemHi = 0;
    if (!ReadGuid(srcItem, &itemLo, &itemHi))
        return false;

    auto fn = reinterpret_cast<SwapFn_t>(Offsets::FUN_INVENTORY_SWAP);
    fn(player,
       itemLo, itemHi,
       srcContainerLo, srcContainerHi, srcLinear,
       dstContainerLo, dstContainerHi, dstLinear,
       0);
    return true;
}

} // namespace Item::Swap
