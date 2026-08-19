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

// Trade-skill *list* links (`|Htrade:...|h`) — the shareable "here's my
// profession and which recipes I know" link. Vanilla 1.12 has NO such link
// type (the engine's SetHyperlink parser only knows `item:` and `enchant:`);
// 2.x introduced it. We backport the whole thing: the link is built entirely
// in C here; only the click-to-display viewer lives in the embedded addon
// (Util/TradeSkillLink.lua), since that's UI.
//
// Wire format (a ClassicAPI-private shape — nothing native emits/reads it, so
// we're free to define it; modeled on the 2.x/3.3.5 `trade:` link, with the
// linker's name in the slot 2.x used for the GUID):
//
//     |cffffd000|Htrade:<skillLineID>:<curRank>:<maxRank>:<linkerName>:<base64 bits>|h[Name]|h|r
//
//   - skillLineID  — SkillLine.dbc row (e.g. 164 = Blacksmithing)
//   - curRank/maxRank — the linker's skill level (cosmetic; viewer title)
//   - linkerName   — the linking character's name
//   - base64 bits — a bitfield over the skill line's *canonical recipe list*:
//     one bit per possible recipe, set = the linker knows it.
//
// The canonical recipe list is every SkillLineAbility.dbc row for the skill
// line whose spell actually produces a craft (see IsCraftRecipe — an item,
// enchant, or Turtle LEARN_SPELL recipe; this excludes profession abilities
// like Disenchant), in ascending record-ID order. That ordering is
// deterministic and identical on every client with the same DBC, so builder
// and reader agree on the bit-index space without any handshake. "Which
// recipes does the linker know" comes from the player-spell bitmap
// (VAR_PLAYER_SPELL_BITMAP — the same store IsPlayerSpell reads, which covers
// profession recipes), NOT from the trade-skill window's server list.
//
// The TradeSkill and Craft windows are separate frames with separate storage
// and can both be open at once, so each gets its own builder (matching
// WotLK) — each reads only its own window's open-state global (both zeroed by
// the engine on close), so "both open" is unambiguous and neither guesses:
//
//   - `C_TradeSkillUI.GetTradeSkillListLink()` — the TradeSkill window
//   - `C_TradeSkillUI.GetCraftListLink()`      — the Craft window (Enchanting)
//   - `C_TradeSkillUI.GetTradeSkillListRecipes(skillLineID, bits)` — decoder
//     for the viewer.

#include "Game.h"
#include "Offsets.h"
#include "dbc/Lookup.h"
#include "spell/Lookup.h"
#include "unit/Identity.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace TradeSkill::Link {

namespace {

// 6 recipes per character, LSB-first (mirrors 3.x's encoder).
constexpr char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr int kRecipesPerChar = 6;
constexpr int kMaxRecipes = 1024; // largest vanilla skill line is ~340
constexpr int kBitsBufSize = kMaxRecipes / kRecipesPerChar + 2;

int Base64Value(char c) {
    for (int i = 0; i < 64; ++i)
        if (kBase64[i] == c)
            return i;
    return -1;
}

// A spell is a craftable recipe (vs. a profession ability like Disenchant, or
// a rank-up / skill-grant spell) iff one of its effects produces a craft: an
// item (CREATE_ITEM), an enchant (ENCHANT_ITEM / _TEMPORARY), or Turtle's
// custom recipes (LEARN_SPELL). Filtering on the skill-up threshold alone
// wrongly kept Disenchant (which carries thresholds but isn't a recipe).
bool IsCraftRecipe(int spellID) {
    const uint8_t *rec = Spell::Lookup::RecordForID(spellID);
    if (rec == nullptr)
        return false;
    const int32_t *eff = Game::Ptr<const int32_t>(
        rec, Offsets::OFF_SPELL_RECORD_EFFECT);
    for (int i = 0; i < Offsets::SPELL_RECORD_EFFECT_COUNT; ++i) {
        const int e = eff[i];
        if (e == Offsets::SPELL_EFFECT_CREATE_ITEM ||
            e == Offsets::SPELL_EFFECT_LEARN_SPELL ||
            e == Offsets::SPELL_EFFECT_ENCHANT_ITEM ||
            e == Offsets::SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
            return true;
    }
    return false;
}

// Fills `outSpell` with the recipe spellIDs for `skillLineID`, in canonical
// (ascending record-ID) order, and — when non-null — `outGrey`/`outGreen` with
// the skill levels the recipe turns grey / green at (its difficulty tiers; the
// required-skill field 7 is uselessly 1 for nearly every trainer-taught
// recipe). Returns the count (capped at `maxOut`). Same walk builder and reader
// use, so the bit-index space is identical.
int BuildRecipeList(uint32_t skillLineID, int *outSpell, int *outGrey,
                    int *outGreen, int maxOut) {
    auto *const *records = Game::Read<const uint8_t *const *>(
        Offsets::VAR_SKILL_LINE_ABILITY_RECORDS);
    const int count = Game::Read<int>(
        Offsets::VAR_SKILL_LINE_ABILITY_COUNT);
    if (records == nullptr || count <= 0)
        return 0;

    int n = 0;
    for (int i = 1; i <= count && n < maxOut; ++i) {
        const uint8_t *rec = records[i];
        if (rec == nullptr)
            continue;
        if (static_cast<uint32_t>(Game::Read<int>(
                rec, Offsets::OFF_SLA_SKILL_ID)) != skillLineID)
            continue;
        const int spellID =
            Game::Read<int>(rec, Offsets::OFF_SLA_SPELL_ID);
        if (!IsCraftRecipe(spellID))
            continue;
        if (outGrey != nullptr)
            outGrey[n] = Game::Read<int>(
                rec, Offsets::OFF_SLA_TRIVIAL_SKILL_HIGH);
        if (outGreen != nullptr)
            outGreen[n] = Game::Read<int>(
                rec, Offsets::OFF_SLA_TRIVIAL_SKILL_LOW);
        outSpell[n++] = spellID;
    }
    return n;
}

// Same bit the engine (and our IsPlayerSpell) reads — covers profession
// recipes, not just trained abilities.
bool IsKnownSpell(int spellID) {
    if (spellID <= 0)
        return false;
    auto *bitmap = Game::Read<const uint32_t *>(
        Offsets::VAR_PLAYER_SPELL_BITMAP);
    if (bitmap == nullptr)
        return false;
    return (bitmap[static_cast<uint32_t>(spellID) >> 5] &
            (1u << (spellID & 31))) != 0;
}

// Skill line (SkillLine.dbc row) of the first SLA row that teaches `spellID`,
// or 0 if none. Used to identify the Craft window's profession, which — unlike
// the TradeSkill window — has no skill-line global (the engine derives it from
// the recipe, see `GetCraftDisplaySkillLine` at `0x004F6CB0`).
uint32_t SkillForSpell(int spellID) {
    if (spellID <= 0)
        return 0;
    auto *const *records = Game::Read<const uint8_t *const *>(
        Offsets::VAR_SKILL_LINE_ABILITY_RECORDS);
    const int count = Game::Read<int>(
        Offsets::VAR_SKILL_LINE_ABILITY_COUNT);
    if (records == nullptr || count <= 0)
        return 0;
    for (int i = 1; i <= count; ++i) {
        const uint8_t *rec = records[i];
        if (rec != nullptr &&
            Game::Read<int>(rec, Offsets::OFF_SLA_SPELL_ID) ==
                spellID)
            return static_cast<uint32_t>(
                Game::Read<int>(rec, Offsets::OFF_SLA_SKILL_ID));
    }
    return 0;
}

// Skill line of the open Craft window (Enchanting, pet training) — derived
// from its first real recipe entry, mirroring the engine. 0 if none open.
uint32_t CraftSkillLine() {
    const int count = Game::Read<int>(Offsets::VAR_CRAFT_COUNT);
    if (count <= 0)
        return 0;
    const int cap = (count < kMaxRecipes) ? count : kMaxRecipes;
    for (int slot = 0; slot < cap; ++slot) {
        const int spellID = Spell::Lookup::RecipeSlotSpellID(
            Offsets::VAR_CRAFT_ENTRIES, Offsets::VAR_CRAFT_COUNT, slot);
        if (spellID > 0) {
            const uint32_t skill = SkillForSpell(spellID);
            if (skill != 0)
                return skill;
        }
    }
    return 0;
}

// Encodes the local player's known-recipe bitfield for `skillLineID` into
// `out` (must hold kBitsBufSize). Returns the recipe count, or -1 if the skill
// line has no recipes.
int EncodeKnownBits(uint32_t skillLineID, char *out) {
    int spells[kMaxRecipes];
    const int n = BuildRecipeList(skillLineID, spells, nullptr, nullptr,
                                  kMaxRecipes);
    if (n <= 0)
        return -1;

    int nb = 0;
    unsigned acc = 0;
    int used = 0;
    for (int i = 0; i < n; ++i) {
        if (IsKnownSpell(spells[i]))
            acc |= (1u << used);
        if (++used == kRecipesPerChar) {
            out[nb++] = kBase64[acc];
            acc = 0;
            used = 0;
        }
    }
    if (used != 0)
        out[nb++] = kBase64[acc];
    out[nb] = '\0';
    return n;
}

// Current / max skill rank for the local player in `skillLine`, read from the
// CGPlayer `+0xE68` sub-struct exactly as the engine's own accessors do (see
// the FUN_SKILL_LINE_TO_SLOT block in Offsets.h). Leaves 0/0 when the player
// isn't resolvable or doesn't have the skill.
void ReadPlayerSkillRank(int skillLine, int *outCur, int *outMax) {
    *outCur = 0;
    *outMax = 0;
    const uint8_t *player = Unit::Identity::PlayerObject();
    if (player == nullptr)
        return;
    using SlotFn = int(__thiscall *)(const void *, int);
    const int slot = reinterpret_cast<SlotFn>(
        Offsets::FUN_SKILL_LINE_TO_SLOT)(player, skillLine);
    if (slot < 0)
        return;
    const uint8_t *base = Game::Read<const uint8_t *>(
        player, Offsets::OFF_CGPLAYER_INFO);
    if (base == nullptr)
        return;
    const uint8_t *rec =
        base + Offsets::OFF_SKILL_INFO_TABLE + slot * Offsets::SKILL_INFO_STRIDE;
    uint32_t cur = Game::Read<uint16_t>(rec, Offsets::OFF_SKILL_INFO_CUR);
    uint32_t mx = Game::Read<uint16_t>(rec, Offsets::OFF_SKILL_INFO_MAX);
    const uint32_t bonus =
        Game::Read<uint16_t>(rec, Offsets::OFF_SKILL_INFO_BONUS);
    if (cur != 0)
        cur += bonus;
    if (mx != 0)
        mx += bonus;
    *outCur = static_cast<int>(cur);
    *outMax = static_cast<int>(mx);
}

// The local player's character name via the engine's object name getter.
const char *PlayerCharacterName() {
    const uint8_t *player = Unit::Identity::PlayerObject();
    if (player == nullptr)
        return "";
    using GetName_t = const char *(__thiscall *)(const void *, int *);
    const char *name = reinterpret_cast<GetName_t>(
        Offsets::FUN_OBJECT_GET_NAME)(player, nullptr);
    return (name != nullptr) ? name : "";
}

// Builds the full `|Htrade:...|h` link for whichever window `useCraft`
// selects. Pushes the link string (returns 1), or nothing (returns 0 → nil)
// when that window is closed or has no recipes.
int BuildLink(void *L, bool useCraft) {
    int skillLine;
    if (useCraft) {
        // Craft-window open-state flag; zeroed by the engine on close.
        if (Game::Read<int>(Offsets::VAR_CRAFT_WINDOW_STATE) == 0)
            return 0;
        skillLine = static_cast<int>(CraftSkillLine());
    } else {
        // TradeSkill skill-line global; zeroed by the engine on close.
        skillLine = Game::Read<int>(
            Offsets::VAR_CURRENT_TRADESKILL_LINE);
    }
    if (skillLine <= 0)
        return 0;

    char bits[kBitsBufSize];
    if (EncodeKnownBits(static_cast<uint32_t>(skillLine), bits) < 0)
        return 0;

    const char *prof = DBC::LocalizedField(
        Offsets::VAR_SKILL_LINE_RECORDS, Offsets::VAR_SKILL_LINE_COUNT,
        static_cast<uint32_t>(skillLine), Offsets::OFF_SKILL_LINE_NAME);
    if (prof == nullptr || *prof == '\0')
        return 0;

    int cur = 0, mx = 0;
    ReadPlayerSkillRank(skillLine, &cur, &mx);
    const char *who = PlayerCharacterName();

    char link[512];
    std::snprintf(link, sizeof link,
                  "|cffffd000|Htrade:%d:%d:%d:%s:%s|h[%s]|h|r", skillLine, cur,
                  mx, who, bits, prof);
    Game::Lua::PushString(L, link);
    return 1;
}

// `C_TradeSkillUI.GetTradeSkillListLink()` -> link for the open TradeSkill
// window, or nil.
int __fastcall Script_GetTradeSkillListLink(void *L) {
    return BuildLink(L, /*useCraft=*/false);
}

// `C_TradeSkillUI.GetCraftListLink()` -> link for the open Craft window, or nil.
int __fastcall Script_GetCraftListLink(void *L) {
    return BuildLink(L, /*useCraft=*/true);
}

// `C_TradeSkillUI.GetTradeSkillListRecipes(skillLineID, bits)`
//     -> { { spellID, isKnown, trivialLevel, greenLevel, createdItem,
//            numMade, reagents = { { itemID, count }, ... } }, ... }
// `trivialLevel` / `greenLevel` are the skill levels the recipe turns
// grey / green at — its difficulty tiers, for sorting and colouring.
// `createdItem` is the crafted item's ID (0 for enchants) — the viewer
// derives its subclass filter from it. `numMade` is how many the recipe
// yields (CREATE_ITEM base points + dice; 0 for enchants). `reagents` lists
// each consumed item + required count in DBC slot order (stopping at the
// first empty slot, the engine's own convention), for the detail pane.
//
// Rebuilds the canonical recipe list for `skillLineID` and decodes `bits`
// against it. Recipes past the end of `bits` (shorter/garbled string) decode
// as not-known rather than erroring. The Lua caller (the viewer) turns each
// spellID into a name/icon via GetSpellInfo and each reagent itemID into
// name/texture/on-hand-count via GetItemInfo / GetItemCount.
//
// The real 3.3.5 reagent API (GetTradeSkillReagentInfo etc.) can't serve this:
// it's indexed by position in the player's *own* open trade window, so it
// can't describe a linked recipe from someone else's profession. We read the
// reagents straight from Spell.dbc instead.
int __fastcall Script_GetTradeSkillListRecipes(void *L) {
    if (!Game::Lua::IsNumber(L, 1) || !Game::Lua::IsString(L, 2)) {
        Game::Lua::Error(
            L, "Usage: C_TradeSkillUI.GetTradeSkillListRecipes(skillLineID, bits)");
        return 0;
    }
    const uint32_t skillLine =
        static_cast<uint32_t>(Game::Lua::ToNumber(L, 1));
    const char *bits = Game::Lua::ToString(L, 2);
    const int blen = (bits != nullptr) ? static_cast<int>(std::strlen(bits)) : 0;

    int spells[kMaxRecipes];
    int greys[kMaxRecipes];
    int greens[kMaxRecipes];
    const int n = BuildRecipeList(skillLine, spells, greys, greens, kMaxRecipes);

    Game::Lua::NewTable(L);
    for (int i = 0; i < n; ++i) {
        const int ci = i / kRecipesPerChar;
        const int bi = i % kRecipesPerChar;
        bool known = false;
        if (ci < blen) {
            const int v = Base64Value(bits[ci]);
            if (v >= 0)
                known = (v & (1 << bi)) != 0;
        }

        // The recipe's crafted item (0 for enchants) — the viewer groups the
        // subclass filter by this item's subclass — plus its yield count.
        const uint8_t *srec = Spell::Lookup::RecordForID(spells[i]);
        int createdItem = 0;
        int numMade = 0;
        if (srec != nullptr) {
            createdItem = Game::Read<int>(
                srec, Offsets::OFF_SPELL_RECORD_CREATED_ITEM);
            // Yield: the CREATE_ITEM effect's magnitude (base points + dice),
            // mirroring the engine's item-creation math. 0 for enchants.
            const int32_t *eff = Game::Ptr<const int32_t>(
                srec, Offsets::OFF_SPELL_RECORD_EFFECT);
            const int32_t *basePts = Game::Ptr<const int32_t>(
                srec, Offsets::OFF_SPELL_RECORD_EFFECT_BASE_POINTS);
            const int32_t *baseDice = Game::Ptr<const int32_t>(
                srec, Offsets::OFF_SPELL_RECORD_EFFECT_BASE_DICE);
            for (int e = 0; e < Offsets::SPELL_RECORD_EFFECT_COUNT; ++e) {
                if (eff[e] == Offsets::SPELL_EFFECT_CREATE_ITEM) {
                    numMade = basePts[e] + baseDice[e];
                    break;
                }
            }
        }

        Game::Lua::PushNumber(L, static_cast<double>(i + 1));
        Game::Lua::NewTable(L);
        Game::Lua::SetFieldNumber(L, "spellID", static_cast<double>(spells[i]));
        Game::Lua::SetFieldBool(L, "isKnown", known);
        Game::Lua::SetFieldNumber(L, "trivialLevel",
                                  static_cast<double>(greys[i]));
        Game::Lua::SetFieldNumber(L, "greenLevel",
                                  static_cast<double>(greens[i]));
        Game::Lua::SetFieldNumber(L, "createdItem",
                                  static_cast<double>(createdItem));
        Game::Lua::SetFieldNumber(L, "numMade", static_cast<double>(numMade));

        // reagents = { {itemID, count}, ... } — nested array under "reagents"
        // (no SetField helper takes a table value, so push key + build + SetTable).
        Game::Lua::PushString(L, "reagents");
        Game::Lua::NewTable(L);
        if (srec != nullptr) {
            const int32_t *rids = Game::Ptr<const int32_t>(
                srec, Offsets::OFF_SPELL_REAGENT_ID);
            const int32_t *rcounts = Game::Ptr<const int32_t>(
                srec, Offsets::OFF_SPELL_REAGENT_COUNT);
            int outIdx = 0;
            for (int r = 0; r < Offsets::SPELL_MAX_REAGENTS; ++r) {
                if (rids[r] == 0)
                    break; // first empty slot ends the list
                Game::Lua::PushNumber(L, static_cast<double>(++outIdx));
                Game::Lua::NewTable(L);
                Game::Lua::SetFieldNumber(L, "itemID",
                                          static_cast<double>(rids[r]));
                Game::Lua::SetFieldNumber(L, "count",
                                          static_cast<double>(rcounts[r]));
                Game::Lua::SetTable(L, -3); // reagents[outIdx] = {itemID,count}
            }
        }
        Game::Lua::SetTable(L, -3); // entry.reagents = { ... }

        Game::Lua::SetTable(L, -3);
    }
    return 1;
}

void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_TradeSkillUI", "GetTradeSkillListLink",
                                     &Script_GetTradeSkillListLink);
    Game::Lua::RegisterTableFunction("C_TradeSkillUI", "GetCraftListLink",
                                     &Script_GetCraftListLink);
    Game::Lua::RegisterTableFunction("C_TradeSkillUI",
                                     "GetTradeSkillListRecipes",
                                     &Script_GetTradeSkillListRecipes);
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace

} // namespace TradeSkill::Link
