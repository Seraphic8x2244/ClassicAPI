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

// `C_PlayerInfo.CanUseItem(itemID)` — true iff the local player meets the
// item's *use/equip* requirements. This is the "is the item red in the
// tooltip" gate, and is distinct from its siblings:
//   - `C_Item.IsEquippableItem` — does the item fit *some* slot (static).
//   - `IsUsableItem`            — is the item's on-use ability castable now.
//   - `C_PlayerInfo.CanUseItem` — may the player equip/use it at all.
//
// So this is the answer to "mail chest on a Mage" (proficiency), "level-40
// item on a level-20" (RequiredLevel), and class/race-restricted gear.
//
// Not in vanilla 1.12 nor in our 3.3.5 reference client (the modern API
// landed in 1.14.4 / 3.4.2 / 10.0.5), so there's no cross-binary source;
// the logic mirrors the requirement checks the 1.12 item tooltip builder
// (`FUN_0052B650`) inlines for its red/green line coloring — the same
// `bVar23` "can't use this" flag, minus the display.
//
// Checks (all must pass):
//   1. Proficiency — for weapon/armor the engine keeps a per-item-class
//      subclass-proficiency bitmask (VAR_PROFICIENCY_TABLE, fed by
//      SMSG_SET_PROFICIENCY). `mask[m_class] & (1 << m_subClass)` must be
//      set. Item classes with no proficiency concept store mask 0 and are
//      unrestricted (matches the tooltip's `mask != 0` gate).
//   2. RequiredLevel <= player level.
//   3. AllowableClass / AllowableRace masks include the player (0 or all-
//      bits = no restriction).
//   4. RequiredSkill — the player must have the item's skill line at an
//      effective rank (current + temp bonus) >= RequiredSkillRank (e.g. a
//      mount "Requires Riding (150)"). Via FUN_SKILL_LINE_TO_SLOT.
//   5. RequiredSpell — the player must know the item's prerequisite spell,
//      or a higher rank of it: a crafting specialization ("Requires
//      Armorsmith") or a proficiency spell. Via the spell-knowledge bitmap
//      plus the engine's rank-chain walk.
//   6. RequiredHonorRank / RequiredCityRank — PvP-rank gates: the player's
//      honor rank (>= required) and earned PvP-medal bitmask.
//   7. RequiredReputation — current standing with the item's faction must
//      reach the required reaction band.
//
// Cache-miss handling matches `C_Item.IsEquippableItem`: a synchronous
// peek that returns false without firing a load.

#include "Game.h"
#include "Offsets.h"
#include "item/Arg.h"
#include "item/Record.h"
#include "spell/Lookup.h"
#include "unit/Identity.h"

#include <cstdint>

namespace Player::CanUseItem {

namespace {

// `__thiscall(player /*ecx*/, skillLineID) -> slot` (-1 if the player
// doesn't have the skill line). See VAR docs on FUN_SKILL_LINE_TO_SLOT.
using SkillLineToSlot_t = int(__thiscall *)(void *player, uint32_t skillLineID);
// `__fastcall(factionID) -> current total reputation` (base + bonus), 0
// for factions not in the player's list. See FUN_REPUTATION_GET_STANDING.
using RepStanding_t = int(__fastcall *)(int factionID);
// `__thiscall(player /*ecx*/, spellID) -> nonzero` if the player knows a
// higher rank in spellID's chain. See FUN_SPELL_RANK_CHAIN_KNOWN.
using SpellRankChainKnown_t = int(__thiscall *)(void *player, uint32_t spellID);

// True iff `mask` restricts a 1-based `index` (class/race) the player
// isn't part of. Mask 0 and 0xFFFFFFFF both mean "no restriction".
bool RestrictedOut(uint32_t mask, uint32_t index1Based) {
    if (mask == 0 || mask == 0xFFFFFFFF || index1Based == 0)
        return false;
    return (mask & (1u << ((index1Based - 1) & 31))) == 0;
}

// Per-item-class proficiency bitmask lookup. Returns false only when the
// item's class *has* a proficiency concept (mask != 0) and the item's
// subclass bit is unset.
bool ProficiencyMet(const uint8_t *record) {
    const uint32_t itemClass = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_CLASS);
    const uint32_t subClass = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_SUBCLASS);
    if (itemClass > 16) // table is 17 entries (m_class 0..16)
        return true;
    auto *table = reinterpret_cast<const uint32_t *>(
        static_cast<uintptr_t>(Offsets::VAR_PROFICIENCY_TABLE));
    const uint32_t mask = table[itemClass];
    if (mask == 0)
        return true; // class has no proficiency concept (consumables, …)
    return (mask & (1u << (subClass & 31))) != 0;
}

// RequiredSkill + RequiredSkillRank gate (e.g. a mount "Requires Riding
// (150)", a profession tool "Requires Engineering (200)"). Maps the
// item's SkillLine.dbc row to the player's skill slot; the player must
// have the line at an effective rank (current + temp bonus) >= required.
bool RequiredSkillMet(const uint8_t *player, const uint8_t *record) {
    const uint32_t reqSkill = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_REQUIRED_SKILL);
    if (reqSkill == 0)
        return true; // no skill requirement
    const uint32_t reqRank = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_REQUIRED_SKILL_RANK);

    auto toSlot = reinterpret_cast<SkillLineToSlot_t>(Offsets::FUN_SKILL_LINE_TO_SLOT);
    const int slot = toSlot(const_cast<uint8_t *>(player), reqSkill);
    if (slot < 0)
        return false; // player doesn't have the skill line at all

    auto *sub = *reinterpret_cast<const uint8_t *const *>(
        player + Offsets::OFF_CGPLAYER_INFO);
    if (sub == nullptr)
        return false;
    const uint8_t *rec = sub + Offsets::OFF_SKILL_INFO_TABLE +
                         slot * Offsets::SKILL_INFO_STRIDE;
    uint32_t rank = *reinterpret_cast<const uint16_t *>(rec + Offsets::OFF_SKILL_INFO_CUR);
    if (rank != 0) // add the temp bonus only when there's a base value
        rank += *reinterpret_cast<const uint16_t *>(rec + Offsets::OFF_SKILL_INFO_BONUS);
    return rank >= reqRank;
}

// True if the local player knows `spellID`, via the engine's spell-
// knowledge bitmap (the active-player path of `FUN_0060c740`, read
// directly — no player object needed). Covers trained abilities, talents,
// racials, profession recipes, and crafting specializations (Armorsmith,
// Weaponsmith, the Engineering branches, …), which are all single learned
// spells that land in this bitmap.
bool PlayerKnowsSpell(int spellID) {
    if (spellID <= 0)
        return false;
    auto *bitmap = *reinterpret_cast<const uint32_t *const *>(
        static_cast<uintptr_t>(Offsets::VAR_PLAYER_SPELL_BITMAP));
    if (bitmap == nullptr)
        return false;
    const int spellCount = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_SPELL_RECORD_COUNT));
    if (spellID > spellCount)
        return false;
    return (bitmap[spellID >> 5] & (1u << (spellID & 31))) != 0;
}

// RequiredSpell gate — the item's `RequiredSpell` (a Spell.dbc row) is the
// prerequisite the "Requires <spell>" tooltip line reflects: a crafting
// specialization like Armorsmith, or an armor/weapon proficiency spell.
// The player must know it. An unset (0) or invalid (no Spell.dbc record)
// requirement is no gate — matching the tooltip's validity guard, which
// skips the line for a missing row.
//
// Mirrors the tooltip's `bitmap(reqSpell) OR rank-chain(reqSpell)`: met if
// the player knows the exact RequiredSpell, or a higher rank of it (the
// latter covers a ranked RequiredSpell — `FUN_SPELL_RANK_CHAIN_KNOWN`
// walks the forward chain). Item RequiredSpell is a non-ranked
// specialization/proficiency in practice, so the chain walk is
// belt-and-suspenders, but it makes the check exact.
bool RequiredSpellMet(const uint8_t *player, const uint8_t *record) {
    const int reqSpell = *reinterpret_cast<const int32_t *>(
        record + Offsets::OFF_ITEMSTATS_REQUIRED_SPELL);
    if (reqSpell <= 0)
        return true; // no requirement
    if (::Spell::Lookup::RecordForID(reqSpell) == nullptr)
        return true; // invalid Spell.dbc row → tooltip skips the gate
    if (PlayerKnowsSpell(reqSpell))
        return true; // knows the exact spell
    auto chainKnown = reinterpret_cast<SpellRankChainKnown_t>(
        Offsets::FUN_SPELL_RANK_CHAIN_KNOWN);
    return chainKnown(const_cast<uint8_t *>(player),
                      static_cast<uint32_t>(reqSpell)) != 0;
}

// RequiredHonorRank — the player's PvP honor rank must be >= the item's.
// `sub` is the CGPlayer +0xE68 sub-struct (may be null pre-world).
bool RequiredHonorRankMet(const uint8_t *sub, const uint8_t *record) {
    const uint32_t req = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_REQUIRED_HONOR_RANK);
    if (req == 0)
        return true;
    if (sub == nullptr)
        return false;
    const uint8_t rank = *(sub + Offsets::OFF_CGPLAYER_HONOR_RANK);
    return rank >= req;
}

// RequiredCityRank — a PvP-medal bitmask gate: the item's rank R passes
// when the player has earned bit (R-1) in the medal mask.
bool RequiredCityRankMet(const uint8_t *sub, const uint8_t *record) {
    const uint32_t req = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_REQUIRED_CITY_RANK);
    if (req == 0)
        return true;
    if (sub == nullptr)
        return false;
    const uint32_t medals = *reinterpret_cast<const uint32_t *>(
        sub + Offsets::OFF_CGPLAYER_PVP_MEDALS);
    return (medals & (1u << ((req - 1) & 31))) != 0;
}

// RequiredReputation — current standing with the item's faction must
// reach the required reaction band. The band index (0=Hated … 7=Exalted)
// indexes the engine's reaction-min threshold table; the player's total
// rep with the faction must be >= that threshold.
bool RequiredReputationMet(const uint8_t *record) {
    const int faction = *reinterpret_cast<const int32_t *>(
        record + Offsets::OFF_ITEMSTATS_REQUIRED_FACTION);
    if (faction == 0)
        return true;
    const uint32_t band = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_REQUIRED_FACTION_RANK);
    if (band > 7) // reaction-min table is 8 bands (0..7); guard OOB
        return true;
    auto getStanding = reinterpret_cast<RepStanding_t>(Offsets::FUN_REPUTATION_GET_STANDING);
    const int current = getStanding(faction);
    const int threshold = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_REACTION_MIN_TABLE) + band * 4);
    return current >= threshold;
}

bool ComputeCanUse(int itemID) {
    if (itemID <= 0)
        return false;
    auto *record = Item::PeekRecord(static_cast<uint32_t>(itemID));
    if (record == nullptr)
        return false; // not cached → sync false, no load fired
    auto *player = Unit::Identity::PlayerObject();
    if (player == nullptr)
        return false; // pre-login / no player
    auto *desc = *reinterpret_cast<const uint8_t *const *>(
        player + Offsets::OFF_UNIT_DESCRIPTOR);
    if (desc == nullptr)
        return false;

    if (!ProficiencyMet(record))
        return false;

    const int playerLevel = *reinterpret_cast<const int *>(
        desc + Offsets::OFF_UNIT_FIELD_LEVEL);
    const uint32_t reqLevel = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_REQUIRED_LEVEL);
    if (static_cast<int>(reqLevel) > playerLevel)
        return false;

    const uint32_t allowClass = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_ALLOWABLE_CLASS);
    const uint32_t playerClass = *(desc + Offsets::OFF_UNIT_DESCRIPTOR_CLASS_BYTE);
    if (RestrictedOut(allowClass, playerClass))
        return false;

    const uint32_t allowRace = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_ALLOWABLE_RACE);
    const uint32_t playerRace = *(desc + Offsets::OFF_UNIT_DESCRIPTOR_RACE_BYTE);
    if (RestrictedOut(allowRace, playerRace))
        return false;

    if (!RequiredSkillMet(player, record))
        return false;

    if (!RequiredSpellMet(player, record))
        return false;

    // PvP rank + reputation gates read the CGPlayer +0xE68 sub-struct.
    auto *sub = *reinterpret_cast<const uint8_t *const *>(
        player + Offsets::OFF_CGPLAYER_INFO);
    if (!RequiredHonorRankMet(sub, record))
        return false;
    if (!RequiredCityRankMet(sub, record))
        return false;
    if (!RequiredReputationMet(record))
        return false;

    return true;
}

int __fastcall Script_CanUseItem(void *L) {
    const int itemID = Item::Arg::ResolveItemID(L, 1);
    Game::Lua::PushBool(L, ComputeCanUse(itemID));
    return 1;
}

} // namespace

static void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_PlayerInfo", "CanUseItem", &Script_CanUseItem);
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace Player::CanUseItem
