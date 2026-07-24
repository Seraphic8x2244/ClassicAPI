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

// Replicates the engine's per-unit "watch descriptor fields → fire per-token
// unit events" registration (`FUN_0051bbb0` / `FUN_0051bdb0`) so synthetic
// tokens can be made first-class. The engine reaches this from
// `FUN_0060c520`, which recomputes a unit's token-category mask
// (target/party/raid/pet/mouseover) at `unit+0xc90` and, on a 0→nonzero
// transition, registers a bank-UNIT observer on every named event field with
// callback `FUN_0051bd50` → `FUN_00515e50(guid, eventId)` → the GUID→token
// reverse map `FUN_00515c50` → fire the event once per token.
//
// We register the same field set with a caller-supplied callback instead, so
// the caller fires its own token. The field set / sizes are read from the
// static event-name table exactly as the engine does; see the
// FUN_DESC_OBSERVER_REGISTER block in Offsets.h.
//
// Cross-checked against nampower's offsets.hpp: `SendUnitSignal` (the
// broadcast) = `0x00515e50`, `StringParamFormat` ("%s") = `0x0082E280`,
// `SignalEventParam` = `0x00703F50`, `UNIT_HEALTH` = 16 — all consistent with
// the field-index-is-event-id derivation here. (nampower left its
// `GetNamesFromGUID` reverse-map offset as an unfilled TODO; it is
// `FUN_00515c50`.)

#include "unit/TokenObserver.h"

#include "Offsets.h"

#include <cstdint>

namespace Unit::TokenObserver {

namespace {

using Register_t = void(__fastcall *)(int bank, uint32_t fieldOffset,
                                      uint32_t guidLo, uint32_t guidHi, int size,
                                      const void *callback, void *userArg1,
                                      void *userArg2);
using Unregister_t = void(__fastcall *)(int bank, uint32_t fieldOffset,
                                        uint32_t guidLo, uint32_t guidHi,
                                        const void *callback, void *userArg1);

const char *const *EventNames() {
    return reinterpret_cast<const char *const *>(
        static_cast<uintptr_t>(Offsets::VAR_EVENT_NAME_TABLE_STATIC));
}

// Bytes the engine watches for the observer on event index `i` — verbatim
// from `FUN_0051bbb0`'s size switch. The 8-byte entries are the 64-bit GUID
// fields; index 0x29 (`UNIT_FIELD_AURA`) spans the whole 0xD8 aura block.
int SizeForIndex(int i) {
    switch (i) {
    case 0:
    case 2:
    case 4:
    case 10:
        return 8;
    case 0x29:
        return 0xD8;
    case 0x6B:
        return 0x30;
    case 0xA7:
    case 0xAE:
        return 0x1C;
    default:
        return 4;
    }
}

} // namespace

void Register(uint64_t guid, FieldCallback cb) {
    if (guid == 0 || cb == nullptr)
        return;
    auto reg = reinterpret_cast<Register_t>(Offsets::FUN_DESC_OBSERVER_REGISTER);
    const char *const *names = EventNames();
    const uint32_t lo = static_cast<uint32_t>(guid);
    const uint32_t hi = static_cast<uint32_t>(guid >> 32);
    for (int i = 0; i < Offsets::EVENT_NAME_UNIT_MAX; ++i) {
        if (names[i] == nullptr)
            continue; // unnamed slot → not a real unit event
        reg(Offsets::DESC_OBSERVER_BANK_UNIT, static_cast<uint32_t>(i * 4), lo, hi,
            SizeForIndex(i), reinterpret_cast<const void *>(cb), nullptr, nullptr);
    }
}

void Unregister(uint64_t guid, FieldCallback cb) {
    if (guid == 0 || cb == nullptr)
        return;
    auto unreg =
        reinterpret_cast<Unregister_t>(Offsets::FUN_DESC_OBSERVER_UNREGISTER);
    const char *const *names = EventNames();
    const uint32_t lo = static_cast<uint32_t>(guid);
    const uint32_t hi = static_cast<uint32_t>(guid >> 32);
    for (int i = 0; i < Offsets::EVENT_NAME_UNIT_MAX; ++i) {
        if (names[i] == nullptr)
            continue;
        unreg(Offsets::DESC_OBSERVER_BANK_UNIT, static_cast<uint32_t>(i * 4), lo, hi,
              reinterpret_cast<const void *>(cb), nullptr);
    }
}

} // namespace Unit::TokenObserver
