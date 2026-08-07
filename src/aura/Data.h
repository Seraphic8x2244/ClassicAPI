// This file is part of ClassicAPI.
//
// ClassicAPI is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// ClassicAPI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.

#pragma once

#include <cstdint>

// Shared aura-table primitives consumed by `C_UnitAuras.*` (and any
// future module that needs to inspect a unit's aura array). All
// reads go through the unit's `m_objectFields` descriptor at
// `unit + OFF_CGUNIT_OBJECT_FIELDS`; the layout is documented in
// `Offsets.h` under `OFF_UNIT_FIELD_AURA*`.
//
// Slot indexing convention used here matches the engine's:
//
//   slot 0..31  → buffs   (helpful)
//   slot 32..47 → debuffs (harmful)
//
// Callers that take a 1-based Lua index into "buffs" or "debuffs"
// translate to the absolute 0..47 slot before calling in.

namespace Aura::Data {

// Filter for slot iteration. The engine's `UnitBuff` / `UnitDebuff`
// pick one direction; modern `C_UnitAuras.GetAuraDataByIndex` defaults
// to helpful when no filter is specified.
enum class Filter { Helpful, Harmful };

// Caster restriction from the `PLAYER` / `!PLAYER` aura filter tokens.
// `Any` = no restriction; `PlayerOnly` = only auras the local player cast
// (`PLAYER`); `NotPlayer` = only auras NOT cast by the local player
// (`!PLAYER`). Caster attribution comes from the `Aura::Source` cache; a
// cache miss counts as "not the player" (so `PlayerOnly` excludes it and
// `NotPlayer` includes it — matching `IsPlayerCast`'s miss semantics).
enum class CasterMode { Any, PlayerOnly, NotPlayer };

// Reads the spell ID at the given absolute slot (0..47). Returns 0
// if the slot is empty or the unit pointer is null.
uint32_t ReadSpellID(const uint8_t *unit, int slot);

// True iff the slot is occupied AND the engine considers the aura
// applied (descriptor flag nibble & UNIT_AURA_VISIBLE_MASK != 0)
// AND the spell record passes the engine's visibility predicate.
// This is the same gate `Script_UnitBuff`/`Script_UnitDebuff` use
// to decide whether to surface an aura through Lua.
bool IsSlotPopulated(const uint8_t *unit, int slot);

// Finds the absolute slot of the `oneBasedIndex`-th populated aura
// matching `filter`. Returns -1 if no such aura. Walks
// 0..UNIT_AURA_BUFF_COUNT-1 for Helpful, UNIT_AURA_BUFF_COUNT..
// UNIT_AURA_TOTAL-1 for Harmful. When `playerOnly` is true, only auras
// the local player cast (per `IsPlayerCast`) are counted/returned.
int FindNthSlot(const uint8_t *unit, int oneBasedIndex, Filter filter,
                CasterMode caster = CasterMode::Any);

// Finds the absolute slot of a populated aura with the given spell
// ID, optionally restricted to one filter range. `filter` of
// nullptr searches both ranges (helpful first, then harmful).
// `playerOnly` restricts to player-cast auras. Returns -1 if not found.
int FindSlotBySpellID(const uint8_t *unit, uint32_t spellID,
                      const Filter *filter, CasterMode caster = CasterMode::Any);

// Finds the absolute slot of a populated aura whose locale-resolved
// spell name exactly matches `spellName`. Case-sensitive. Otherwise
// same semantics as `FindSlotBySpellID`. Returns -1 if not found
// or if `spellName` is null/empty.
int FindSlotBySpellName(const uint8_t *unit, const char *spellName,
                        const Filter *filter, CasterMode caster = CasterMode::Any);

// True iff the aura at `slot` was cast by the local player, per the
// `Aura::Source` cache. False on a cache miss (caster unknown) — so a
// PLAYER-filtered query excludes auras whose cast we didn't observe.
bool IsPlayerCast(const uint8_t *unit, int slot);

// Applies a `CasterMode` to a per-aura "was cast by the local player" answer.
// `Any` → always true; `PlayerOnly` → the answer; `NotPlayer` → its negation.
bool CasterMatches(CasterMode caster, bool isPlayerCast);

// Display stack count for the given slot (engine stores stacks-1,
// we add 1). Returns 0 if the unit is null.
int ReadStacks(const uint8_t *unit, int slot);

// Returns true if the engine considers `spellRecord` a user-visible
// aura. Thin wrapper around `FUN_SPELL_IS_VISIBLE_AURA`. The record
// pointer must come from `Spell.dbc[spellID]`.
bool IsVisible(const uint8_t *spellRecord);

// Looks up `SpellDispelType.dbc[dispelTypeID]` and returns the
// locale-applied name (e.g. "Magic", "Curse", "Disease", "Poison"),
// or nullptr if the ID is 0, out of range, or has no name.
const char *DispelName(uint32_t dispelTypeID);

// Builds a modern-style `AuraData` table on top of the Lua stack
// from the aura currently in `unit`'s descriptor at `slot`. Caller
// is responsible for having validated that the slot's spell ID is
// non-zero and visible (or just wants the data anyway).
//
// Populates these fields with real data:
//   name, icon, applications, spellId, dispelName,
//   isHelpful, isHarmful, timeMod, duration
//
// And these from the `Aura::Source` SMSG_SPELL_GO cache when available
// (else their inapplicable default):
//   expirationTime (player: engine buff table; others: cache; 0 on miss),
//   sourceUnit     (caster token from the cache; nil on miss),
//   sourceGUID     (caster "0x..." GUID string; nil on miss — a ClassicAPI
//                   extension, not in retail AuraData)
//
// And these with vanilla-truthful defaults (matches modern's
// "field present but inapplicable" semantics):
//   charges=0, maxCharges=0, auraInstanceID=nil, points=nil,
//   isStealable=false, isBossAura=false, isFromPlayerOrPlayerPet=false,
//   isNameplateOnly=false, nameplateShowAll=false,
//   nameplateShowPersonal=false, canApplyAura=false,
//   shouldConsolidate=false, isRaid=false
//
// Net stack effect: +1 (the table).
void Push(void *L, const uint8_t *unit, int slot);

// Fallback for the index path when the descriptor has dropped an aura's slot
// (rogue stealth, party range fluctuation) but it's still active server-side.
// `oneBasedIndex` is the same index the caller passed to `FindNthSlot`; this
// resolves it against the `Aura::Source` cache *after* the descriptor matches
// (so cache entries append after descriptor ones, preserving existing index
// order). On a hit it pushes the AuraData table and returns true; on a miss it
// pushes nothing and returns false (caller pushes nil). Only entries whose
// helpful/harmful classification matches `filter` and that aren't already in a
// populated descriptor slot are considered.
bool PushNthCacheFallback(void *L, const uint8_t *unit, int oneBasedIndex,
                          Filter filter, CasterMode caster = CasterMode::Any);

// Appends every eligible `Aura::Source` cache fallback for `unit` matching
// `filter` into the array table at `outerIdx`, continuing from `nextKey`
// (updated in place). The bulk-enumeration analog of `PushNthCacheFallback`,
// for `GetUnitAuras`. Skips entries already present in a populated descriptor
// slot so it never double-lists.
void AppendCacheFallbacks(void *L, const uint8_t *unit, Filter filter,
                          CasterMode caster, int outerIdx, int &nextKey);

// ── Out-of-range groupmate path ────────────────────────────────────────────
//
// When a party/raid member has no live CGUnit at all (different map, far out
// of range) the token resolves to a GUID but no object, so there is no
// descriptor to read. The server still transmits that member's current aura
// spell IDs via SMSG_PARTY_MEMBER_STATS, which the client keeps in the
// group-member stats structs (`Group::MemberStats::AuraArray`). These functions
// enumerate that array — exactly what the built-in `UnitBuff`/`UnitDebuff` do
// out of range — and build the same AuraData table.
//
// Spell IDs only come off the wire: `applications` is 1 (stacks aren't sent)
// and IDs are u16-truncated (custom IDs > 65535 are wrong). Any caster / real
// expirationTime we observed for the member via SMSG_SPELL_GO is still merged
// in from the `Aura::Source` cache. `guid` of 0 (not a rostered member) yields
// no results. These are only meaningful when the unit has NO descriptor —
// callers use them in the `unit == nullptr` branch, so there is no descriptor
// to dedup against.

// Pushes AuraData for the `oneBasedIndex`-th group-array aura on `guid`
// matching `filter`. On a hit pushes the table and returns true; otherwise
// pushes nothing and returns false (caller pushes nil).
bool PushNthGroupAura(void *L, uint64_t guid, int oneBasedIndex, Filter filter,
                      CasterMode caster = CasterMode::Any);

// Pushes AuraData for the group-array aura with `spellID` on `guid`, optionally
// restricted to one filter range (nullptr = both, helpful first). Returns true
// on a hit (table pushed), false otherwise (nothing pushed).
bool PushGroupAuraBySpellID(void *L, uint64_t guid, uint32_t spellID,
                            const Filter *filter, CasterMode caster = CasterMode::Any);

// As `PushGroupAuraBySpellID` but matched by locale-resolved spell name
// (case-sensitive exact match), mirroring `FindSlotBySpellName`.
bool PushGroupAuraBySpellName(void *L, uint64_t guid, const char *spellName,
                              const Filter *filter, CasterMode caster = CasterMode::Any);

// Appends every group-array aura on `guid` matching `filter` into the array
// table at `outerIdx`, continuing from `nextKey` (updated in place). The
// bulk-enumeration analog of `PushNthGroupAura`, for `GetUnitAuras`.
void AppendGroupAuras(void *L, uint64_t guid, Filter filter, CasterMode caster,
                      int outerIdx, int &nextKey);

} // namespace Aura::Data
