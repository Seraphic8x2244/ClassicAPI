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

// Resolve `guid` to its live client object. `typeMask` is a bitmask of
// `Offsets::OBJ_TYPE_*` (e.g. `OBJ_TYPE_UNIT | OBJ_TYPE_PLAYER`, or
// `TYPEMASK_UNIT` / `TYPEMASK_OBJECT`). Returns nullptr when the GUID isn't
// currently in the object table (out of range, despawned, pre-world).
// Non-throwing — safe from any context (Lua callback, world tick, packet
// hook). `priority` mirrors the engine's call-site priority argument: 0x172
// for a normal resolve, 0 on the loot/enumeration paths. `debugName` is an
// engine diagnostic label (may be nullptr).
//
// Returns `void *` — callers static_cast to the concrete object type they
// expect (the engine hands back the same pointer regardless of type).
void *ByGuid(int typeMask, uint64_t guid, const char *debugName = nullptr,
             int priority = 0x172);

} // namespace Object
