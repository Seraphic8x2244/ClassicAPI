// This file is part of ClassicAPI.
//
// ClassicAPI is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// ClassicAPI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.

// `HEARTHSTONE_BOUND` event — polyfills modern WoW's event of the
// same name. Fires every time the player binds their hearthstone at
// an innkeeper, even when they re-bind at the SAME inn — matching
// modern semantics where the bind ACTION fires the event regardless
// of whether the location changes.
//
// Hooks the BINDPOINTUPDATE packet handler at `FUN_005ED3C0`. Two
// non-bind packets must be suppressed:
//
//   1. Initial post-login / character-switch sync — gated on
//      `VAR_BIND_POINT_VALID`. The engine zeroes that flag during
//      per-character entry init (`FUN_005E2510`); the handler sets it
//      to 1 after parsing. Flag 0 before the original runs ⇒ initial
//      sync, don't fire.
//   2. Map/zone-transition resync — Turtle's server re-sends this
//      packet on every map change with the (unchanged) bind location.
//      The valid flag is already 1 by then, so (1) doesn't catch it.
//      But those packets arrive behind a LOADING SCREEN, and the
//      player can't bind mid-load — so gate on `VAR_IN_WORLD` (the
//      engine's "world is live" flag, 0 during any loading screen).
//      A genuine rebind always arrives with the world live (flag 1),
//      so this suppresses the resync while still firing for a rebind
//      at the same innkeeper.
//
// Event has no payload — addons call `GetBindLocation()` to read
// the new location, matching modern semantics.

#include "Game.h"
#include "Offsets.h"
#include "event/Custom.h"

#include <cstdint>

namespace Player::HearthstoneBound {

namespace {

constexpr const char *kEventName = "HEARTHSTONE_BOUND";

const Event::Custom::AutoReserve _reserve{kEventName};

using BindPointUpdateHandler_t = void(__fastcall *)(void *packetBuffer);
BindPointUpdateHandler_t BindPointUpdateHandler_o = nullptr;

void __fastcall BindPointUpdateHandler_h(void *packetBuffer) {
    const uint32_t wasValid = *reinterpret_cast<const uint32_t *>(
        static_cast<uintptr_t>(Offsets::VAR_BIND_POINT_VALID));

    BindPointUpdateHandler_o(packetBuffer);

    if (wasValid == 0)
        return; // initial post-login / char-switch sync — not a user rebind

    // Resync packets ride the enter-world burst behind a loading screen;
    // a real innkeeper bind only happens with the world live. The player
    // can't bind mid-load, so a not-in-world packet is always a resync.
    if (*reinterpret_cast<const volatile uint8_t *>(
            static_cast<uintptr_t>(Offsets::VAR_IN_WORLD)) == 0)
        return;

    const int slot = Event::Custom::Lookup(kEventName);
    Event::Custom::Fire(slot, "");
}

} // namespace

static const Game::HookAutoRegister _hook{
    Offsets::FUN_BINDPOINT_UPDATE_HANDLER,
    reinterpret_cast<void *>(&BindPointUpdateHandler_h),
    reinterpret_cast<void **>(&BindPointUpdateHandler_o)};

} // namespace Player::HearthstoneBound
