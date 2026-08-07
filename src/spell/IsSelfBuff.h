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

namespace Spell::IsSelfBuff {

// True iff every active effect of `spellID` targets only the caster — each
// effect's implicit target A and B is `TARGET_NONE` (0) or `TARGET_SELF` (1),
// read from `Spell.dbc.EffectImplicitTargetA/B[3]`. Such an aura can only have
// been self-cast, so a unit carrying one is definitionally its own source.
// False for an unknown spell or one with no active effect. Shared by the
// `C_Spell.IsSelfBuff` Lua binding and the `C_UnitAuras` sourceGUID inference.
bool IsSelfBuff(uint32_t spellID);

} // namespace Spell::IsSelfBuff
