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

// Quest log delta events — polyfills two modern WoW events from one
// pre-/post-rebuild diff:
//
// `QUEST_ACCEPTED(questLogIndex, questID)` — fires once per new quest
// entering the local quest log. Matches the Cata/WotLK signature,
// 1-based log index (event added in 3.1.0).
//
// `QUEST_REMOVED(questID)` — fires once per quest leaving the local
// quest log, for both turn-ins and abandons (event added in 8.0.1;
// Classic-era signature is the bare questID). For turn-ins this fires
// *after* `QUEST_TURNED_IN`, matching retail ordering: the 0x191
// complete packet doesn't touch the log; the removal arrives in the
// follow-up quest-log update packets, whose rebuild we diff here.
// Observed turn-in sequence: QUEST_TURNED_IN → UNIT_QUEST_LOG_CHANGED
// → QUEST_LOG_UPDATE (fired inside the rebuild) → QUEST_REMOVED.
//
// Hook target: `FUN_QUEST_LOG_REBUILD` (`0x004DE510`) — the single
// chokepoint the engine uses to refresh the Lua-visible quest log
// from the player's authoritative slot data at `[CGPlayer + 0xE68 +
// 0x28]`. Every state change that adds, removes, or updates a quest
// flows through here. By snapshotting the log before and after the
// original runs, we can compute the delta and fire only for genuine
// additions / removals.
//
// Login suppression: the same function handles the initial bulk-sync
// at character entry, which appears as a single rebuild call adding
// N quests at once. We suppress fires when more than one quest is
// added in a single rebuild (which can only happen on login / reload
// resync — human input speed can't accept multiple quests in the
// same engine tick). A brand-new character's first accept is
// `0 → 1` and fires correctly. Removals apply the same rule (a user
// action removes at most one quest per rebuild) and are additionally
// suppressed when the rebuild also added multiple quests — a
// mixed-delta rebuild is a cross-character resync, not gameplay.

#include "Game.h"
#include "Offsets.h"
#include "event/Custom.h"

#include <cstdint>
#include <unordered_set>

namespace Quest::LogEvents {

namespace {

constexpr const char *kAcceptedEventName = "QUEST_ACCEPTED";
constexpr const char *kRemovedEventName = "QUEST_REMOVED";

const Event::Custom::AutoReserve _reserveAccepted{kAcceptedEventName};
const Event::Custom::AutoReserve _reserveRemoved{kRemovedEventName};

// Walk the live quest log and collect questIDs of real (non-header)
// entries. Headers are 16-byte rows with a non-NULL pointer at +0x8;
// real quests have NULL there and the questID at +0x0.
void SnapshotQuestIDs(std::unordered_set<int> &out) {
    out.clear();
    const int count = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_QUEST_LOG_ENTRY_COUNT));
    if (count <= 0)
        return;
    const auto *entries = reinterpret_cast<const uint8_t *>(
        static_cast<uintptr_t>(Offsets::VAR_QUEST_LOG_ENTRIES));
    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const uint8_t *entry = entries + i * Offsets::OFF_QUEST_LOG_ENTRY_STRIDE;
        const void *hdr = *reinterpret_cast<const void *const *>(
            entry + Offsets::OFF_QUEST_LOG_ENTRY_HEADER_PTR);
        if (hdr != nullptr)
            continue;
        const int questID = *reinterpret_cast<const int *>(
            entry + Offsets::OFF_QUEST_LOG_ENTRY_QUEST_ID);
        if (questID > 0)
            out.insert(questID);
    }
}

} // namespace

using QuestLogRebuild_t = void(__fastcall *)(int param_1);
QuestLogRebuild_t QuestLogRebuild_o = nullptr;

void __fastcall QuestLogRebuild_h(int param_1) {
    // `param_1 == 0` is the engine's "no-op rebuild" signal — just
    // refires QUEST_LOG_UPDATE without touching the entry array.
    // No diff possible, no accept/remove happening, pass through.
    if (param_1 == 0) {
        QuestLogRebuild_o(param_1);
        return;
    }

    std::unordered_set<int> pre;
    SnapshotQuestIDs(pre);

    QuestLogRebuild_o(param_1);

    // Walk the post-state in index order so the fired event's
    // `questLogIndex` matches the 1-based slot the engine just
    // assigned. Collect the full post-set on the way for the
    // removal diff.
    const int count = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_QUEST_LOG_ENTRY_COUNT));
    const auto *entries = reinterpret_cast<const uint8_t *>(
        static_cast<uintptr_t>(Offsets::VAR_QUEST_LOG_ENTRIES));

    std::unordered_set<int> post;
    if (count > 0)
        post.reserve(static_cast<size_t>(count));

    int newCount = 0;
    int firstNewIdx = 0;
    int firstNewID = 0;
    for (int i = 0; i < count; ++i) {
        const uint8_t *entry = entries + i * Offsets::OFF_QUEST_LOG_ENTRY_STRIDE;
        const void *hdr = *reinterpret_cast<const void *const *>(
            entry + Offsets::OFF_QUEST_LOG_ENTRY_HEADER_PTR);
        if (hdr != nullptr)
            continue;
        const int questID = *reinterpret_cast<const int *>(
            entry + Offsets::OFF_QUEST_LOG_ENTRY_QUEST_ID);
        if (questID <= 0)
            continue;
        post.insert(questID);
        if (pre.count(questID) != 0)
            continue;
        ++newCount;
        if (newCount == 1) {
            firstNewIdx = i + 1;
            firstNewID = questID;
        }
    }

    int removedCount = 0;
    int firstRemovedID = 0;
    for (const int questID : pre) {
        if (post.count(questID) != 0)
            continue;
        ++removedCount;
        if (removedCount == 1)
            firstRemovedID = questID;
    }

    // Single addition → user-driven accept. Multiple → bulk resync
    // (login or post-reload). Modern WoW's QUEST_ACCEPTED doesn't
    // fire on initial sync either; we match that.
    if (newCount == 1) {
        const int slot = _reserveAccepted.Slot();
        if (slot >= 0)
            Event::Custom::Fire(slot, "%d%d", firstNewIdx, firstNewID);
    }

    // Single removal → user-driven turn-in or abandon. The extra
    // `newCount <= 1` gate rejects the one resync shape the removal
    // count alone can't: switching to a character whose log shares
    // all but one of the previous character's quests (1 removal, N
    // additions in the same rebuild).
    if (removedCount == 1 && newCount <= 1) {
        const int slot = _reserveRemoved.Slot();
        if (slot >= 0)
            Event::Custom::Fire(slot, "%d", firstRemovedID);
    }
}

static const Game::HookAutoRegister _hookreg{
    Offsets::FUN_QUEST_LOG_REBUILD,
    reinterpret_cast<void *>(&QuestLogRebuild_h),
    reinterpret_cast<void **>(&QuestLogRebuild_o)};

} // namespace Quest::LogEvents
