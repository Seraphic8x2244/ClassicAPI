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

// `GetMouseFoci()` -> table of every frame under the cursor, top-first.
//
// Modern WoW exposes GetMouseFoci() alongside GetMouseFocus(), with the
// invariant `GetMouseFocus() == GetMouseFoci()[1]`. Vanilla stores only the
// single topmost focus — Script_GetMouseFocus (0x0048DF40) reads
// `*(*VAR_UI_CONTEXT_PTR + OFF_UI_CONTEXT_MOUSE_FOCUS)` and there is no stored
// stack — so this is a native reimplementation of the frame-stack walk
// Blizzard_DebugTools does in Lua (`EnumerateFrames` + `IsMouseOver`):
//
//   1. Walk the engine frame list (the same one Script_EnumerateFrames
//      0x00705F60 iterates): head at `*(ctx + OFF_UI_CONTEXT_FRAME_LIST_HEAD)`,
//      each frame's next at `+OFF_FRAME_ENUM_NEXT`, a low-bit-set / null node
//      ends it.
//   2. Keep frames that are visible + mouse-enabled and whose rect contains the
//      cursor (hit-test mirrors Frame::Modern's IsMouseOver: Region getters +
//      cursor / effective-scale).
//   3. Order top-first by strata rank then frame level.
//   4. Force index 1 to be the engine's stored focus so the documented
//      invariant holds exactly, regardless of how our sort tie-breaks.
//
// Pure query — no hooks, registered like GetMouseFocus.

#include "Game.h"
#include "Offsets.h"
#include "ui/FrameObject.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Frame::MouseFoci {

namespace {

using ScriptFn_t = int(__fastcall *)(void *L);

int CallScript(uintptr_t fn, void *L) {
    return reinterpret_cast<ScriptFn_t>(fn)(L);
}

// The UI context is a pointer stored at VAR_UI_CONTEXT_PTR (null pre-world).
void *UIContext() {
    return Game::Read<void *>(static_cast<uintptr_t>(Offsets::VAR_UI_CONTEXT_PTR));
}

// Strata name -> draw rank, low (behind) to high (on top). Unknown -> 0.
int StrataRank(const char *s) {
    if (s == nullptr)
        return 0;
    static const struct {
        const char *name;
        int rank;
    } kStrata[] = {
        {"WORLD", 0},      {"BACKGROUND", 1},        {"LOW", 2},
        {"MEDIUM", 3},     {"HIGH", 4},              {"DIALOG", 5},
        {"FULLSCREEN", 6}, {"FULLSCREEN_DIALOG", 7}, {"TOOLTIP", 8},
    };
    for (const auto &e : kStrata)
        if (std::strcmp(s, e.name) == 0)
            return e.rank;
    return 0;
}

// Region getter that pushes one number, called with `self` at Lua idx 1.
// Returns false when it pushed nil/none (unresolved rect).
bool SelfNumber(void *L, uintptr_t fn, double *out) {
    Game::Lua::SetTop(L, 1); // (self)
    CallScript(fn, L);       // (self, value | nil)
    if (!Game::Lua::IsNumber(L, 2))
        return false;
    *out = Game::Lua::ToNumber(L, 2);
    return true;
}

// Boolean predicate (IsVisible / IsMouseEnabled), `self` at Lua idx 1.
bool SelfBool(void *L, uintptr_t fn) {
    Game::Lua::SetTop(L, 1);
    CallScript(fn, L);
    return Game::Lua::ToBoolean(L, 2) != 0;
}

struct Cursor {
    double x, y;
};

// GetCursorPosition and the Region rect getters share the same scale factor;
// dividing the cursor by the frame's effective scale (the field GetLeft/etc.
// themselves divide by) puts both in the frame's logical space. Mirrors
// Frame::Modern's IsMouseOver exactly.
Cursor ReadCursor(void *L) {
    Game::Lua::SetTop(L, 0);
    CallScript(Offsets::FUN_SCRIPT_GETCURSORPOSITION, L); // (x, y)
    return Cursor{Game::Lua::ToNumber(L, 1), Game::Lua::ToNumber(L, 2)};
}

// With `self` at Lua idx 1, is the cursor within the frame's rect?
bool CursorOverSelf(void *L, void *obj, const Cursor &cur) {
    const float effScale = Game::Read<float>(obj, Offsets::OFF_REGION_EFFECTIVE_SCALE);
    if (effScale == 0.0f)
        return false; // unpositioned region — no rect
    double left, right, top, bottom;
    if (!SelfNumber(L, Offsets::FUN_SCRIPT_REGION_GETLEFT, &left) ||
        !SelfNumber(L, Offsets::FUN_SCRIPT_REGION_GETRIGHT, &right) ||
        !SelfNumber(L, Offsets::FUN_SCRIPT_REGION_GETTOP, &top) ||
        !SelfNumber(L, Offsets::FUN_SCRIPT_REGION_GETBOTTOM, &bottom))
        return false;
    const double x = cur.x / effScale;
    const double y = cur.y / effScale;
    return x >= left && x <= right && y >= bottom && y <= top;
}

struct Hit {
    void *obj;
    int strata;
    int level;
};

int __fastcall Script_GetMouseFoci(void *L) {
    void *ctx = UIContext();
    Game::Lua::SetTop(L, 0);
    if (ctx == nullptr) {
        Game::Lua::NewTable(L); // pre-world → empty table
        return 1;
    }

    // The engine's authoritative topmost focus — the frame GetMouseFocus
    // returns. Forced to index 1 below.
    void *focus = Game::Read<void *>(ctx, Offsets::OFF_UI_CONTEXT_MOUSE_FOCUS);

    const Cursor cur = ReadCursor(L);

    std::vector<Hit> hits;
    void *node = Game::Read<void *>(ctx, Offsets::OFF_UI_CONTEXT_FRAME_LIST_HEAD);
    while (node != nullptr && (reinterpret_cast<uintptr_t>(node) & 1) == 0) {
        // Read the next link before any Lua work (the walk is engine-list
        // based, independent of the Lua stack, but this keeps it obvious).
        void *next = Game::Read<void *>(node, Offsets::OFF_FRAME_ENUM_NEXT);

        Game::Lua::SetTop(L, 0);
        UI::FrameObject::Push(L, node); // (self)

        if (SelfBool(L, Offsets::FUN_SCRIPT_FRAME_IS_VISIBLE) &&
            SelfBool(L, Offsets::FUN_SCRIPT_FRAME_IS_MOUSE_ENABLED) &&
            CursorOverSelf(L, node, cur)) {
            Game::Lua::SetTop(L, 1);
            CallScript(Offsets::FUN_SCRIPT_FRAME_GET_STRATA, L); // (self, strata)
            const int rank = StrataRank(Game::Lua::ToString(L, 2));
            const int level = Game::Read<int>(node, Offsets::OFF_FRAME_LEVEL);
            hits.push_back(Hit{node, rank, level});
        }

        node = next;
    }

    // Top-first: higher strata, then higher level within a strata.
    std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
        if (a.strata != b.strata)
            return a.strata > b.strata;
        return a.level > b.level;
    });

    // Order the frames, forcing GetMouseFocus() to be [1] so the invariant
    // holds by construction (focus may be a frame our rect test skips, e.g.
    // the WorldFrame, or a same-strata/level tie our sort orders differently).
    std::vector<void *> ordered;
    ordered.reserve(hits.size() + 1);
    if (focus != nullptr)
        ordered.push_back(focus);
    for (const Hit &h : hits)
        if (h.obj != focus)
            ordered.push_back(h.obj);

    Game::Lua::SetTop(L, 0);
    Game::Lua::NewTable(L); // result at idx 1
    for (size_t i = 0; i < ordered.size(); ++i) {
        Game::Lua::PushNumber(L, static_cast<double>(i + 1));
        UI::FrameObject::Push(L, ordered[i]);
        Game::Lua::RawSet(L, 1); // result[i+1] = frame
    }
    return 1;
}

void Register() {
    Game::Lua::RegisterGlobalFunction("GetMouseFoci", &Script_GetMouseFoci);
}

const Game::ModuleAutoRegister _autoreg{&Register};

} // namespace

} // namespace Frame::MouseFoci
