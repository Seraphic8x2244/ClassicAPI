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

// `FocusUnit(unit)` / `ClearFocus()` / `focus` unit token. Polyfills
// the post-vanilla focus-target system on top of the engine's
// existing token resolver via the hook in
// `unit/TokenExtensions.cpp`.
//
// Storage: a single 64-bit GUID held in-process. The 3.3.5 engine
// keeps the same shape (two globals at `DAT_00BD07D0`/`D4`); we
// don't share those because vanilla never reads them. The resolver
// hook reads our `Unit::Focus::Get()` directly.
//
// Setter mirrors 3.3.5's `FUN_0051FF20` minus the post-vanilla
// `UnitFlag bit 0x2000` ("focus glow" rendering hint, introduced in
// TBC for the default focus frame). Vanilla addons (pfUI) render
// their own focus indicator and don't need the hint.
//
// Auto-clear on despawn: modern WoW fires `PLAYER_FOCUS_CHANGED`
// when the focused unit leaves the client's object table (out of
// rendering range, despawn) and does NOT auto-refocus when they
// come back. We mirror that by probing `ObjectByGUID(g_focusGUID)`
// every world tick — when it returns null, we `Set(0)` which fires
// the event. Cost is one hash-table lookup per tick while focus is
// set; zero when no focus is active.

#include "Focus.h"

#include "Game.h"
#include "Offsets.h"
#include "event/Custom.h"
#include "tick/WorldTick.h"
#include "unit/TokenObserver.h"

#include <cstdint>

namespace Unit::Focus {

namespace {

constexpr const char *kEventName = "PLAYER_FOCUS_CHANGED";
const Event::Custom::AutoReserve _reserve{kEventName};

uint64_t g_focusGUID = 0;

// Descriptor-field observer callback — fires the changed field's unit event
// with the `"focus"` token, making `arg1 == "focus"` work for `UNIT_HEALTH`,
// `UNIT_MANA`, `UNIT_AURA`, … exactly like a native token. Registered per
// focus unit via `Unit::TokenObserver` (mirrors the engine's own watch of
// target/party/raid units). The event id is the field index (`fieldOffset >>
// 2`); the engine fires every unit event with format "%s" + the token, so we
// match that. Guarded on the live focus GUID so an in-flight field change that
// races an unregister can't fire a stale token.
int __fastcall FocusFieldCb(uint32_t fieldOffset, uint32_t /*size*/,
                            uint32_t guidLo, uint32_t guidHi,
                            const uint32_t * /*oldValue*/, void * /*userArg*/) {
    if (guidLo != static_cast<uint32_t>(g_focusGUID) ||
        guidHi != static_cast<uint32_t>(g_focusGUID >> 32))
        return 1;
    using Fire_t = void(__cdecl *)(int eventId, const char *fmt, ...);
    reinterpret_cast<Fire_t>(static_cast<uintptr_t>(Offsets::FUN_FIRE_EVENT))(
        static_cast<int>(fieldOffset >> 2), "%s", "focus");
    return 1;
}

using TokenToGUID_t = uint64_t(__fastcall *)(const char *token);
using ResolveByGUID_t = void *(__fastcall *)(int type, const char *debugName,
                                              uint32_t guidLo, uint32_t guidHi,
                                              int priority);

uint64_t ResolveTokenGUID(const char *token) {
    if (token == nullptr || *token == '\0')
        return 0;
    auto fn = reinterpret_cast<TokenToGUID_t>(
        static_cast<uintptr_t>(Offsets::FUN_TOKEN_TO_GUID));
    return fn(token);
}

// Per-tick despawn watcher. Probes the engine's object table for
// `g_focusGUID`; when it disappears (out of range, fully despawned),
// clear focus and fire PLAYER_FOCUS_CHANGED — matches modern's
// "leaves render distance → focus drops" behavior. Won't refocus
// when the unit comes back, also matching modern.
void OnWorldTick() {
    if (g_focusGUID == 0)
        return;
    auto resolve = reinterpret_cast<ResolveByGUID_t>(
        static_cast<uintptr_t>(Offsets::FUN_OBJECT_RESOLVE_BY_GUID));
    if (resolve(Offsets::OBJ_TYPE_UNIT, "Focus",
                static_cast<uint32_t>(g_focusGUID),
                static_cast<uint32_t>(g_focusGUID >> 32),
                0x172) == nullptr)
        Set(0);
}

// `FocusUnit(unit)` — sets focus to whatever GUID `unit` currently
// resolves to. Modern signature: `FocusUnit(unit)` with the token
// argument. Modern also allows `FocusUnit()` (no arg) to mean
// `"target"`, matching the `/focus` slash command's default.
int __fastcall Script_FocusUnit(void *L) {
    if (Game::Lua::IsString(L, 1)) {
        const char *token = Game::Lua::ToString(L, 1);
        Set(ResolveTokenGUID(token));
    } else {
        Set(ResolveTokenGUID("target"));
    }
    return 0;
}

// `ClearFocus()` — drops the focus, fires PLAYER_FOCUS_CHANGED.
int __fastcall Script_ClearFocus(void *L) {
    (void)L;
    Set(0);
    return 0;
}

void RegisterLuaFunctions() {
    Game::Lua::RegisterGlobalFunction("FocusUnit", &Script_FocusUnit);
    Game::Lua::RegisterGlobalFunction("ClearFocus", &Script_ClearFocus);
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};
const Tick::WorldTick::AutoSubscribe _tickSub{&OnWorldTick};

} // namespace

uint64_t Get() { return g_focusGUID; }

void Set(uint64_t guid) {
    if (guid == g_focusGUID)
        return; // no-op: same target → no event fire (matches 3.3.5)
    // Stop watching the old focus, start watching the new one, so unit events
    // (UNIT_HEALTH, …) fire with "focus" for whatever is focused — even a unit
    // the engine isn't otherwise watching. Unregister is a safe no-op if the
    // old unit already despawned (nodes died with it).
    if (g_focusGUID != 0)
        TokenObserver::Unregister(g_focusGUID, &FocusFieldCb);
    g_focusGUID = guid;
    if (guid != 0)
        TokenObserver::Register(guid, &FocusFieldCb);
    const int slot = Event::Custom::Lookup(kEventName);
    if (slot >= 0)
        Event::Custom::Fire(slot, "");
}

} // namespace Unit::Focus
