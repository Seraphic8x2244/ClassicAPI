// This file is part of ClassicAPI.
//
// ClassicAPI is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// ClassicAPI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.

#include "Game.h"
#include "guid/Guid.h"

#include <cstdint>

namespace Guid::CreatureInfoBindings {

namespace {

// `C_CreatureInfo.GetCreatureID(guid) → creatureID | nil`
//
// Extracts the creature template / NPC ID from a unit GUID. Vanilla
// 1.12 packs the entry ID into bits 24-47 of the GUID for the world-
// object types that carry one (creature `0xF130`, pet `0xF140`); the
// low 24 bits are the per-spawn counter. Player GUIDs use the low 32
// bits for the player ID and have no template, so they return nil.
//
// Modern's `C_CreatureInfo.GetCreatureID` returns nil for any GUID
// that isn't a creature; we additionally accept pet GUIDs since
// vanilla pets carry an entry ID too (the pet's creature template,
// the same field that drives the pet bar icon). Game-object GUIDs
// have entry IDs in the same bit range but modern doesn't surface
// them through `C_CreatureInfo` — addons that need those should
// look at the GUID prefix and shift manually.
//
// Returns nil for: non-string input, malformed GUIDs, non-creature /
// non-pet types, and entry IDs of 0 (the engine never assigns
// entry 0 to anything; treat as "no info").
int __fastcall Script_GetCreatureID(void *L) {
    if (!Game::Lua::IsString(L, 1)) {
        Game::Lua::PushNil(L);
        return 1;
    }
    uint64_t guid = 0;
    if (!Parse(Game::Lua::ToString(L, 1), &guid)) {
        Game::Lua::PushNil(L);
        return 1;
    }
    // `Guid::CreatureEntry` handles the creature/pet type gate + the bits-24-47
    // extraction, and returns 0 for players, non-creature types, and entry 0.
    const uint32_t entryID = CreatureEntry(guid);
    if (entryID == 0) {
        Game::Lua::PushNil(L);
        return 1;
    }
    Game::Lua::PushNumber(L, static_cast<double>(entryID));
    return 1;
}

void Register() {
    Game::Lua::RegisterTableFunction("C_CreatureInfo", "GetCreatureID",
                                     &Script_GetCreatureID);
}

} // namespace

static const Game::ModuleAutoRegister _autoreg{&Register};

} // namespace Guid::CreatureInfoBindings
