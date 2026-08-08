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

// `DBC::*` display-name resolvers for the character/zone DBCs. Thin wrappers
// over `DBC::Record` / `LocalizedField` / `StringField` (see `Lookup.h`) that
// bake in the specific table + field for a game concept, so callers don't
// repeat the `(recordsVar, countVar, id, offset)` tuple. Shared by the
// friends-list and /who surfaces (and any future class/race/zone display).
// All return `nullptr` when the id is 0 / out of range / the field is empty.

namespace DBC {

// Localized class name for a `ChrClasses.dbc` id (e.g. "Mage").
const char *ClassName(uint32_t classID);

// Locale-independent class token for a `ChrClasses.dbc` id (e.g. "MAGE") —
// the key addons use for RAID_CLASS_COLORS and other class tables.
const char *ClassToken(uint32_t classID);

// Localized race name for a `ChrRaces.dbc` id (e.g. "Night Elf").
const char *RaceName(uint32_t raceID);

// Locale-independent race token for a `ChrRaces.dbc` id — the client-filename
// column (e.g. "NightElf", "Scourge"), the race analog of ClassToken and what
// `UnitRace`'s second return / GetPlayerInfoByGUID's englishRace use.
const char *RaceToken(uint32_t raceID);

// Reverse of ClassToken / RaceToken: the `ChrClasses.dbc` / `ChrRaces.dbc` id
// whose client-filename token matches `token` (case-insensitive), or 0 if none
// matches. Walks the table (cheap — a handful of records).
uint32_t ClassIdForToken(const char *token);
uint32_t RaceIdForToken(const char *token);

// Localized zone name for an `AreaTable.dbc` id. When `resolveToParent` is
// true, a sub-area reports its parent zone (so "Goldshire" → "Elwynn
// Forest"); when false, the area's own name is returned (what the engine's
// GetWhoInfo does — /who zones are already zone-level).
const char *AreaName(uint32_t areaID, bool resolveToParent);

} // namespace DBC
