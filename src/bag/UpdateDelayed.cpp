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

// `BAG_UPDATE_DELAYED` event — fires once per frame in which any
// `BAG_UPDATE` fired. Polyfills modern WoW's event of the same name
// (added 5.4.x) with exact same-semantic coalescing: addons listen
// to DELAYED instead of BAG_UPDATE and rescan once per frame.
//
// See TODO #67 for the full investigation history.
//
// Architecture: three MinHooks plus one tick subscription.
//
//   1. `FUN_004F91A0` — bag-slot diff loop. Hook just sets the
//      `g_pending` flag.
//   2. `FUN_004F9370` — item→bag resolver (most common path). Hook
//      just sets the `g_pending` flag.
//   3. `FUN_004F8DB0` — bag-descriptor change callback. Covers the
//      keyring `BAG_UPDATE(slot)` direct-fire branch that doesn't
//      route through `FUN_004F9370`.
//   4. `Tick::WorldTick::AutoSubscribe` — shared per-frame
//      subscription (the engine's world-subsystem update at
//      `FUN_0066FD50`, single-hooked in `src/tick/WorldTick.cpp`).
//      Our callback drains `g_pending` and fires DELAYED if set.
//
// All hook targets are quiet (≤3 callers each, in code regions
// other DLLs don't touch). The previous attempt at this feature
// hooked `FUN_FIRE_EVENT` (100+ callers, in the chat path) and a
// per-frame UI dispatcher — both prime collision targets for
// SuperWoWhook / nampower / transmogfix. By contrast:
//
//   - `FUN_0066FD50` is the WORLD-subsystem update (not the UI one),
//     deep in the rendering pipeline. Only one caller, no DLL
//     touches it. It runs exactly once per frame and increments
//     the engine's own per-frame counter at `0x00C7B2C8`, so we
//     know it ticks 1:1 with frames.
//   - The bag-subsystem hooks (`FUN_004F91A0`, `FUN_004F9370`,
//     `FUN_004F8DB0`) live in the `0x004F8xxx`/`0x004F9xxx` region
//     none of the popular DLLs care about.
//
// Coverage verified by pattern-searching the `.text` for every
// caller of `FUN_FIRE_EVENT(0x148, "%d", ...)` (the BAG_UPDATE
// event ID): exactly three sites, all covered by these three
// hooks. Double-fire concern with `FUN_004F8DB0` → `FUN_004F9370`
// path is moot: `g_pending` is a bool, idempotent until the
// per-frame drain clears it.

#include "Game.h"
#include "Offsets.h"
#include "bag/UpdateDelayed.h"
#include "event/Custom.h"
#include "tick/WorldTick.h"

#include <cstdint>

namespace Bag::UpdateDelayed {

namespace {

constexpr const char *kEventName = "BAG_UPDATE_DELAYED";

const Event::Custom::AutoReserve _reserve{kEventName};

// Set by the bag-update hooks, drained by the world-tick hook.
// Single bool, no atomics needed — the engine is single-threaded.
bool g_pending = false;

// C++ subscribers notified once per bag-change frame (see UpdateDelayed.h).
AutoSubscribe *g_subs = nullptr;

} // namespace

AutoSubscribe::AutoSubscribe(Callback cb) : cb(cb), next(g_subs) {
    g_subs = this;
}

// `FUN_004F91A0` — `__cdecl void()`. Bag-slot diff loop. Walks the
// 10 bag slots, fires BAG_UPDATE for each that changed.
using BagSlotDiff_t = void(__cdecl *)();
BagSlotDiff_t BagSlotDiff_o = nullptr;
void __cdecl BagSlotDiff_h() {
    BagSlotDiff_o();
    g_pending = true;
}

// `FUN_004F9370` — `__stdcall void(int guidLo, int guidHi)`. Most
// common BAG_UPDATE path — fires from every per-item descriptor
// field change.
using BagItemToBag_t = void(__stdcall *)(int guidLo, int guidHi);
BagItemToBag_t BagItemToBag_o = nullptr;
void __stdcall BagItemToBag_h(int guidLo, int guidHi) {
    BagItemToBag_o(guidLo, guidHi);
    g_pending = true;
}

// `FUN_004F8DB0` — `__thiscall unsigned(this, a1, a2, a3, a4)` in the
// Octo build. Wired with `__fastcall` + dummy EDX to match the
// thiscall ABI: ECX takes `this`, EDX unused, stack takes the four
// dword args, callee cleans 16 bytes (`RET 0x10`). Same trick
// `Frame::RegisterEvent` uses in DllMain, just with four stack args
// instead of one — the Octo function is wider than the vanilla
// signature Ghidra showed when this hook was first written.
using KeyringDescChange_t = unsigned(__fastcall *)(void *bagDesc, void *edx_unused,
                                                    unsigned a1, unsigned a2,
                                                    unsigned a3, unsigned a4);
KeyringDescChange_t KeyringDescChange_o = nullptr;
unsigned __fastcall KeyringDescChange_h(void *bagDesc, void *edx,
                                         unsigned a1, unsigned a2,
                                         unsigned a3, unsigned a4) {
    const unsigned result = KeyringDescChange_o(bagDesc, edx, a1, a2, a3, a4);
    g_pending = true;
    return result;
}

// Per-frame drain. Runs at the tail of each world tick (subscribed
// through `Tick::WorldTick`, which owns the single hook on
// `FUN_0066FD50`). All this frame's BAG_UPDATEs have already fired
// by the time we run; we just consume the pending flag and emit one
// DELAYED if set.
void OnWorldTick() {
    if (!g_pending)
        return;
    // Bag changes aren't meaningful until the player is actually in the world.
    // During any loading screen (initial login, zone/instance transition) the
    // engine re-populates bag slots as objects stream in (setting g_pending)
    // while still behind the screen; the per-frame world tick runs there too,
    // so firing BAG_UPDATE_DELAYED would make addons rescan bags repeatedly
    // mid-load for no real change. Hold the pending flag — do NOT clear it —
    // until the engine's in-world flag is set; the first in-world frame then
    // emits a single DELAYED covering the settled inventory (matching retail's
    // one BAG_UPDATE_DELAYED per load). VAR_IN_WORLD clears on every transition,
    // so this re-suppresses on later zone changes too, not just initial login.
    if (*reinterpret_cast<const volatile uint8_t *>(
            static_cast<uintptr_t>(Offsets::VAR_IN_WORLD)) == 0)
        return;
    g_pending = false;
    const int slot = _reserve.Slot();
    if (slot >= 0)
        Event::Custom::Fire(slot, "");
    // Notify C++ subscribers of the bag-change frame (see UpdateDelayed.h).
    for (auto *node = g_subs; node != nullptr; node = node->next)
        node->cb();
}

static const Game::HookAutoRegister _hookSlotDiff{
    Offsets::FUN_BAG_SLOT_DIFF,
    reinterpret_cast<void *>(&BagSlotDiff_h),
    reinterpret_cast<void **>(&BagSlotDiff_o)};

static const Game::HookAutoRegister _hookItemToBag{
    Offsets::FUN_BAG_ITEM_TO_BAG,
    reinterpret_cast<void *>(&BagItemToBag_h),
    reinterpret_cast<void **>(&BagItemToBag_o)};

static const Game::HookAutoRegister _hookKeyringDesc{
    Offsets::FUN_BAG_KEYRING_DESC_CHANGE,
    reinterpret_cast<void *>(&KeyringDescChange_h),
    reinterpret_cast<void **>(&KeyringDescChange_o)};

static const Tick::WorldTick::AutoSubscribe _tickSub{&OnWorldTick};

} // namespace Bag::UpdateDelayed
