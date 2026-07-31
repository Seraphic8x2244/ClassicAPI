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

// `Event::EnteringWorld` owns the single interception of PLAYER_ENTERING_WORLD
// (see EnteringWorld.cpp). It also exposes the one durable fact other modules
// need from that signal: whether the player has finished at least one
// enter-world transition this session — i.e. the initial-login loading screen
// is done and the world is live.
//
// This is the reliable "in world" gate. There is NO clean pollable in-world
// boolean in the 1.12 binary: 0x00B4E378 is the current map ID (map 0 is
// valid) and 0x00B4E37C is instance grace-period state written from a packet
// handler, not a per-frame flag. PLAYER_ENTERING_WORLD, which fires once the
// loading screen completes and the world is ready, is the correct edge.

namespace Event::EnteringWorld {

// True once PLAYER_ENTERING_WORLD has fired at least once this session (initial
// login, /reload, or a zone/instance transition — whichever comes first).
// One-way latch: false only during the initial-login loading screen, before the
// world is live. Process-global, so it stays true across /reload.
bool HasEnteredWorld();

} // namespace Event::EnteringWorld
