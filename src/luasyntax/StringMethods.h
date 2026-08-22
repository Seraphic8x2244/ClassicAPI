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

namespace LuaSyntax::StringMethods {

// Forget the in-game state's captured `string` table. Called from
// FrameScript_Initialize_h BEFORE the engine resets (/reload) or destroys
// (/logout) the world Lua state, so the capture can never outlive the table
// it points at. Fails closed: until the ModuleAutoRegister re-capture at the
// next LoadScriptFunctions, strings error on index exactly as stock 1.12.
void PrepareForReload();

} // namespace LuaSyntax::StringMethods
