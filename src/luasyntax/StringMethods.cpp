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

// Lua 5.1 string methods (`("asd"):upper()`, `s:format(...)`) on the 5.0 VM.
//
// Lua 5.0 has no string methods AT ALL — the shared string metatable with
// `__index = string` is a 5.1 addition (`createmetatable` in 5.1's
// lstrlib.c). Nothing was stripped from this build: both halves of the
// would-be mechanism match stock 5.0 source exactly —
//   * `lua_setmetatable` (0x006F4020) accepts only tables/userdata, so no
//     API call can attach a metatable to the string TYPE; and
//   * `luaT_gettmbyobj` (0x006F7BD0) hardcodes `&nilobject` for anything
//     that isn't a table or userdata, so the VM would never read one.
// So this is the runtime half of the 5.1 backport (the syntax half is
// Transpile.cpp): we transplant 5.1's mechanism at the read side.
//
// Mechanism: co-hook FUN_LUA_INDEX_NONTABLE (0x006F7EE0) — luaV_gettable's
// non-table continuation, the ONE choke every string index reaches (3 call
// sites: luaV_gettable's else-branch + luaV_execute's OP_GETTABLE/OP_SELF
// fallbacks; see Offsets.h). When the indexed object is a string, answer
// with `luaV_gettable(L, <string library table>, key, loop)` — byte-for-byte
// what the original's own TM-table branch would do had gettmbyobj returned
// that table (it passes `loop` through unchanged too, 0x006F7F23). The
// engine's unmodified lookup then does everything, including exact 5.1 miss
// semantics: `("x").nonexistent` → plain table miss → nil, not an error
// (verified: FUN_006F7D60 returns &nilobject when the table has no
// metatable). Non-strings fall through to the original — userdata __index,
// and the stock "attempt to index" error for everything else. `settable`
// on a string is a different continuation (FUN_006F7F40), untouched: still
// an error, which is also 5.1's behavior.
//
// Hotness: cold. Table `__index` dispatch (all frame/OO method traffic)
// reads the table's OWN metatable via FUN_006F7D60/fasttm and never lands
// here; today's only callers are about-to-error non-table indexes. No other
// Octo DLL hooks the Lua VM internals.
//
// State/GC lifecycle: one slot per Lua state (in-game + glue), keyed by the
// state's global_State (`*(L+0x10)`) so coroutine threads — which share G
// with their parent — resolve too. Each capture anchors the string table in
// that state's registry, so the hook can never serve a GC-collected table:
// even if an addon nils/replaces `_G.string`, resolution keeps the snapshot
// (5.1 parity — its metatable pins the open-time table the same way).
// Captures re-run per /reload (ModuleAutoRegister) and per glue boot
// (GlueModuleAutoRegister), overwriting the anchor so the previous table is
// released. Between a state teardown and its re-capture a slot can be
// stale, but no Lua executes in those windows (engine C init only — and on
// this 5.0 engine no pre-existing code indexes strings anyway, since doing
// so always errored).

#include "Game.h"
#include "Offsets.h"

#include <cstdint>

namespace LuaSyntax::StringMethods {

namespace {

// `global_State *` slot in lua_State — the per-universe object shared by
// every coroutine thread of a state. Verified: luaT_gettmbyobj reads the
// tmname array at `*(L+0x10) + 0x80`, lua_setmetatable resolves the
// REGISTRY pseudo-index via `*(L+0x10) + 0x30`. Single-consumer offset,
// local per the Offsets.h rule.
constexpr uintptr_t OFF_L_GLOBALSTATE = 0x10;

// This build's TValue is 16 bytes with `tt` FIRST: {tt @ +0, value @ +8} —
// NOT stock 5.0's {value, tt} order. Verified from lua_setmetatable
// (tag = `*tv`, GC pointer = `tv[2]`, Table->metatable at hvalue+0x08 /
// fast-TM flags byte at hvalue+0x06) and the push helpers' 0x10 stride.
struct TValueMirror {
    int tt = 0;
    int pad0 = 0;
    const void *value = nullptr; // Table* when tt == TYPE_TABLE
    int pad1 = 0;
};
static_assert(sizeof(TValueMirror) == 16, "TValue is 16 bytes in this build");

// Captured `string` library table for one Lua state. `G` doubles as the
// valid flag — nullptr means "no capture, pass everything through".
struct StateSlot {
    const void *G = nullptr;
    TValueMirror tv{};
};
StateSlot g_gameSlot;
StateSlot g_glueSlot;

// Registry key anchoring the captured table against GC (rawset — the
// registry has no metatable). One key per state's registry; re-capture
// overwrites it, releasing the previously anchored table.
constexpr const char *kAnchorKey = "__classicapi_string_lib";

// FUN_LUA_INDEX_NONTABLE / FUN_LUA_V_GETTABLE shared convention:
// __fastcall(L /*ecx*/, tv /*edx*/, key /*stack*/, loop /*stack*/), RET 8,
// result TValue* in eax (see Offsets.h for the disassembly evidence).
using IndexResolve_t = void *(__fastcall *)(void *L, const void *tv, void *key, int loop);
IndexResolve_t g_indexNonTableOriginal = nullptr;

void *__fastcall IndexNonTable_h(void *L, const void *obj, void *key, int loop) {
    // 5.1 backport: a string's `__index` is the `string` library table.
    if (obj != nullptr && *static_cast<const int *>(obj) == Game::Lua::TYPE_STRING) {
        const void *G = Game::Read<const void *>(L, OFF_L_GLOBALSTATE);
        const StateSlot *slot = nullptr;
        if (g_gameSlot.G == G)
            slot = &g_gameSlot;
        else if (g_glueSlot.G == G)
            slot = &g_glueSlot;
        if (slot != nullptr && slot->tv.value != nullptr)
            return reinterpret_cast<IndexResolve_t>(Offsets::FUN_LUA_V_GETTABLE)(
                L, &slot->tv, key, loop);
    }
    return g_indexNonTableOriginal(L, obj, key, loop);
}

const Game::HookAutoRegister _hook{Offsets::FUN_LUA_INDEX_NONTABLE,
                                   reinterpret_cast<void *>(&IndexNonTable_h),
                                   reinterpret_cast<void **>(&g_indexNonTableOriginal)};

// Captures `_G.string` for the CURRENT state (whatever VAR_LUA_STATE points
// at — the registration callbacks below run inside the right window for
// each) into `slot`, anchoring it in that state's registry.
void CaptureFor(StateSlot &slot) {
    slot.G = nullptr;
    slot.tv.value = nullptr;
    void *L = Game::Lua::State();
    if (L == nullptr)
        return;
    Game::Lua::PushString(L, "string");
    Game::Lua::GetTable(L, Game::Lua::GLOBALS_INDEX); // [stringlib?]
    if (Game::Lua::Type(L, -1) == Game::Lua::TYPE_TABLE) {
        Game::Lua::PushString(L, kAnchorKey);            // [tbl, key]
        Game::Lua::PushValue(L, -2);                     // [tbl, key, tbl]
        Game::Lua::RawSet(L, Game::Lua::REGISTRY_INDEX); // registry[key]=tbl. [tbl]
        slot.tv.tt = Game::Lua::TYPE_TABLE;
        slot.tv.value = Game::Lua::ToPointer(L, -1); // hvalue (Table*)
        slot.G = Game::Read<const void *>(L, OFF_L_GLOBALSTATE);
    }
    Game::Lua::SetTop(L, -2); // pop
}

void RegisterLuaFunctions() { CaptureFor(g_gameSlot); }
void RegisterGlueFunctions() { CaptureFor(g_glueSlot); }

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};
const Game::GlueModuleAutoRegister _glueAutoreg{&RegisterGlueFunctions};

} // namespace

} // namespace LuaSyntax::StringMethods
