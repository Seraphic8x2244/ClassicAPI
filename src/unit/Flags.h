// This file is part of ClassicAPI.
//
// ClassicAPI is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// ClassicAPI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.

#pragma once

#include <cstdint>

namespace Unit::Flags {

// Tests `UNIT_FIELD_FLAGS & UNIT_FLAG_PLAYER_CONTROLLED` on the unit's
// `m_objectFields` descriptor — same predicate
// `Script_UnitPlayerControlled` (`0x00516410`) implements. True for
// players AND their pets/totems/guardians/companions/charms (the
// server sets this flag on all of them — see Turtle
// `UnitDefines.h` UNIT_FLAG_PLAYER_CONTROLLED).
//
// NOT a safe gate for reading the CGPlayer sub-struct at
// `unit + OFF_CGPLAYER_INFO` (= +0xE68): a player-controlled *creature*
// (pet/totem/MC'd mob) is a CGCreature_C with no such sub-struct, so
// its +0xE68 slot is garbage-but-non-null and the subsequent deref AVs
// (pfUI issue #34 — `UnitIsInMyGuild` on a nameplate pet). Use
// `IsPlayerObject` for that. This predicate is for minion/ownership
// classification, where the broad flag is what's wanted.
//
// Returns `false` for `nullptr` units and units with no
// descriptor pointer yet (transient pre-spawn / out-of-sync state).
bool IsPlayerControlled(const uint8_t *unit);

// True only for real player objects (`OBJECT_TYPE_PLAYER`), read from
// the base CGObject `m_objectType` field at `[obj + 0x14]`
// (`OFF_CGOBJECT_TYPE_ID`). This is the correct gate before reading the
// CGPlayer sub-struct at `unit + OFF_CGPLAYER_INFO` (guild key,
// PLAYER_FLAGS, visible items, quest list) — that sub-struct exists only
// on player objects, never on player-controlled creatures.
//
// Returns `false` for `nullptr` units.
bool IsPlayerObject(const uint8_t *unit);

} // namespace Unit::Flags
