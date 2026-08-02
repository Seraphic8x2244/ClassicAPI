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

// `markN` raid-target-marker unit tokens (`mark1`=star … `mark8`=skull).
//
// Two halves, mirroring `Unit::Focus`:
//
//   1. Resolution — `GetGUIDByMark` reads the engine's marker table
//      (`VAR_RAID_TARGET_GUIDS`); the `markN` branch of the token
//      resolver in `unit/TokenExtensions.cpp` calls it so every stock
//      `Script_Unit*` (UnitHealth, UnitExists, UnitName, …) accepts
//      `"markN"` for free.
//
//   2. Events — `Unit::TokenObserver` watches each marked unit's
//      descriptor fields so `UNIT_HEALTH` / `UNIT_MANA` / `UNIT_AURA` /
//      … fire with `arg1 == "markN"`, exactly like a native token. The
//      engine only auto-watches its own tokens (target/party/raid/pet/
//      mouseover), so a marked mob that is none of those fires no unit
//      events until we observe it.
//
// Why the WorldTick poll (vs. a packet hook): the marker table is written
// by the server (`SMSG_RAID_TARGET_UPDATE`), and — more importantly — a
// `TokenObserver` can only attach to a LIVE `CGUnit`. A unit marked while
// out of range has no object to observe; when it enters range there is no
// per-mark event announcing it, and when it leaves range its observer
// nodes die with the object silently. So each tick we diff the 8 slots
// and (re)attach/detach observers against the current live objects — the
// same range-transition handling `Unit::Focus::OnWorldTick` does, ×8. The
// idle case (no markers set) costs 8 compares and zero object lookups.
//
// A marker on a non-unit GUID (totem corpse / gameobject / loot) simply
// never resolves to a `CGUnit`, so it is never observed and fires no
// events — correct, and matches `UnitExists("markN")` being false there.

#include "unit/RaidTarget.h"

#include "Game.h"
#include "Offsets.h"
#include "tick/WorldTick.h"
#include "unit/TokenObserver.h"

#include <cstdint>

namespace Unit::RaidTarget {

namespace {

// `"mark1".."mark8"` — the token strings fired as `arg1`. Static so the
// pointers stay valid for the `FUN_FIRE_EVENT` "%s" argument.
const char *const kMarkTokens[kMarkCount] = {
    "mark1", "mark2", "mark3", "mark4",
    "mark5", "mark6", "mark7", "mark8",
};

// GUID we are currently tracking for each marker slot (mirrors the live
// marker table), or 0. Doubles as the callback's stale-fire guard.
uint64_t g_markGUID[kMarkCount] = {};

// Whether a live descriptor observer is currently registered on
// `g_markGUID[i]`. Cleared when that unit leaves the object table so the
// tick re-registers when it returns (nodes die with the object).
bool g_markObserved[kMarkCount] = {};

using ResolveByGUID_t = void *(__fastcall *)(int type, const char *debugName,
                                             uint32_t guidLo, uint32_t guidHi,
                                             int priority);

// Resolve a GUID to its live `CGUnit`, or null when it isn't currently in
// the client's object table (out of range / despawned / a non-unit
// marker). Non-throwing — safe from the tick. Mirrors `Focus::ResolveObject`.
const uint8_t *ResolveObject(uint64_t guid) {
    if (guid == 0)
        return nullptr;
    auto resolve = reinterpret_cast<ResolveByGUID_t>(
        static_cast<uintptr_t>(Offsets::FUN_OBJECT_RESOLVE_BY_GUID));
    return static_cast<const uint8_t *>(
        resolve(Offsets::OBJ_TYPE_UNIT, "RaidTarget", static_cast<uint32_t>(guid),
                static_cast<uint32_t>(guid >> 32), 0x172));
}

// Descriptor-field observer callback for marker slot `Idx` (0-based).
// Fires the changed field's unit event (`fieldOffset >> 2`) with the
// `"markN"` token, so `arg1 == "markN"` works for `UNIT_HEALTH`,
// `UNIT_MANA`, `UNIT_AURA`, … — exactly like `FocusFieldCb`. Guarded on
// the live tracked GUID so an in-flight field change racing an unregister
// can't fire a stale token.
template <int Idx>
int __fastcall MarkFieldCb(uint32_t fieldOffset, uint32_t /*size*/,
                           uint32_t guidLo, uint32_t guidHi,
                           const uint32_t * /*oldValue*/, void * /*userArg*/) {
    const uint64_t g = g_markGUID[Idx];
    if (guidLo != static_cast<uint32_t>(g) || guidHi != static_cast<uint32_t>(g >> 32))
        return 1;
    using Fire_t = void(__cdecl *)(int eventId, const char *fmt, ...);
    reinterpret_cast<Fire_t>(static_cast<uintptr_t>(Offsets::FUN_FIRE_EVENT))(
        static_cast<int>(fieldOffset >> 2), "%s", kMarkTokens[Idx]);
    return 1;
}

// One distinct callback per slot — `TokenObserver` keys observer nodes on
// the callback pointer, and each fires its own fixed token, so the slots
// must not share a function.
const TokenObserver::FieldCallback g_markCb[kMarkCount] = {
    &MarkFieldCb<0>, &MarkFieldCb<1>, &MarkFieldCb<2>, &MarkFieldCb<3>,
    &MarkFieldCb<4>, &MarkFieldCb<5>, &MarkFieldCb<6>, &MarkFieldCb<7>,
};

const uint64_t *MarkerTable() {
    return reinterpret_cast<const uint64_t *>(
        static_cast<uintptr_t>(Offsets::VAR_RAID_TARGET_GUIDS));
}

// Keep each slot's observer pointed at the unit currently wearing that
// marker. Per slot: adopt a changed/cleared assignment (dropping the old
// observer), then (re)register on the live object or note a despawn. The
// heavy path (ResolveObject) runs only for slots that actually hold a
// marker, so an unmarked table is 8 cheap compares.
void OnWorldTick() {
    const uint64_t *table = MarkerTable();
    for (int i = 0; i < kMarkCount; ++i) {
        const uint64_t want = table[i];
        if (want != g_markGUID[i]) {
            // Marker moved to a different GUID (or was cleared): stop
            // watching the old unit, adopt the new assignment.
            if (g_markObserved[i]) {
                TokenObserver::Unregister(g_markGUID[i], g_markCb[i]);
                g_markObserved[i] = false;
            }
            g_markGUID[i] = want;
        }
        if (g_markGUID[i] != 0) {
            const bool live = ResolveObject(g_markGUID[i]) != nullptr;
            if (live && !g_markObserved[i]) {
                TokenObserver::Register(g_markGUID[i], g_markCb[i]);
                g_markObserved[i] = true;
            } else if (!live && g_markObserved[i]) {
                // Marked unit left the object table (out of range /
                // despawn); its observer nodes died with it. Re-register
                // when it returns.
                g_markObserved[i] = false;
            }
        }
    }
}

const Tick::WorldTick::AutoSubscribe _tickSub{&OnWorldTick};

// Reset tracking on logout→character-select. The DLL isn't reloaded
// across a relog, so stale per-slot GUIDs from character A must not leak
// into B. Observer nodes already died with the world teardown, and the
// tick re-reads the (fresh) marker table anyway, so we only clear our
// mirror. Same rationale as `Focus::ClearOnLogout`; glue hook so it fires
// on logout but NOT on /reload.
void ClearOnLogout() {
    for (int i = 0; i < kMarkCount; ++i) {
        g_markGUID[i] = 0;
        g_markObserved[i] = false;
    }
}
const Game::GlueModuleAutoRegister _clearOnLogout{&ClearOnLogout};

} // namespace

uint64_t GetGUIDByMark(int mark) {
    if (mark < 1 || mark > kMarkCount)
        return 0;
    return MarkerTable()[mark - 1];
}

} // namespace Unit::RaidTarget
