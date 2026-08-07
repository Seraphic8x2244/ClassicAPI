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

// Crowd-control classification of a Spell.dbc record, shared by
// `C_LossOfControl` (which reports the control-loss type) and the
// `C_UnitAuras` `CROWD_CONTROL` aura filter (which only needs presence). One
// source of truth for "is this a hard control effect" so the two never drift.

namespace Spell::CrowdControl {

// Classifies a spell by its `EffectApplyAuraName` set into a control-loss
// type string, priority-ordered (strongest control first) so a multi-effect
// spell resolves to one type:
//   POSSESS, CHARM, STUN, FEAR, CONFUSE, PACIFYSILENCE, SILENCE, PACIFY,
//   ROOT, DISARM
// Returns nullptr if the spell applies no control-loss aura (`spellRecord`
// may be null → nullptr). Movement-only slows/snares are intentionally NOT
// classified — they are not a loss of control.
const char *Classify(const uint8_t *spellRecord);

// True iff `spellID` is a crowd-control aura. Broader than `Classify`: it
// also counts movement slows / snares (`MOD_DECREASE_SPEED`), which are crowd
// control but not loss of control. Backs the C_UnitAuras CROWD_CONTROL filter.
bool IsCrowdControl(uint32_t spellID);

} // namespace Spell::CrowdControl
