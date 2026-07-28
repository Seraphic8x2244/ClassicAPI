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

// `C_Spell.CastAtUnit(spellIDOrName, unit)` — cast a spell at the given
// unit, whatever its target type. One primitive covering both:
//
//   - **Ground-target spells** (Blizzard, Flamestrike, Rain of Fire…) are
//     placed at the unit's feet — the reticle is committed at the unit's
//     world position, no manual click.
//   - **Unit-target / normal spells** (Frostbolt, Healing Touch, a buff…)
//     fire directly on the unit — its GUID is fed to the engine's cast
//     dispatcher as the implicit target, so no current-target juggling and
//     no "select target" reticle.
//
// Mechanism: resolve the token once to the unit object, read its position
// and GUID, then dispatch the cast WITH that GUID
// (`Spell::AtCursor::DispatchSpellCast(spell, guid)`). The engine's
// dispatcher substitutes the GUID exactly where it would the current
// selection, so a unit-target spell fires immediately at that unit. A
// ground spell ignores the GUID and enters placement mode instead — so we
// branch on `IsPlacementActive()`:
//
//   - No placement after dispatch → the cast already went out on the unit
//     (unit-target / normal). Success.
//   - Ground placement active     → commit at the unit's feet. Success.
//   - Other placement still active → the engine didn't accept the unit as a
//     target (e.g. wrong faction / out of range); `CommitAtCoords` cancels
//     it. Failure.
//
// Returns `true` when the spell was cast at the unit; `false` when the
// spell isn't in the spellbook, the unit has no resolvable position, or the
// unit wasn't a valid target. A genuinely unrecognized token string raises
// the engine's standard "Unknown unit" error inside the resolver, same as
// `UnitHealth("garbage")`.

#include "Game.h"
#include "spell/AtCursor.h"
#include "spell/MacroPrimarySpell.h"
#include "unit/Identity.h"
#include "unit/Position.h"

#include <cstdint>

namespace Spell::AtUnit {

namespace {

int __fastcall Script_C_Spell_CastAtUnit(void *L) {
    // arg1: numeric spellID (exact rank) or spell name ("(Rank N)"-aware).
    // arg2: unit token.
    const bool byNumber = Game::Lua::IsNumber(L, 1);
    if ((!byNumber && !Game::Lua::IsString(L, 1)) ||
        !Game::Lua::IsString(L, 2)) {
        Game::Lua::PushBool(L, false);
        return 1;
    }
    const char *token = Game::Lua::ToString(L, 2);

    // Resolve the unit once → object, then read position + GUID from it.
    // Fail fast so a bad/absent unit doesn't leave the engine mid-cast.
    void *unitObj = Unit::Position::ResolveToken(token);
    float pos[3];
    if (unitObj == nullptr || !Unit::Position::Read(unitObj, pos)) {
        Game::Lua::PushBool(L, false);
        return 1;
    }
    const uint64_t targetGuid = Unit::Identity::GuidForObject(unitObj);

    const bool dispatched =
        byNumber
            ? Spell::AtCursor::DispatchSpellCast(
                  static_cast<int>(Game::Lua::ToNumber(L, 1)), targetGuid)
            : Spell::AtCursor::DispatchSpellCastByName(Game::Lua::ToString(L, 1),
                                                       targetGuid);
    if (!dispatched) {
        Game::Lua::PushBool(L, false);
        return 1;
    }

    // Unit-target / normal spells fired straight at the GUID — no placement.
    // Ground spells are now sitting in placement mode; commit at the unit's
    // feet (CommitAtCoords cancels + fails for a non-ground placement, i.e.
    // an unaccepted unit target).
    if (!Spell::AtCursor::IsPlacementActive()) {
        Game::Lua::PushBool(L, true);
        return 1;
    }
    const bool placed = Spell::AtCursor::CommitAtCoords(pos);
    Game::Lua::PushBool(L, placed);
    return 1;
}

void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_Spell", "CastAtUnit",
                                     &Script_C_Spell_CastAtUnit);
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

// Macro-parser pattern: tag macros calling
// `C_Spell.CastAtUnit(<spellID>, ...)` with the spellID so action-bar
// UIs highlight the slot. The first arg is the numeric spellID, so the
// extractor just reads the leading numeric literal — same shape as
// `CastAtCursor`'s.
int ExtractCastAtUnitArg(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t'))
        ++p;
    int value = 0;
    const char *digitStart = p;
    while (p < end && *p >= '0' && *p <= '9') {
        if (value > 100000000) // sanity cap before overflow
            return 0;
        value = value * 10 + (*p - '0');
        ++p;
    }
    return (p > digitStart && value > 0) ? value : 0;
}

const Spell::MacroPrimarySpell::PatternAutoRegister _patreg{
    "C_Spell.CastAtUnit(", &ExtractCastAtUnitArg};

} // namespace

} // namespace Spell::AtUnit
