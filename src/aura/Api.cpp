// This file is part of ClassicAPI.
//
// ClassicAPI is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// ClassicAPI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.

// `C_UnitAuras.*` — modern aura-data namespace. Returns AuraData
// tables built by `Aura::Data::Push` (see `Data.h`). The fields the
// vanilla descriptor exposes (`name`, `icon`, `applications`,
// `spellId`, `dispelName`, `isHelpful`, `isHarmful`, `timeMod`)
// match modern; the fields that depend on systems vanilla doesn't
// have (`duration`, `expirationTime`, `auraInstanceID`, charges,
// nameplate flags, etc.) are populated with vanilla-truthful
// defaults so consumers reading those keys get sensible values.
//
// Filter parsing here mirrors what most modern addons actually pass:
// "HELPFUL" / "HARMFUL" are honored, every other modern filter
// (`PLAYER` / `RAID` / `CANCELABLE` / `INCLUDE_NAME_PLATE_ONLY`) is
// accepted but no-ops — they'd require either source-GUID tracking
// (`PLAYER`), class-dispel-matrix infra (`RAID`), or systems
// vanilla doesn't have at all.

#include "Data.h"

#include "Game.h"
#include "Offsets.h"
#include "ui/ColorData.h"
#include "unit/Identity.h"

#include <cstdint>
#include <cstring>

namespace Aura::Api {

namespace {

using ResolveUnitToken_t = void *(__fastcall *)(const char *token);

const uint8_t *ResolveUnit(const char *token) {
    if (token == nullptr)
        return nullptr;
    auto fn = reinterpret_cast<ResolveUnitToken_t>(
        static_cast<uintptr_t>(Offsets::FUN_RESOLVE_UNIT_TOKEN));
    return static_cast<const uint8_t *>(fn(token));
}

// Resolves a token to its GUID for the out-of-range (no live CGUnit) path.
// Only valid to call AFTER `ResolveUnit(token)` returned null *without raising*:
// `ResolveUnit` runs the engine's token→GUID resolver first, so reaching a null
// return means the token was well-formed (the resolver never raised). This runs
// the same non-raising resolver and returns 0 for a token that maps to no
// rostered member — which the group-array push paths treat as "no auras".
uint64_t GuidForOutOfRange(const char *token) {
    if (token == nullptr)
        return 0;
    return Unit::Identity::GuidForToken(token);
}

// Parses the filter string. Returns Helpful by default; Harmful if
// the string contains the substring "HARMFUL". Case-sensitive — the
// modern API documents the filter tokens as upper-case constants
// (`"HELPFUL"` etc.) and addons that pass them through case-changing
// transforms are already broken on modern too.
Data::Filter ParseFilter(const char *filter) {
    if (filter == nullptr)
        return Data::Filter::Helpful;
    if (strstr(filter, "HARMFUL") != nullptr)
        return Data::Filter::Harmful;
    return Data::Filter::Helpful;
}

// The `PLAYER` filter token restricts results to auras the local player
// cast. No other modern token contains the substring "PLAYER", so a plain
// substring test is safe.
bool HasPlayerFilter(const char *filter) {
    return filter != nullptr && strstr(filter, "PLAYER") != nullptr;
}

const char *ArgUnit(void *L, int idx) {
    if (!Game::Lua::IsString(L, idx))
        return nullptr;
    return Game::Lua::ToString(L, idx);
}

int ArgInt(void *L, int idx) {
    if (!Game::Lua::IsNumber(L, idx))
        return 0;
    return static_cast<int>(Game::Lua::ToNumber(L, idx));
}

const char *ArgOptString(void *L, int idx) {
    if (!Game::Lua::IsString(L, idx))
        return nullptr;
    return Game::Lua::ToString(L, idx);
}

// Pushes `AuraData` for the n-th aura on `unit` matching `filter`,
// or nil if no such aura. Used by `GetAuraDataByIndex` and the
// filter-locked aliases.
int PushAuraByIndex(void *L, const char *unitToken, int index,
                    Data::Filter filter, bool playerOnly = false) {
    const uint8_t *unit = ResolveUnit(unitToken);
    if (unit != nullptr) {
        const int slot = Data::FindNthSlot(unit, index, filter, playerOnly);
        if (slot >= 0) {
            Data::Push(L, unit, slot);
            return 1;
        }
        // Descriptor exhausted — an aura the engine dropped from the slot array
        // (rogue stealth, nearby party range fluctuation) may still be live in
        // the Aura::Source cache. Surface it after the descriptor entries.
        if (Data::PushNthCacheFallback(L, unit, index, filter, playerOnly))
            return 1;
    } else {
        // No live CGUnit — an out-of-range / cross-map groupmate. The server
        // still sends their aura spell IDs via SMSG_PARTY_MEMBER_STATS; read
        // them the same way the built-in UnitBuff/UnitDebuff do.
        if (Data::PushNthGroupAura(L, GuidForOutOfRange(unitToken), index,
                                   filter, playerOnly))
            return 1;
    }
    Game::Lua::PushNil(L);
    return 1;
}

int __fastcall Script_GetAuraDataByIndex(void *L) {
    const char *unit = ArgUnit(L, 1);
    const int index = ArgInt(L, 2);
    const char *filterStr = ArgOptString(L, 3);
    if (unit == nullptr || index < 1) {
        Game::Lua::PushNil(L);
        return 1;
    }
    return PushAuraByIndex(L, unit, index, ParseFilter(filterStr),
                           HasPlayerFilter(filterStr));
}

int __fastcall Script_GetBuffDataByIndex(void *L) {
    const char *unit = ArgUnit(L, 1);
    const int index = ArgInt(L, 2);
    if (unit == nullptr || index < 1) {
        Game::Lua::PushNil(L);
        return 1;
    }
    return PushAuraByIndex(L, unit, index, Data::Filter::Helpful);
}

int __fastcall Script_GetDebuffDataByIndex(void *L) {
    const char *unit = ArgUnit(L, 1);
    const int index = ArgInt(L, 2);
    if (unit == nullptr || index < 1) {
        Game::Lua::PushNil(L);
        return 1;
    }
    return PushAuraByIndex(L, unit, index, Data::Filter::Harmful);
}

int __fastcall Script_GetUnitAuraBySpellID(void *L) {
    const char *unitToken = ArgUnit(L, 1);
    const int spellID = ArgInt(L, 2);
    const char *filterStr = ArgOptString(L, 3);
    if (unitToken == nullptr || spellID <= 0) {
        Game::Lua::PushNil(L);
        return 1;
    }
    const uint8_t *unit = ResolveUnit(unitToken);
    Data::Filter f;
    const Data::Filter *fp = nullptr;
    if (filterStr != nullptr) {
        f = ParseFilter(filterStr);
        fp = &f;
    }
    const bool playerOnly = HasPlayerFilter(filterStr);
    if (unit != nullptr) {
        const int slot = Data::FindSlotBySpellID(
            unit, static_cast<uint32_t>(spellID), fp, playerOnly);
        if (slot >= 0) {
            Data::Push(L, unit, slot);
            return 1;
        }
    } else if (Data::PushGroupAuraBySpellID(L, GuidForOutOfRange(unitToken),
                                            static_cast<uint32_t>(spellID), fp,
                                            playerOnly)) {
        // Out-of-range groupmate — read from the group-member aura array.
        return 1;
    }
    Game::Lua::PushNil(L);
    return 1;
}

int __fastcall Script_GetAuraDataBySpellName(void *L) {
    const char *unitToken = ArgUnit(L, 1);
    const char *spellName = ArgUnit(L, 2);
    const char *filterStr = ArgOptString(L, 3);
    if (unitToken == nullptr || spellName == nullptr || *spellName == '\0') {
        Game::Lua::PushNil(L);
        return 1;
    }
    const uint8_t *unit = ResolveUnit(unitToken);
    Data::Filter f;
    const Data::Filter *fp = nullptr;
    if (filterStr != nullptr) {
        f = ParseFilter(filterStr);
        fp = &f;
    }
    const bool playerOnly = HasPlayerFilter(filterStr);
    if (unit != nullptr) {
        const int slot = Data::FindSlotBySpellName(unit, spellName, fp, playerOnly);
        if (slot >= 0) {
            Data::Push(L, unit, slot);
            return 1;
        }
    } else if (Data::PushGroupAuraBySpellName(L, GuidForOutOfRange(unitToken),
                                              spellName, fp, playerOnly)) {
        // Out-of-range groupmate — read from the group-member aura array.
        return 1;
    }
    Game::Lua::PushNil(L);
    return 1;
}

int __fastcall Script_GetPlayerAuraBySpellID(void *L) {
    const int spellID = ArgInt(L, 1);
    if (spellID <= 0) {
        Game::Lua::PushNil(L);
        return 1;
    }
    const uint8_t *unit = ResolveUnit("player");
    const int slot = Data::FindSlotBySpellID(unit, static_cast<uint32_t>(spellID), nullptr);
    if (slot < 0) {
        Game::Lua::PushNil(L);
        return 1;
    }
    Data::Push(L, unit, slot);
    return 1;
}

// Iterates one slot range and pushes AuraData tables into `outer` at
// sequential keys starting from `nextKey`. Updates `nextKey` so a
// follow-up call can append to the same outer table.
void AppendRangeToArray(void *L, const uint8_t *unit, int outerIdx,
                       Data::Filter filter, int &nextKey, bool playerOnly) {
    const int start = (filter == Data::Filter::Harmful)
                          ? Offsets::UNIT_AURA_BUFF_COUNT
                          : 0;
    const int end = (filter == Data::Filter::Harmful)
                        ? Offsets::UNIT_AURA_TOTAL
                        : Offsets::UNIT_AURA_BUFF_COUNT;
    for (int slot = start; slot < end; ++slot) {
        if (!Data::IsSlotPopulated(unit, slot))
            continue;
        if (playerOnly && !Data::IsPlayerCast(unit, slot))
            continue;
        Game::Lua::PushNumber(L, static_cast<double>(nextKey++));
        Data::Push(L, unit, slot);
        Game::Lua::SetTable(L, outerIdx);
    }
    // Append auras the descriptor dropped but Aura::Source still has live.
    Data::AppendCacheFallbacks(L, unit, filter, playerOnly, outerIdx, nextKey);
}

int __fastcall Script_GetUnitAuras(void *L) {
    const char *unitToken = ArgUnit(L, 1);
    const char *filterStr = ArgOptString(L, 2);
    const uint8_t *unit = ResolveUnit(unitToken);

    Game::Lua::SetTop(L, 0);
    Game::Lua::NewTable(L);

    // Range tokens are independent of PLAYER: an explicit HELPFUL/HARMFUL
    // selects that range, neither selects both. So `"PLAYER"` alone returns
    // both ranges restricted to player-cast auras (matching retail).
    const bool playerOnly = HasPlayerFilter(filterStr);
    const bool hasHelpful = filterStr != nullptr && strstr(filterStr, "HELPFUL") != nullptr;
    const bool hasHarmful = filterStr != nullptr && strstr(filterStr, "HARMFUL") != nullptr;
    const bool both = !hasHelpful && !hasHarmful;

    int nextKey = 1;
    if (unit != nullptr) {
        if (hasHelpful || both)
            AppendRangeToArray(L, unit, 1, Data::Filter::Helpful, nextKey, playerOnly);
        if (hasHarmful || both)
            AppendRangeToArray(L, unit, 1, Data::Filter::Harmful, nextKey, playerOnly);
    } else {
        // No live CGUnit — out-of-range / cross-map groupmate. Enumerate the
        // group-member aura array (spell IDs the server still transmits), the
        // same source the built-in UnitBuff/UnitDebuff read out of range.
        const uint64_t guid = GuidForOutOfRange(unitToken);
        if (hasHelpful || both)
            Data::AppendGroupAuras(L, guid, Data::Filter::Helpful, playerOnly, 1, nextKey);
        if (hasHarmful || both)
            Data::AppendGroupAuras(L, guid, Data::Filter::Harmful, playerOnly, 1, nextKey);
    }
    return 1;
}

// Pushes a plain `{r, g, b, a}` table decoded from the packed argb
// int in `ColorData.h`. Used only on the fallback path where
// `!!!ClassicAPI/Util/Color.lua` hasn't run — without `CreateColor`
// we can't build a real ColorMixin, but addons reading `.r/.g/.b/.a`
// still get the right numbers.
void PushPlainColorTable(void *L, int32_t argb) {
    const uint32_t v = static_cast<uint32_t>(argb);
    Game::Lua::NewTable(L);
    Game::Lua::SetFieldNumber(L, "r", ((v >> 16) & 0xFF) / 255.0);
    Game::Lua::SetFieldNumber(L, "g", ((v >>  8) & 0xFF) / 255.0);
    Game::Lua::SetFieldNumber(L, "b", ( v        & 0xFF) / 255.0);
    Game::Lua::SetFieldNumber(L, "a", ((v >> 24) & 0xFF) / 255.0);
}

bool PushColorByTag(void *L, const char *baseTag) {
    for (int i = 0; i < UI::ColorData::kColorCount; ++i) {
        if (strcmp(UI::ColorData::kColors[i].baseTag, baseTag) == 0) {
            PushPlainColorTable(L, UI::ColorData::kColors[i].argb);
            return true;
        }
    }
    return false;
}

// Mirrors modern retail's one-liner:
//     return _G["DEBUFF_TYPE_"..type:upper().."_COLOR"]
//            or DEBUFF_TYPE_NONE_COLOR
//
// `!!!ClassicAPI/Util/Color.lua` walks `C_UIColor.GetColors()` at
// addon load and publishes every entry as a ColorMixin under its
// `baseTag` global. By the time any addon calls us,
// `_G.DEBUFF_TYPE_MAGIC_COLOR` etc. are already ColorMixin
// instances — we push the same one, no rebuild, no duplicate
// source of truth. The Enrage row is a ClassicAPI extension in
// `ColorData.h` so it gets the same treatment.
//
// Fallback path: if `!!!ClassicAPI` isn't loaded (or hasn't reached
// `Color.lua` yet), the global lookup returns nil. We then read the
// raw argb out of `ColorData.h` and push a plain `{r,g,b,a}` table
// so consumers don't get a nil and crash on `:GetRGB()`. The plain
// table loses ColorMixin methods but preserves the field access most
// callers actually use.
int __fastcall Script_GetAuraDispelTypeColor(void *L) {
    const char *type = ArgOptString(L, 1);

    char tag[64];
    if (type != nullptr && type[0] != '\0') {
        std::memcpy(tag, "DEBUFF_TYPE_", 12);
        size_t off = 12;
        for (size_t i = 0; type[i] != '\0' && off < sizeof(tag) - 7;
             ++i, ++off) {
            const char c = type[i];
            tag[off] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        }
        std::memcpy(tag + off, "_COLOR", 7); // includes the NUL
    } else {
        std::memcpy(tag, "DEBUFF_TYPE_NONE_COLOR", 23);
    }

    Game::Lua::SetTop(L, 0);
    Game::Lua::PushString(L, tag);
    Game::Lua::GetTable(L, Game::Lua::GLOBALS_INDEX);
    if (Game::Lua::Type(L, -1) == Game::Lua::TYPE_TABLE)
        return 1;

    Game::Lua::SetTop(L, 0);
    if (PushColorByTag(L, tag))
        return 1;
    PushColorByTag(L, "DEBUFF_TYPE_NONE_COLOR");
    return 1;
}

} // namespace

static void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_UnitAuras", "GetAuraDataByIndex",
                                     &Script_GetAuraDataByIndex);
    Game::Lua::RegisterTableFunction("C_UnitAuras", "GetBuffDataByIndex",
                                     &Script_GetBuffDataByIndex);
    Game::Lua::RegisterTableFunction("C_UnitAuras", "GetDebuffDataByIndex",
                                     &Script_GetDebuffDataByIndex);
    Game::Lua::RegisterTableFunction("C_UnitAuras", "GetUnitAuraBySpellID",
                                     &Script_GetUnitAuraBySpellID);
    Game::Lua::RegisterTableFunction("C_UnitAuras", "GetPlayerAuraBySpellID",
                                     &Script_GetPlayerAuraBySpellID);
    Game::Lua::RegisterTableFunction("C_UnitAuras", "GetAuraDataBySpellName",
                                     &Script_GetAuraDataBySpellName);
    Game::Lua::RegisterTableFunction("C_UnitAuras", "GetUnitAuras",
                                     &Script_GetUnitAuras);
    Game::Lua::RegisterTableFunction("C_UnitAuras", "GetAuraDispelTypeColor",
                                     &Script_GetAuraDispelTypeColor);
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace Aura::Api
