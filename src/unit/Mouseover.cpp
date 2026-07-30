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

// UPDATE_MOUSEOVER_UNIT loss-fire polyfill.
//
// Vanilla fires `UPDATE_MOUSEOVER_UNIT` only when a mouseover is *gained*
// (the engine's `FUN_SET_MOUSEOVER_UNIT` fires it inline in the
// friendly/hostile-unit tooltip case). When the cursor leaves the unit,
// the engine clears the mouseover GUID but fires nothing — so an addon
// that reacted to `UnitExists("mouseover")` becoming true never gets told
// when it becomes false. Retail fires the event for both gain and loss.
//
// We close the gap by co-hooking the single set/clear chokepoint (it is
// the ONLY writer of the mouseover GUID globals, so every gain / loss /
// change flows through it): resolve whether the mouseover was a *unit*
// before the original, then after, and fire `UPDATE_MOUSEOVER_UNIT` when
// it went from a unit to a non-unit.
//
// The unit-vs-non-unit test matters: the mouseover GUID also holds
// gameobjects (and items), but the engine only fires the *gain* event for
// units — the gameobject/item cases never fire it. So a naive
// "GUID present -> absent" test spuriously fires when you move off a
// gameobject that never fired a gain. Gating on "old was a unit, new
// isn't" fixes that while still matching retail's other cases:
//
//   * unit -> nothing      : fire  (retail fires)
//   * unit -> gameobject   : fire  (retail fires — mouseover unit lost)
//   * gameobject -> nothing: no    (bug fixed; no gain ever fired)
//   * X -> unit            : engine already fired inline; we don't (new is
//                            a unit, so our `!newIsUnit` guard is false —
//                            no double-fire on gain or unit->unit change)
//
// The event carries no arguments in vanilla (same as retail); handlers
// read `UnitExists("mouseover")` — which is false at fire time on the loss
// path, exactly matching retail semantics.

#include "Game.h"
#include "Offsets.h"

#include <cstdint>

namespace Unit::Mouseover {

namespace {

// `FUN_SET_MOUSEOVER_UNIT` is `__stdcall(guidLo, guidHi, arg3, arg4)` —
// see Offsets.h. We don't use the args; they pass straight through.
using SetMouseover_t = void(__stdcall *)(uint32_t guidLo, uint32_t guidHi,
                                         uint32_t arg3, uint32_t arg4);

using FireEventNoArgs_t = void(__fastcall *)(int eventID);

// `FUN_OBJECT_RESOLVE_BY_GUID(typeMask, debugName, guidLo, guidHi, prio)`
// — keeps the object only if its type flags intersect `typeMask`, so the
// unit|player mask returns non-null only for a live unit / player / pet /
// MC'd creature (never a gameobject or item). Same primitive
// `Unit::Flags::ResolveUnitOrPlayerByGuid` uses.
using ResolveObjectByGuid_t = void *(__fastcall *)(int typeMask,
                                                   const char *debugName,
                                                   uint32_t guidLo,
                                                   uint32_t guidHi,
                                                   int priority);

// True iff the current mouseover GUID resolves to a unit (not a
// gameobject / item / nothing).
bool MouseoverIsUnit() {
    const uint32_t lo = *reinterpret_cast<const uint32_t *>(
        static_cast<uintptr_t>(Offsets::VAR_MOUSEOVER_GUID_LO));
    const uint32_t hi = *reinterpret_cast<const uint32_t *>(
        static_cast<uintptr_t>(Offsets::VAR_MOUSEOVER_GUID_HI));
    if ((lo | hi) == 0)
        return false;
    constexpr int kUnitOrPlayerMask =
        (1 << Offsets::OBJECT_TYPE_UNIT) | (1 << Offsets::OBJECT_TYPE_PLAYER);
    auto fn = reinterpret_cast<ResolveObjectByGuid_t>(
        static_cast<uintptr_t>(Offsets::FUN_OBJECT_RESOLVE_BY_GUID));
    return fn(kUnitOrPlayerMask, "UPDATE_MOUSEOVER_UNIT", lo, hi, 0) != nullptr;
}

SetMouseover_t SetMouseover_o = nullptr;

void __stdcall SetMouseover_h(uint32_t guidLo, uint32_t guidHi,
                              uint32_t arg3, uint32_t arg4) {
    const bool hadUnit = MouseoverIsUnit();
    SetMouseover_o(guidLo, guidHi, arg3, arg4);
    const bool hasUnit = MouseoverIsUnit();

    // Only the unit-loss transition is missing from the engine. Any
    // ->unit transition (gain / unit->unit change) already fired inline
    // in the original, so `!hasUnit` prevents a double-fire there.
    if (hadUnit && !hasUnit) {
        auto fire = reinterpret_cast<FireEventNoArgs_t>(
            static_cast<uintptr_t>(Offsets::FUN_FIRE_EVENT_NO_ARGS));
        fire(Offsets::EVENT_UPDATE_MOUSEOVER_UNIT);
    }
}

static const Game::HookAutoRegister _hookreg{
    Offsets::FUN_SET_MOUSEOVER_UNIT,
    reinterpret_cast<void *>(&SetMouseover_h),
    reinterpret_cast<void **>(&SetMouseover_o)};

} // namespace

} // namespace Unit::Mouseover
