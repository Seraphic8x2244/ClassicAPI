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

namespace Spell::Tooltip {

// Resolves the `GameTooltip` self at Lua stack[1] (via the
// engine's `FrameScript_PushObject`/`FrameScript_GetObject` chain)
// and dispatches the engine's spell-tooltip builder for `spellID`,
// with the spell's rank shown in the top-right corner of the title
// line (matching `SetAction`/`SetPetAction`; native `SetSpell` omits it).
// Silent no-op if `spellID <= 0` or stack[1] isn't a frame object.
//
// Used by `GameTooltip:SetSpellByID` directly, and by
// `GameTooltip:SetTalentByID` as a fallback for cross-class talents
// (where the engine's per-player TalentEntry tables don't apply,
// so we render the rank-1 spell tooltip instead).
void ShowByID(void *L, int spellID);

// Like `ShowByID` but APPENDS the spell's tooltip to the current one
// (no clear), and overwrites the engine's talent "next rank" header with
// the real spell name. Backs `GameTooltip:AddSpellByID`. Silent no-op if
// `spellID <= 0`, stack[1] isn't a frame object, or the build added no
// lines. Does not stash the displayed spellID (so `GetSpell` still
// reflects the base tooltip). Useful for cross-class talent tooltips
// (`AddLine(talentName) + AppendByID(rankSpellID)`).
void AppendByID(void *L, int spellID);

} // namespace Spell::Tooltip
