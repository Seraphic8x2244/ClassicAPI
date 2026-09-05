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

#include "Game.h"

// Shared `vector2` marshalling for the `C_Map` surface. Map positions cross
// the C/Lua boundary as `Vector2DMixin` objects (so consumers can call
// `pos:GetXY()` as well as read `.x` / `.y`), and several modules both return
// and accept them — this keeps the stack handling in one place.

namespace Map {

// Pushes a `Vector2DMixin` built from (x, y) via the `CreateVector2D` global.
// Falls back to a plain `{x=, y=}` table when that global isn't reachable
// (the addon half isn't loaded). Leaves exactly one value on the stack.
inline void PushVector2D(void *L, double x, double y) {
    const int base = Game::Lua::GetTop(L);
    Game::Lua::PushString(L, "CreateVector2D");
    Game::Lua::GetTable(L, Game::Lua::GLOBALS_INDEX);
    Game::Lua::PushNumber(L, x);
    Game::Lua::PushNumber(L, y);
    if (Game::Lua::PCall(L, 2, 1, 0) == 0)
        return; // Vector2DMixin object on top
    // pcall failed (global missing / not callable): drop the error object and
    // build a plain coordinate table instead.
    Game::Lua::SetTop(L, base);
    Game::Lua::NewTable(L);
    Game::Lua::SetFieldNumber(L, "x", x);
    Game::Lua::SetFieldNumber(L, "y", y);
}

// Reads `t.x` / `t.y` from the table at absolute stack index `idx` (a
// `Vector2DMixin` or a plain `{x, y}`). `idx` must be positive so it survives
// the key pushes. Returns false when the slot isn't a table. Stack-balanced.
inline bool ReadVector2D(void *L, int idx, double *x, double *y) {
    if (Game::Lua::Type(L, idx) != Game::Lua::TYPE_TABLE)
        return false;
    Game::Lua::PushString(L, "x");
    Game::Lua::GetTable(L, idx);
    *x = Game::Lua::ToNumber(L, -1);
    Game::Lua::SetTop(L, -2);
    Game::Lua::PushString(L, "y");
    Game::Lua::GetTable(L, idx);
    *y = Game::Lua::ToNumber(L, -1);
    Game::Lua::SetTop(L, -2);
    return true;
}

} // namespace Map
