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

#pragma once

#include <cstdint>

// Passive read of the client-side item cache (the ItemSparse `ItemStats_C`
// records). Every read-only item accessor used to re-declare the same
// `DBCache_ItemStats_C_GetRecord` typedef and call it with a null callback;
// this names that one passive peek.

namespace Item {

// The cached `ItemStats_C` record for `itemID`, or nullptr if it isn't cached.
// PASSIVE — a null callback means "look up only": it never warms the cache or
// fires SMSG_ITEM_QUERY_SINGLE, so it is safe to call many times per frame
// from tooltips / bag scans. (Eagerly triggering a query from a passive getter
// is the cache-race trap documented in CLAUDE.md — the request/response path
// lives in item/Data.cpp, not here.) Fields are read via the `OFF_ITEMSTATS_*`
// offsets.
const uint8_t *PeekRecord(uint32_t itemID);

} // namespace Item
