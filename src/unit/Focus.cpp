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
//
// Unit events for `focus` / `focustarget`: `Unit::TokenObserver` watches
// the focus unit's descriptor fields so `UNIT_HEALTH` / `UNIT_AURA` / …
// fire with `arg1 == "focus"` (see FocusFieldCb). `focustarget` gets the
// same treatment via a second observer — but its GUID is the focus unit's
// *current target*, which changes with no event to signal it (vanilla's
// `UNIT_FIELD_TARGET`, field 10, has no event-table slot). So the WorldTick
// watcher re-reads the focus's target each tick and re-points the
// focustarget observer only when that identity changes; the focustarget
// *events* still fire event-driven through the observer — only the
// re-pointing is tick-gated (≤1 tick lag after the focus swaps targets).

#include "Focus.h"

#include "Game.h"
#include "Offsets.h"
#include "event/Custom.h"
#include "object/Resolve.h"
#include "tick/WorldTick.h"
#include "unit/TokenObserver.h"

#include <cstdint>

namespace Unit::Focus {

namespace {

constexpr const char *kEventName = "PLAYER_FOCUS_CHANGED";
const Event::Custom::AutoReserve _reserve{kEventName};

uint64_t g_focusGUID = 0;

// True while the `"focus"` descriptor-field observers are currently registered
// on a LIVE focus object. Cleared when the object despawns (the nodes die with
// it) so `OnWorldTick` re-registers when a groupmate returns to range; guards
// against double-registering while the object stays resolvable.
bool g_focusObserved = false;

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

// GUID currently watched as `focustarget` (the focus unit's target), or 0.
// Re-pointed by `OnWorldTick`; distinct from `g_focusGUID`.
uint64_t g_focusTargetGUID = 0;

// Same as `FocusFieldCb` but fires the `"focustarget"` token.
int __fastcall FocusTargetFieldCb(uint32_t fieldOffset, uint32_t /*size*/,
                                  uint32_t guidLo, uint32_t guidHi,
                                  const uint32_t * /*oldValue*/, void * /*userArg*/) {
    if (guidLo != static_cast<uint32_t>(g_focusTargetGUID) ||
        guidHi != static_cast<uint32_t>(g_focusTargetGUID >> 32))
        return 1;
    using Fire_t = void(__cdecl *)(int eventId, const char *fmt, ...);
    reinterpret_cast<Fire_t>(static_cast<uintptr_t>(Offsets::FUN_FIRE_EVENT))(
        static_cast<int>(fieldOffset >> 2), "%s", "focustarget");
    return 1;
}

using TokenToGUID_t = uint64_t(__fastcall *)(const char *token);

uint64_t ResolveTokenGUID(const char *token) {
    if (token == nullptr || *token == '\0')
        return 0;
    auto fn = reinterpret_cast<TokenToGUID_t>(
        static_cast<uintptr_t>(Offsets::FUN_TOKEN_TO_GUID));
    return fn(token);
}

// Resolve a GUID to its live CGUnit via the object manager, or null if it
// isn't currently in the client's object table (out of range / despawned /
// pre-world). Non-throwing — safe from any context.
const uint8_t *ResolveObject(uint64_t guid) {
    if (guid == 0)
        return nullptr;
    return static_cast<const uint8_t *>(
        Object::ByGuid(Offsets::TYPEMASK_UNIT, guid, "Focus", 0x172));
}

// True if `guid` is a current party or raid member. Reads the roster GUID
// storage directly — the 4-slot party GUID array and the 40-slot raid
// member-pointer array (GUID at `*member + 0`) — which are populated from the
// group roster independent of the object table, so this answers correctly for
// a groupmate whose CGUnit has been destroyed by leaving your range. Mirrors
// 3.3.5's focus-clear gate `FUN_00512a30` (= `FUN_0052d310` party-member OR
// `FUN_00573200` raid-member).
bool IsGroupMemberGuid(uint64_t guid) {
    if (guid == 0)
        return false;
    const auto *party = reinterpret_cast<const uint64_t *>(
        static_cast<uintptr_t>(Offsets::VAR_PARTY_GUIDS));
    for (int i = 0; i < Offsets::PARTY_MAX_SLOTS; ++i)
        if (party[i] == guid)
            return true;
    const int raidCount = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_RAID_MEMBER_COUNT));
    if (raidCount > 0) {
        const auto *const *raid = reinterpret_cast<const uint8_t *const *>(
            static_cast<uintptr_t>(Offsets::VAR_RAID_MEMBER_PTRS));
        for (int i = 0; i < Offsets::RAID_MAX_SLOTS; ++i) {
            const uint8_t *m = raid[i];
            if (m != nullptr && *reinterpret_cast<const uint64_t *>(m) == guid)
                return true;
        }
    }
    return false;
}

// Per-tick focus watcher. Resolving `g_focusGUID` from the object table is
// both the despawn check and (when alive) the `focustarget` read.
//
// When the object is gone (out of range, LoS, despawn) we mirror 3.3.5's focus
// gate: a **party/raid member keeps focus** — their CGUnit is destroyed when
// they leave your range, but the roster GUID persists, so focus reattaches
// when they return — while any **other** unit drops focus, exactly as
// vanilla/3.3.5 do on a real despawn. (Verified against 3.3.5's object-teardown
// purge `FUN_00524350`, gated by the party-or-raid check `FUN_00512a30`.)
// Vanilla fires no re-target event, so this once-per-tick read is also how
// `focustarget` follows the focus switching targets; its events stay
// observer-driven below.
void OnWorldTick() {
    uint64_t desiredFocusTarget = 0;
    if (g_focusGUID != 0) {
        const uint8_t *focusObj = ResolveObject(g_focusGUID);
        if (focusObj == nullptr) {
            if (IsGroupMemberGuid(g_focusGUID)) {
                // Groupmate out of range: keep focus. Its observers died with
                // the object; OnWorldTick re-registers them when it resolves.
                g_focusObserved = false;
            } else {
                Set(0); // non-group unit left the object table → drop focus
            }
        } else {
            // (Re)attach the unit-event observers on the live object — covers a
            // groupmate returning to range, and a focus set while out of range.
            if (!g_focusObserved) {
                TokenObserver::Register(g_focusGUID, &FocusFieldCb);
                g_focusObserved = true;
            }
            auto *fields = *reinterpret_cast<const uint8_t *const *>(
                focusObj + Offsets::OFF_CGUNIT_OBJECT_FIELDS);
            if (fields != nullptr)
                desiredFocusTarget = *reinterpret_cast<const uint64_t *>(
                    fields + Offsets::OFF_UNIT_FIELD_TARGET);
        }
    }

    // Re-point the focustarget observer only when the target identity
    // changes (focus swapped targets, focus cleared, or focus has no target).
    if (desiredFocusTarget != g_focusTargetGUID) {
        if (g_focusTargetGUID != 0)
            TokenObserver::Unregister(g_focusTargetGUID, &FocusTargetFieldCb);
        g_focusTargetGUID = desiredFocusTarget;
        if (g_focusTargetGUID != 0)
            TokenObserver::Register(g_focusTargetGUID, &FocusTargetFieldCb);
    }
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

// Drop focus when the player logs out to the glue screen. The DLL isn't
// reloaded on a logout→character-select→login, so g_focusGUID would
// otherwise carry a dead GUID from character A into character B. The
// WorldTick despawn probe self-heals it within a tick (ObjectByGUID miss
// → Set(0)), but that fires a spurious PLAYER_FOCUS_CHANGED on B's first
// tick and can't cover the (astronomically rare) GUID-collision case, so
// clearing here is both cleaner and exact retail parity. We use the glue
// hook, NOT FrameScript_Initialize, because it fires on the world→glue
// return a logout triggers but NOT on /reload — so focus correctly
// survives /reload (retail behavior) and drops only on logout. No event /
// observer work: char A's focus observers already died with the object
// during world teardown, and B starts with focus nil (no event expected).
void ClearOnLogout() {
    g_focusGUID = 0;
    g_focusTargetGUID = 0;
    g_focusObserved = false;
}
const Game::GlueModuleAutoRegister _clearOnLogout{&ClearOnLogout};

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
    g_focusObserved = false;
    // Focus changed → the old focustarget is stale; drop its observer now.
    // WorldTick re-resolves and re-points to the new focus's target next tick.
    if (g_focusTargetGUID != 0) {
        TokenObserver::Unregister(g_focusTargetGUID, &FocusTargetFieldCb);
        g_focusTargetGUID = 0;
    }
    g_focusGUID = guid;
    // Register the unit-event observers only against a LIVE object. If the new
    // focus is currently out of range (e.g. /focus a raid member across the
    // map), OnWorldTick registers once it resolves — keeping the observers tied
    // to a real object instance rather than an absent GUID.
    if (guid != 0 && ResolveObject(guid) != nullptr) {
        TokenObserver::Register(guid, &FocusFieldCb);
        g_focusObserved = true;
    }
    const int slot = Event::Custom::Lookup(kEventName);
    if (slot >= 0)
        Event::Custom::Fire(slot, "");
}

} // namespace Unit::Focus
