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

#include "CGItem.h"

#include "Offsets.h"

namespace Item {

const uint8_t *InstanceBlock(const uint8_t *cgItem) {
    if (cgItem == nullptr)
        return nullptr;
    return *reinterpret_cast<const uint8_t *const *>(
        cgItem + Offsets::OFF_ITEM_INSTANCE_BLOCK);
}

const uint8_t *ObjectFields(const uint8_t *cgItem) {
    if (cgItem == nullptr)
        return nullptr;
    return *reinterpret_cast<const uint8_t *const *>(
        cgItem + Offsets::OFF_ITEM_DESCRIPTOR);
}

} // namespace Item
