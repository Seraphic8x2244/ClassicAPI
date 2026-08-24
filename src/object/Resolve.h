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

// Shared wrapper over the engine's object-manager GUID resolver
// (`FUN_OBJECT_RESOLVE_BY_GUID` at 0x00468460). Every caller used to
// re-declare the same `__fastcall` function-pointer typedef and split the
// GUID by hand; this centralizes it.

namespace Object {

// True while the engine's client/player-rooted object registry is up. The
// resolver (`FUN_OBJECT_RESOLVE_BY_GUID`) and the descriptor-observer
// register/unregister all funnel through `FUN_00464890`, whose FIRST action
// dereferences the registry root at `[VAR_LOCAL_PLAYER_PTR]` (reads its
// `+0x24` mask) with no null check. That root is NULL out of world — login
// screen, loading screens, logout teardown — where the deref is a fatal
// ERROR #132 (`MOV EAX,[EDX+0x24]`, EDX=0). Anything that can run
// mid-transition (packet hooks, Lua, WorldTick) must gate on this before
// touching the registry. See issue #34 for a live crash (SMSG_SPELL_GO
// dispatched during a world transition).
bool RegistryReady();

// Resolve `guid` to its live client object. `typeMask` is a bitmask of
// `Offsets::TYPEMASK_*` (e.g. `TYPEMASK_UNIT | TYPEMASK_PLAYER`, or
// `TYPEMASK_UNIT` / `TYPEMASK_OBJECT`). Returns nullptr when the GUID isn't
// currently in the object table (out of range, despawned, pre-world).
// Non-throwing — safe from any context (Lua callback, world tick, packet
// hook): internally gated on `RegistryReady`, so an out-of-world call
// resolves to nullptr instead of null-derefing in the engine. `priority`
// mirrors the engine's call-site priority argument: 0x172 for a normal
// resolve, 0 on the loot/enumeration paths. `debugName` is an engine
// diagnostic label (may be nullptr).
//
// Returns `void *` — callers static_cast to the concrete object type they
// expect (the engine hands back the same pointer regardless of type).
void *ByGuid(int typeMask, uint64_t guid, const char *debugName = nullptr,
             int priority = 0x172);

} // namespace Object
