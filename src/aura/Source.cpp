// This file is part of ClassicAPI.
//
// ClassicAPI is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// ClassicAPI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.

// See `Source.h` for the contract. This is the backend: a co-hook on
// `FUN_SPELL_GO` that mirrors nampower's `SpellGoHook` parse to capture the
// caster + duration of every aura-applying cast, and a small fixed-size
// cache `Aura::Data::Push` reads from.

#include "Source.h"

#include "ComboDuration.h"
#include "Data.h"
#include "Game.h"
#include "Offsets.h"
#include "net/PacketReader.h"
#include "player/StatSignal.h"
#include "spell/Lookup.h"
#include "tick/WorldTick.h"
#include "totem/Tracker.h"
#include "unit/Identity.h"

#include <cstdint>

namespace Aura::Source {

namespace {

using Net::CDataStore;

// A cast applies an aura iff any of its three effects names an aura. The
// EffectApplyAuraName[3] int32 array sits at `+0x16C` (already used by the
// shapeshift / mechanic-immunity code). Gating on this keeps pure-damage
// and utility casts out of the cache.
bool SpellAppliesAura(const uint8_t *spellRecord) {
    if (spellRecord == nullptr)
        return false;
    const auto *auraNames = reinterpret_cast<const int32_t *>(
        spellRecord + Offsets::OFF_SPELL_RECORD_EFFECT_APPLY_AURA_NAME);
    for (int i = 0; i < Offsets::SPELL_RECORD_EFFECT_COUNT; ++i) {
        if (auraNames[i] != 0)
            return true;
    }
    return false;
}

uint32_t NowMs() {
    using TickCount_t = uint32_t(__fastcall *)();
    return reinterpret_cast<TickCount_t>(
        static_cast<uintptr_t>(Offsets::FUN_OS_TICKCOUNT_MS))();
}

// Server-authoritative duration (ms). When the local player is the caster
// we let the engine fold in the player's duration modifiers (skipMod = 0);
// for any other caster we take the unmodified base (skipMod = 1) since we
// only have the local player's mod tables. `unit = 0` selects the player
// mod context per FUN_GET_SPELL_DURATION's signature.
uint32_t SpellDurationMs(const uint8_t *spellRecord, bool casterIsPlayer) {
    using GetDuration_t = int(__fastcall *)(const uint8_t *spellRecord,
                                            int unit, char skipMod);
    const int ms = reinterpret_cast<GetDuration_t>(
        static_cast<uintptr_t>(Offsets::FUN_GET_SPELL_DURATION))(
        spellRecord, 0, casterIsPlayer ? 0 : 1);
    return ms > 0 ? static_cast<uint32_t>(ms) : 0;
}

// ---- Recent local-player aura casts --------------------------------------

// The aura-application hooks (OnAuraAdded / OnAuraStacksChanged) get no caster,
// so they compute the UNMODIFIED base duration (`skipMod = 1`). That's correct
// for other units' auras, but wrong for one the LOCAL PLAYER just cast: it
// should carry the player-talented duration (Improved Shadow Word: Pain → 24s
// vs the 18s base). SpellGo normally owns that entry with the talented value,
// but when it can't attribute the cast to this target — an empty SMSG_SPELL_GO
// hit list files it under the caster instead, or a slot reshuffle evicts and
// refills the entry after SpellGo ran — the base value is what's left, so the
// duration flickers 24 → 18. SpellGo (which knows caster == player) records the
// spellId here so the application hooks can recover the player-modified
// duration for it.
constexpr int kRecentCastCount = 16;
constexpr uint32_t kRecentCastTtlMs = 1500;
struct RecentCast {
    uint32_t spellId;
    uint32_t tMs; // 0 = empty
};
RecentCast g_recentCasts[kRecentCastCount];
int g_recentCastCursor = 0;

void RememberPlayerCast(uint32_t spellId) {
    const uint32_t now = NowMs();
    for (auto &r : g_recentCasts)
        if (r.spellId == spellId) {
            r.tMs = now;
            return;
        }
    g_recentCasts[g_recentCastCursor] = {spellId, now};
    g_recentCastCursor = (g_recentCastCursor + 1) % kRecentCastCount;
}

bool WasRecentPlayerCast(uint32_t spellId) {
    if (spellId == 0)
        return false;
    const uint32_t now = NowMs();
    for (const auto &r : g_recentCasts)
        if (r.spellId == spellId && r.tMs != 0 && now - r.tMs <= kRecentCastTtlMs)
            return true;
    return false;
}

// ---- Cache ---------------------------------------------------------------

// Helpful/harmful classification, recorded from the aura's descriptor slot
// when it's applied (the only place the buff/debuff split is known). SpellGo
// gives no slot, so casts we only saw there stay Unknown until/unless an
// application hook fires for the same aura.
enum Kind : int8_t { KIND_UNKNOWN = -1, KIND_HELPFUL = 0, KIND_HARMFUL = 1 };

struct Entry {
    uint64_t targetGuid;
    uint64_t casterGuid;
    uint32_t spellId;
    uint32_t expirationMs; // 0 = infinite / unknown duration
    uint32_t durationMs;   // applied duration (incl. caster mods); 0 = none
    int8_t kind;           // Kind; descriptor-slot-derived, KIND_UNKNOWN if only seen via SpellGo
    bool used;
};

constexpr int kCacheSize = 256;
Entry g_cache[kCacheSize];
int g_writeCursor = 0;

// `fromCast` true: the SpellGo hook — authoritative caster + caster-modified
// (talented) timing. False: the OnAuraAdded application hook — timing only,
// no caster, and it must not clobber an entry SpellGo already owns (that
// would replace talented timing with the unmodified base), so it skips
// entries that already carry a caster.
void Store(uint64_t targetGuid, uint32_t spellId, uint64_t casterGuid,
           uint32_t expirationMs, uint32_t durationMs, bool fromCast,
           int8_t kind) {
    if (targetGuid == 0 || spellId == 0)
        return;

    // Update an existing entry for this exact aura instance.
    for (auto &e : g_cache) {
        if (e.used && e.targetGuid == targetGuid && e.spellId == spellId) {
            // Learn the classification whenever a slot-derived kind arrives —
            // independent of caster/timing ownership, and never downgrade a
            // known kind back to unknown.
            if (kind != KIND_UNKNOWN)
                e.kind = kind;
            if (!fromCast && e.casterGuid != 0)
                return; // SpellGo owns this entry; keep its caster + timing
            if (casterGuid != 0)
                e.casterGuid = casterGuid;
            e.expirationMs = expirationMs;
            e.durationMs = durationMs;
            return;
        }
    }
    // Take a free slot, else an expired one, else evict round-robin.
    const uint32_t now = NowMs();
    for (auto &e : g_cache) {
        if (!e.used || (e.expirationMs != 0 && now >= e.expirationMs)) {
            e = {targetGuid, casterGuid, spellId, expirationMs, durationMs,
                 kind, true};
            return;
        }
    }
    Entry &slot = g_cache[g_writeCursor];
    g_writeCursor = (g_writeCursor + 1) % kCacheSize;
    slot = {targetGuid, casterGuid, spellId, expirationMs, durationMs, kind,
            true};
}

// Evict the entry for an aura the engine reports gone. Keyed by (target,
// spell) like the rest of the cache. Without this, the GetAuraDataByIndex
// fallback would keep surfacing a dropped aura until its computed expiry —
// e.g. a Rank 1 buff replaced by Rank 2 (engine drops Rank 1 from the
// descriptor) would show as a phantom second aura, or a dispelled buff would
// linger.
void Evict(uint64_t targetGuid, uint32_t spellId) {
    if (targetGuid == 0 || spellId == 0)
        return;
    for (auto &e : g_cache) {
        if (e.used && e.targetGuid == targetGuid && e.spellId == spellId) {
            e.used = false;
            return;
        }
    }
}

// Wipe the whole cache. Used on a map transition (see OnWorldTick).
void FlushAll() {
    for (auto &e : g_cache)
        e.used = false;
}

// -1 = no map seen yet (never a valid Map.dbc id), so the first tick just
// records the current map without flushing.
int g_lastMapId = -1;

// Drop entries whose timed aura has elapsed so the table doesn't fill with
// stale combat debuffs. Infinite-duration entries (expirationMs == 0) stay
// until overwritten.
//
// Also flush the entire cache on a map-ID change. A battleground/instance/
// continent transition (including the teleport out of a BG via /afk) bulk-
// clears the player's aura descriptor WITHOUT firing per-aura OnAuraRemoved,
// so entries for auras that are now gone would linger and the
// GetAuraDataByIndex fallback would keep surfacing them — the "buffs/debuffs
// stuck after leaving a battleground" report. Every other unit from the old
// map has despawned too, and the local player's real auras re-sync into the
// descriptor / PLAYER_BUFF tables on entering the new world, so a full flush
// loses nothing legitimate. Crucially, the descriptor drops the fallback
// must survive — rogue stealth and party-range fluctuation — do NOT change
// the map ID, so they're unaffected.
void OnWorldTick() {
    const int mapId = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_CURRENT_MAP_ID));
    if (mapId != g_lastMapId) {
        if (g_lastMapId != -1)
            FlushAll();
        g_lastMapId = mapId;
    }

    const uint32_t now = NowMs();
    for (auto &e : g_cache) {
        if (e.used && e.expirationMs != 0 && now >= e.expirationMs)
            e.used = false;
    }
}

const Tick::WorldTick::AutoSubscribe _tickSub{&OnWorldTick};

// ---- SMSG_SPELL_GO co-hook ----------------------------------------------

// The hit-target list is all we need: each entry is a unit the cast landed
// on, which is exactly where an aura gets applied. We deliberately stop
// before the missed-target list / target mask — the post-mask "intended"
// target is redundant with the hit list for aura purposes, and skipping it
// keeps the parse short and robust.
constexpr int kMaxTargets = 16;

using SpellGo_t = void(__fastcall *)(uint64_t *itemGUID, uint64_t *casterGUID,
                                     uint32_t spellId, CDataStore *packet);
SpellGo_t g_origSpellGo = nullptr;

void __fastcall SpellGo_h(uint64_t *itemGUID, uint64_t *casterGUID,
                          uint32_t spellId, CDataStore *packet) {
    uint64_t targets[kMaxTargets];
    int numTargets = 0;

    if (packet != nullptr) {
        const uint32_t savedRead = packet->m_read;
        Net::Read<int16_t>(packet); // castFlags (unused)
        const uint8_t numHit = Net::Read<uint8_t>(packet);
        for (int i = 0; i < numHit; ++i) {
            const uint64_t guid = Net::Read<uint64_t>(packet);
            if (numTargets < kMaxTargets)
                targets[numTargets++] = guid;
        }
        packet->m_read = savedRead; // restore so the engine re-parses cleanly
    }

    g_origSpellGo(itemGUID, casterGUID, spellId, packet);

    const uint64_t caster = (casterGUID != nullptr) ? *casterGUID : 0;
    if (caster == 0 || spellId == 0)
        return;

    // Feed the totem tracker BEFORE the aura gate below — a totem summon
    // applies no aura, so it would otherwise be dropped. Player casts only.
    if (caster == Unit::Identity::PlayerGuid())
        Totem::Tracker::OnPlayerSpellGo(spellId);

    const uint8_t *rec = Spell::Lookup::RecordForID(static_cast<int>(spellId));
    if (!SpellAppliesAura(rec))
        return;

    const bool casterIsPlayer = (caster == Unit::Identity::PlayerGuid());
    // Combo-point finishers (Rupture, Kidney Shot, …) have their real
    // duration computed at cast time from the combo points spent — the
    // base-duration helper can't know it. ComboDuration mirrors the
    // server's computation from the CP snapshot its send hook captured;
    // 0 means "not combo-scaled", fall through to the base path.
    uint32_t durationMs =
        casterIsPlayer ? ComboDuration::TryComboScaledMs(rec, spellId) : 0;
    if (durationMs == 0)
        durationMs = SpellDurationMs(rec, casterIsPlayer);
    const uint32_t expirationMs = durationMs > 0 ? NowMs() + durationMs : 0;

    // Let the application hooks recover the player-talented duration for this
    // spell if they end up owning the target's entry (empty hit list / refill
    // race) — see WasRecentPlayerCast.
    if (casterIsPlayer)
        RememberPlayerCast(spellId);

    if (numTargets == 0) {
        // No explicit hit list (self-cast with caster-implicit target).
        Store(caster, spellId, caster, expirationMs, durationMs, true,
              KIND_UNKNOWN);
        return;
    }
    for (int i = 0; i < numTargets; ++i)
        Store(targets[i], spellId, caster, expirationMs, durationMs, true,
              KIND_UNKNOWN);
}

const Game::HookAutoRegister _hook{Offsets::FUN_SPELL_GO,
                                   reinterpret_cast<void *>(&SpellGo_h),
                                   reinterpret_cast<void **>(&g_origSpellGo)};

// ---- Aura-application co-hooks (timing for proc / triggered auras) -------

// Stamp expiration for an aura that just landed/refreshed on `unit`. Used by
// both the add and stack-change hooks. No caster is available from these
// paths, so it stamps timing only with `fromCast=false` — Store skips any
// entry SpellGo already owns, so a directly-cast aura keeps its talented
// timing. Base (unmodified) duration is the best estimate without a caster.
void StampApplication(void *unit, uint32_t spellId, int8_t kind) {
    if (spellId == 0)
        return;
    const uint8_t *rec = Spell::Lookup::RecordForID(static_cast<int>(spellId));
    if (!SpellAppliesAura(rec))
        return;
    const uint64_t unitGuid = Unit::Identity::GuidForObject(unit);
    if (unitGuid == 0)
        return;
    // If the local player just cast this aura, use its player-talented duration
    // (and attribute the caster) — SpellGo couldn't always land the talented
    // value on this target's entry. Otherwise base (we lack other casters'
    // mods). See WasRecentPlayerCast.
    const bool byPlayer = WasRecentPlayerCast(spellId);
    const uint32_t durationMs = SpellDurationMs(rec, byPlayer);
    const uint64_t caster = byPlayer ? Unit::Identity::PlayerGuid() : 0;
    const uint32_t expirationMs = durationMs > 0 ? NowMs() + durationMs : 0;
    Store(unitGuid, spellId, caster, expirationMs, durationMs,
          /*fromCast*/ false, kind);
}

// Classify by the absolute aura slot: 0..BUFF_COUNT-1 = buff (helpful),
// BUFF_COUNT..TOTAL-1 = debuff (harmful).
int8_t KindForSlot(int slot) {
    return slot >= Offsets::UNIT_AURA_BUFF_COUNT ? KIND_HARMFUL : KIND_HELPFUL;
}

// Bump the player-stat-inputs signal when an aura change hits the LOCAL
// player — buffs/debuffs move GetSpellBonusHealing's flat and Spirit/Armor
// terms, so its lazy cache must invalidate. Guarded on the player object so
// other units' aura churn (combat) doesn't needlessly invalidate it.
void NotifyIfPlayer(void *unit) {
    if (static_cast<const uint8_t *>(unit) == Unit::Identity::PlayerObject())
        Player::StatSignal::Notify();
}

// OnAuraAdded — a new aura occupies a slot (gives the spellId directly).
using OnAuraAdded_t = void(__fastcall *)(void *unit, void *edx, uint32_t slot,
                                         uint32_t spellId);
OnAuraAdded_t g_origOnAuraAdded = nullptr;

void __fastcall OnAuraAdded_h(void *unit, void *edx, uint32_t slot,
                              uint32_t spellId) {
    g_origOnAuraAdded(unit, edx, slot, spellId);
    StampApplication(unit, spellId, KindForSlot(static_cast<int>(slot)));
    NotifyIfPlayer(unit);
}

const Game::HookAutoRegister _hookAuraAdded{
    Offsets::FUN_ON_AURA_ADDED, reinterpret_cast<void *>(&OnAuraAdded_h),
    reinterpret_cast<void **>(&g_origOnAuraAdded)};

// OnAuraStacksChanged — an existing aura's stack count changed (e.g. Shadow
// Weaving climbing). Only the slot is given, so read the spellID back from
// the unit's aura array. Re-stamps expiration so stacking refreshes count.
using OnAuraStacksChanged_t = void(__fastcall *)(void *unit, void *edx,
                                                 int slot, uint8_t stackCount);
OnAuraStacksChanged_t g_origOnAuraStacksChanged = nullptr;

void __fastcall OnAuraStacksChanged_h(void *unit, void *edx, int slot,
                                      uint8_t stackCount) {
    g_origOnAuraStacksChanged(unit, edx, slot, stackCount);
    StampApplication(
        unit,
        Aura::Data::ReadSpellID(static_cast<const uint8_t *>(unit), slot),
        KindForSlot(slot));
    NotifyIfPlayer(unit);
}

const Game::HookAutoRegister _hookAuraStacks{
    Offsets::FUN_ON_AURA_STACKS_CHANGED,
    reinterpret_cast<void *>(&OnAuraStacksChanged_h),
    reinterpret_cast<void **>(&g_origOnAuraStacksChanged)};

// OnAuraRemoved — a descriptor aura slot went empty: the aura fell off, was
// dispelled, was cancelled by its owner, or was replaced by a higher rank
// (the diff dispatcher fires Removed(old) + Added(new)). Evict the cache
// entry so the descriptor-drop fallback stops surfacing it. Same ABI as
// OnAuraAdded (unit in ecx, slot + spellId on the stack); `slot` is unused
// here — we evict by spellId.
//
// Always evict, even for a death-persistent aura (flask, world buff) on a
// dead unit: the server keeps those through death WITHOUT changing any
// field, so death itself fires no OnAuraRemoved. Any removal we do receive
// for one is therefore genuine — e.g. the owner cancelled the flask while
// dead — and must clear the cache, or the fallback keeps surfacing a phantom
// whose SetUnitAura tooltip is empty.
using OnAuraRemoved_t = void(__fastcall *)(void *unit, void *edx, uint32_t slot,
                                           uint32_t spellId);
OnAuraRemoved_t g_origOnAuraRemoved = nullptr;

void __fastcall OnAuraRemoved_h(void *unit, void *edx, uint32_t slot,
                                uint32_t spellId) {
    g_origOnAuraRemoved(unit, edx, slot, spellId);
    (void)slot;
    Evict(Unit::Identity::GuidForObject(unit), spellId);
    NotifyIfPlayer(unit);
}

const Game::HookAutoRegister _hookAuraRemoved{
    Offsets::FUN_ON_AURA_REMOVED, reinterpret_cast<void *>(&OnAuraRemoved_h),
    reinterpret_cast<void **>(&g_origOnAuraRemoved)};

} // namespace

bool Get(uint64_t unitGuid, uint32_t spellId, uint64_t *outCaster,
         uint32_t *outExpirationMs, uint32_t *outDurationMs) {
    if (unitGuid == 0 || spellId == 0)
        return false;
    for (const auto &e : g_cache) {
        if (e.used && e.targetGuid == unitGuid && e.spellId == spellId) {
            *outCaster = e.casterGuid;
            *outExpirationMs = e.expirationMs;
            *outDurationMs = e.durationMs;
            return true;
        }
    }
    return false;
}

void EvictAbsent(uint64_t unitGuid, const uint32_t *presentSpellIds, int count) {
    if (unitGuid == 0 || presentSpellIds == nullptr || count <= 0)
        return;
    for (auto &e : g_cache) {
        if (!e.used || e.targetGuid != unitGuid)
            continue;
        bool present = false;
        for (int i = 0; i < count; ++i)
            if (presentSpellIds[i] == e.spellId) {
                present = true;
                break;
            }
        if (!present)
            e.used = false;
    }
}

int Enumerate(uint64_t unitGuid, bool harmful, CachedAura *out, int maxOut) {
    if (unitGuid == 0 || out == nullptr || maxOut <= 0)
        return 0;
    const int8_t want = harmful ? KIND_HARMFUL : KIND_HELPFUL;
    const uint32_t now = NowMs();
    int n = 0;
    for (const auto &e : g_cache) {
        if (n >= maxOut)
            break;
        if (!e.used || e.targetGuid != unitGuid || e.kind != want)
            continue;
        if (e.expirationMs != 0 && now >= e.expirationMs)
            continue; // expired (infinite-duration entries pass)
        out[n++] = {e.spellId, e.casterGuid, e.expirationMs, e.durationMs};
    }
    return n;
}

} // namespace Aura::Source
