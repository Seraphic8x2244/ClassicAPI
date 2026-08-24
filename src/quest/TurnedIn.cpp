// This file is part of ClassicAPI.
//
// ClassicAPI is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// ClassicAPI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.

// `QUEST_TURNED_IN(questID, xpReward, moneyReward)` event — polyfills
// modern WoW's event of the same name. Fires when the server confirms
// a quest turn-in (SMSG_QUESTGIVER_QUEST_COMPLETE, opcode 0x191).
//
// Hook target is `FUN_005DC400` — the per-opcode handler for 0x191,
// reached through the multi-opcode dispatcher at `FUN_005E59B0`. We
// hook the inner handler rather than the dispatcher to keep the gate
// narrow; the dispatcher routes 9 different opcodes through one
// function and a wrong-opcode early-return would still pay the hook
// trampoline cost on every quest-status update.
//
// The payload comes from the packet body, which the engine handler
// reads via the packet stream's read cursor at `+0x14`. We save the
// cursor before calling the original, peek the first 4 uint32s
// (questID, unknown, xpReward, moneyReward), then restore the
// cursor so the original handler reads from the same position. This
// is the same save-and-restore pattern the engine doesn't use
// itself, but the stream cursor at `+0x14` is a plain uint32 with no
// side effects from being written, so the round-trip is safe.

#include "Game.h"
#include "Offsets.h"
#include "event/Custom.h"

#include <cstdint>

namespace Quest::TurnedIn {

namespace {

constexpr const char *kEventName = "QUEST_TURNED_IN";

const Event::Custom::AutoReserve _reserve{kEventName};

using PacketReadUint32_t = void(__thiscall *)(void *stream, uint32_t *out);

uint32_t PeekStreamCursor(void *stream) {
    return *reinterpret_cast<uint32_t *>(
        static_cast<uint8_t *>(stream) + Offsets::OFF_PACKET_STREAM_CURSOR);
}

void SetStreamCursor(void *stream, uint32_t cursor) {
    *reinterpret_cast<uint32_t *>(
        static_cast<uint8_t *>(stream) + Offsets::OFF_PACKET_STREAM_CURSOR) = cursor;
}

// `FUN_005DC400(player, packetStream)` — original handler. `__thiscall`
// with `this` = player object (resolved from `FUN_00468550` by the
// dispatcher); the packet stream is on the stack.
using Handler_t = int(__fastcall *)(void *thisPlayer, void *edx_unused, void *packetStream);
Handler_t Handler_o = nullptr;

int __fastcall Handler_h(void *thisPlayer, void *edx, void *packetStream) {
    auto readUint32 = reinterpret_cast<PacketReadUint32_t>(Offsets::FUN_PACKET_READ_UINT32);

    const uint32_t saved = PeekStreamCursor(packetStream);
    uint32_t questID = 0;
    uint32_t unknown = 0;
    uint32_t xpReward = 0;
    uint32_t moneyReward = 0;
    readUint32(packetStream, &questID);
    readUint32(packetStream, &unknown);
    readUint32(packetStream, &xpReward);
    readUint32(packetStream, &moneyReward);
    SetStreamCursor(packetStream, saved);

    const int result = Handler_o(thisPlayer, edx, packetStream);

    if (questID > 0) {
        Event::Custom::Fire(_reserve.Slot(), "%u%u%u",
                            questID, xpReward, moneyReward);
    }

    return result;
}

} // namespace

static const Game::HookAutoRegister _hook{
    Offsets::FUN_QUEST_GIVER_QUEST_COMPLETE_HANDLER,
    reinterpret_cast<void *>(&Handler_h),
    reinterpret_cast<void **>(&Handler_o)};

} // namespace Quest::TurnedIn
