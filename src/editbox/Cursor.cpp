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

// EditBox cursor methods — `editBox:SetCursorPosition(position)` and
// `editBox:GetCursorPosition()`, the modern widget methods vanilla's EditBox
// lacks (its 48-method registry has HighlightText / Insert / SetText but no
// cursor accessor). Modern addons that place the caret after programmatically
// setting text (autocomplete, chat helpers) call SetCursorPosition and error
// on 1.12 without it.
//
// Both are registered on the EditBox method registry, so — like every other
// per-type frame method — only EditBox frames dispatch to them; a Button or
// Frame calling `:SetCursorPosition()` still gets "method not found", so the
// resolved `this` is always a real CSimpleEditBox.
//
// The engine already stores and manipulates the caret; we just expose it. The
// cursor is a BYTE offset into the UTF-8 text buffer (field +0x36c), which is
// exactly what modern WoW's byte-based EditBox cursor uses — for ASCII text a
// byte offset equals a character index, so callers passing `#text` land at the
// end either way. Set path: the engine's own `FUN_EDITBOX_SET_CURSOR_BYTE`
// (clamp + set + cursor-dirty) then `FUN_EDITBOX_COLLAPSE_SELECTION` (clear any
// selection onto the caret), then flag the selection dirty so a prior highlight
// repaints away.

#include "Game.h"
#include "Offsets.h"

#include <cstdint>

namespace EditBox::Cursor {

namespace {

using SetCursorByte_t = void(__thiscall *)(void *editbox, int byteOffset);
using CollapseSelection_t = void(__thiscall *)(void *editbox);

int __fastcall Script_SetCursorPosition(void *L) {
    void *eb = Game::Lua::ResolveObject(L, 1);
    if (eb == nullptr || !Game::Lua::IsNumber(L, 2)) {
        Game::Lua::Error(L, "Usage: EditBox:SetCursorPosition(position)");
        return 0;
    }
    const int pos = static_cast<int>(Game::Lua::ToNumber(L, 2));
    reinterpret_cast<SetCursorByte_t>(
        Offsets::FUN_EDITBOX_SET_CURSOR_BYTE)(eb, pos);
    reinterpret_cast<CollapseSelection_t>(
        Offsets::FUN_EDITBOX_COLLAPSE_SELECTION)(eb);
    // COLLAPSE_SELECTION moves selStart/selEnd but doesn't flag them; set the
    // selection-dirty bit (2) so any highlight from before is cleared on the
    // next paint. SET_CURSOR_BYTE already set the cursor-dirty bit (4).
    Game::Ref<uint32_t>(eb, Offsets::OFF_EDITBOX_FLAGS) |= 2;
    return 0;
}

int __fastcall Script_GetCursorPosition(void *L) {
    void *eb = Game::Lua::ResolveObject(L, 1);
    if (eb == nullptr) {
        Game::Lua::Error(L, "Usage: EditBox:GetCursorPosition()");
        return 0;
    }
    Game::Lua::PushNumber(L, static_cast<double>(
        Game::Read<int>(eb, Offsets::OFF_EDITBOX_CURSOR_BYTE)));
    return 1;
}

const Game::Lua::FrameMethodEntry g_methods[] = {
    {"SetCursorPosition", &Script_SetCursorPosition},
    {"GetCursorPosition", &Script_GetCursorPosition},
};

void RegisterLuaFunctions() {
    Game::Lua::RegisterFrameMethods(
        reinterpret_cast<void *>(Offsets::VAR_EDITBOX_METHOD_REGISTRY),
        g_methods,
        static_cast<int>(sizeof(g_methods) / sizeof(g_methods[0])));
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace

} // namespace EditBox::Cursor
