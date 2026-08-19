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

#include "item/TooltipItem.h"

#include "Game.h"
#include "Offsets.h"
#include "item/ID.h"
#include "item/Location.h"

namespace Item::TooltipItem {

int CurrentID(const void *tooltipObj, uint64_t *outGuid) {
    if (outGuid != nullptr)
        *outGuid = 0;
    if (tooltipObj == nullptr)
        return 0;
    auto *c = static_cast<const uint8_t *>(tooltipObj);

    const uint64_t itemGuid =
        Game::Read<uint64_t>(c, Offsets::OFF_TOOLTIP_ITEM_GUID_LO);

    // Link item: the itemID field is authoritative (and reliably cleared).
    const int linkItemID = Game::Read<int>(c, Offsets::OFF_TOOLTIP_ITEM_ID);
    if (linkItemID > 0) {
        if (outGuid != nullptr)
            *outGuid = itemGuid;
        return linkItemID;
    }

    // CGItem item: only the GUID is stored, and +0x380 isn't zeroed by the
    // clear — so it lingers after the tooltip switches to a unit / GO /
    // spell. Each of those sets its own (cleared-then-written) id, so trust
    // the item GUID only when none of them is currently shown, then resolve
    // it to the itemID (this also yields the dressed link for callers).
    if (itemGuid == 0)
        return 0;
    if (Game::Read<uint64_t>(c, Offsets::OFF_TOOLTIP_UNIT_GUID_LO) != 0)
        return 0;
    if (Game::Read<uint64_t>(c, Offsets::OFF_TOOLTIP_GAMEOBJECT_GUID_LO) != 0)
        return 0;
    if (Game::Read<int>(c, Offsets::OFF_TOOLTIP_SPELL_ID) > 0)
        return 0;

    const uint8_t *cg = Item::Location::ResolveByGUID(itemGuid);
    if (cg == nullptr)
        return 0;
    const int id = Item::ID::FromCGItem(cg);
    if (id <= 0)
        return 0;
    if (outGuid != nullptr)
        *outGuid = itemGuid;
    return id;
}

} // namespace Item::TooltipItem
