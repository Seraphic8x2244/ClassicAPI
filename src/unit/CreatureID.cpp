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

// `UnitCreatureID(unit)` — the creature-template / NPC id for a unit token,
// or `nil`. The token version of `C_CreatureInfo.GetCreatureID(guid)`: it
// resolves the token to a GUID and reads the same entry field the GUID packs
// into bits 24-47. Equivalent to `C_CreatureInfo.GetCreatureID(UnitGUID(unit))`
// but without the round-trip.
//
// `nil` for: players (their GUID carries no template), an unresolvable-but-
// valid token (`target` with nothing targeted, an empty `partyN` slot), and
// any unit whose GUID isn't a creature or pet. Raises a Lua error on a
// missing / non-string `unit` argument or a garbage token — the standard
// `UnitX` "Unknown unit" behavior (via the token resolver).

#include "Game.h"
#include "guid/Guid.h"
#include "unit/Identity.h"

#include <cstdint>

namespace Unit::CreatureID {

namespace {

int __fastcall Script_UnitCreatureID(void *L) {
    if (!Game::Lua::IsString(L, 1)) {
        Game::Lua::Error(L, "Usage: UnitCreatureID(\"unit\")");
        return 0;
    }
    const char *token = Game::Lua::ToString(L, 1);
    if (token == nullptr)
        return 0;

    // Token -> GUID (player fast-path, focus / nameplateN / markN, raw GUID),
    // then the shared creature-entry extraction. A valid-but-empty token
    // resolves to GUID 0 -> entry 0 -> nil; a player resolves to a player GUID
    // -> entry 0 -> nil.
    const uint32_t entryID =
        Guid::CreatureEntry(Unit::Identity::GuidForToken(token));
    if (entryID == 0) {
        Game::Lua::PushNil(L);
        return 1;
    }
    Game::Lua::PushNumber(L, static_cast<double>(entryID));
    return 1;
}

void RegisterLuaFunctions() {
    Game::Lua::RegisterGlobalFunction("UnitCreatureID", &Script_UnitCreatureID);
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace

} // namespace Unit::CreatureID
