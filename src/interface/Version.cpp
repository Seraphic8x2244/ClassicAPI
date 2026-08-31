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

// `INTERFACE_VERSION` global — the client's interface (TOC) version number,
// 11200 on this client. Read from the engine's own accessor (the same value
// the addon loader compares each `## Interface:` against) rather than a
// literal, and re-published on every `/reload` via the standard
// `ModuleAutoRegister` flow. It is a true fixed client constant, so it is a
// global number like `CLASSIC_API_VERSION`, not a function.

#include "Game.h"
#include "Offsets.h"

#include <cstdint>

namespace Interface::Version {

namespace {

using ClientIfaceVer_t = uint32_t(__cdecl *)();

void RegisterLuaFunctions() {
    void *L = Game::Lua::State();
    if (L == nullptr)
        return;
    const uint32_t version = reinterpret_cast<ClientIfaceVer_t>(
        static_cast<uintptr_t>(Offsets::FUN_ADDON_CLIENT_INTERFACE_VERSION))();
    Game::Lua::SetGlobalNumber(L, "INTERFACE_VERSION",
                               static_cast<double>(version));
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace

} // namespace Interface::Version
