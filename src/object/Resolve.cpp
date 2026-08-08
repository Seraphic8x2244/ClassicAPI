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

#include "Resolve.h"

#include "Offsets.h"

namespace Object {

void *ByGuid(int typeMask, uint64_t guid, const char *debugName, int priority) {
    using Fn = void *(__fastcall *)(int typeMask, const char *debugName,
                                    uint32_t guidLo, uint32_t guidHi,
                                    int priority);
    auto fn = reinterpret_cast<Fn>(
        static_cast<uintptr_t>(Offsets::FUN_OBJECT_RESOLVE_BY_GUID));
    return fn(typeMask, debugName, static_cast<uint32_t>(guid),
              static_cast<uint32_t>(guid >> 32), priority);
}

} // namespace Object
