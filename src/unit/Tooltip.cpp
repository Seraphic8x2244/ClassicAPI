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

#include "Game.h"
#include "Offsets.h"
#include "guid/Guid.h"
#include "object/Resolve.h"

#include <cstdint>

namespace Unit::Tooltip {

// `GameTooltip:SetUnitAura(unit, index, [filter])` — modern unified-aura
// method. 1.12 splits this into `SetUnitBuff` (slot 32) and
// `SetUnitDebuff` (slot 33); we dispatch to the right one based on the
// `filter` string ("HELPFUL" → SetUnitBuff, "HARMFUL" → SetUnitDebuff).
//
// `filter` defaults to "HELPFUL" when omitted, matching modern
// behavior. Anything other than a HARMFUL match goes to SetUnitBuff.
static int __fastcall Script_GameTooltipSetUnitAura(void *L) {
    // Args: self (table), unit (string), index (number), filter (optional string)
    if (Game::Lua::Type(L, 1) != Game::Lua::TYPE_TABLE) {
        Game::Lua::Error(L, "Usage: GameTooltip:SetUnitAura(unit, index, [filter])");
        return 0;
    }
    if (!Game::Lua::IsString(L, 2) || !Game::Lua::IsNumber(L, 3)) {
        Game::Lua::Error(L, "Usage: GameTooltip:SetUnitAura(unit, index, [filter])");
        return 0;
    }

    bool isHarmful = false;
    if (Game::Lua::Type(L, 4) == Game::Lua::TYPE_STRING) {
        const char *filter = Game::Lua::ToString(L, 4);
        // Case-insensitive prefix check for "HARM" — covers "HARMFUL"
        // (the canonical filter) and any reasonable variant. Anything
        // else, including missing/empty, falls through to the buff path.
        if (filter != nullptr) {
            const char a = filter[0], b = filter[1], c = filter[2], d = filter[3];
            isHarmful = (a == 'H' || a == 'h') && (b == 'A' || b == 'a') &&
                        (c == 'R' || c == 'r') && (d == 'M' || d == 'm');
        }
    }

    // Drop the filter arg before calling — the existing
    // SetUnitBuff/SetUnitDebuff take (self, unit, index) only.
    Game::Lua::SetTop(L, 3);

    using Script_t = int(__fastcall *)(void *L);
    auto fn = reinterpret_cast<Script_t>(
        isHarmful ? Offsets::FUN_SCRIPT_GAMETOOLTIP_SET_UNIT_DEBUFF
                  : Offsets::FUN_SCRIPT_GAMETOOLTIP_SET_UNIT_BUFF);
    return fn(L);
}

// Vanilla 1.12 drops the unit-token string at the `Script_GameTooltip_SetUnit`
// boundary: the token is resolved to a 64-bit GUID via TokenToGUID, the GUID
// is passed to the inner `FUN_00529FE0` builder, and the original string is
// discarded. The builder writes the GUID into `[tooltip + 0x368]` / `+0x36C`
// and the shared `FUN_00530050` clear zeroes both at the start of every
// `SetX` call — same gating pattern HasSpell / HasItem use.
//
// Modern `GameTooltip:GetUnit()` returns `(name, unitToken)`, but the token
// can't be recovered exactly in vanilla — multiple unit tokens (`"target"`,
// `"focus"`, `"mouseover"` etc.) can point at the same GUID at any given
// moment, and the engine doesn't remember which one was used. The GUID is
// what addons actually need for cross-referencing (it threads through
// `UnitGUID`, the NameCache, item-link unique IDs, etc.); exposing it
// directly as `GetUnitGUID()` avoids inventing a faux token that wouldn't
// match the original Lua input anyway.

using ObjectGetName_t = const char *(__fastcall *)(void *obj, void *edx_unused, int *outFlags);

static const char *ResolveUnitName(uint32_t guidLo, uint32_t guidHi) {
    void *obj = Object::ByGuid(Offsets::TYPEMASK_UNIT,
                               (static_cast<uint64_t>(guidHi) << 32) | guidLo,
                               "GameTooltip:GetUnitGUID", 0x172);
    if (obj == nullptr)
        return nullptr;
    // `FUN_OBJECT_GET_NAME` is __thiscall — wire as __fastcall with the
    // ignored EDX slot. Returns a const char* (the canonical display
    // name for the object); falls back to engine `"UNKNOWNOBJECT"` /
    // `"Unknown Being"` literals when the unit's name isn't cached.
    auto getName = reinterpret_cast<ObjectGetName_t>(Offsets::FUN_OBJECT_GET_NAME);
    return getName(obj, nullptr, nullptr);
}

// `GameTooltip:GetUnitGUID()` → (name, guidString) for whichever unit
// the tooltip is currently displaying, or nothing if it isn't showing
// a unit. Return order matches modern's `GameTooltip:GetUnit()` shape
// (name first), so addons porting from `local name, unit = ttip:GetUnit()`
// can swap to `GetUnitGUID` and keep their existing destructuring.
// `name` is the unit's display name (may be `"UNKNOWNOBJECT"` for a
// remote unit whose info hasn't been queried yet — same fallback the
// engine uses internally). `guidString` is the canonical
// `"0xHHHHHHHHLLLLLLLL"` format matching `UnitGUID(unit)`.
static int __fastcall Script_GameTooltipGetUnitGUID(void *L) {
    if (Game::Lua::Type(L, 1) != Game::Lua::TYPE_TABLE) {
        Game::Lua::Error(L, "Usage: GameTooltip:GetUnitGUID()");
        return 0;
    }
    void *tooltipObj = Game::Lua::ResolveObject(L, 1);
    if (tooltipObj == nullptr)
        return 0;

    const auto *base = static_cast<const uint8_t *>(tooltipObj);
    const uint32_t guidLo = *reinterpret_cast<const uint32_t *>(
        base + Offsets::OFF_TOOLTIP_UNIT_GUID_LO);
    const uint32_t guidHi = *reinterpret_cast<const uint32_t *>(
        base + Offsets::OFF_TOOLTIP_UNIT_GUID_HI);
    if (guidLo == 0 && guidHi == 0)
        return 0;

    const char *name = ResolveUnitName(guidLo, guidHi);
    if (name != nullptr)
        Game::Lua::PushString(L, name);
    else
        Game::Lua::PushNil(L);

    const uint64_t guid = (static_cast<uint64_t>(guidHi) << 32) | guidLo;
    char buf[Guid::STRING_SIZE];
    Game::Lua::PushString(L, Guid::FormatAsString(guid, buf, sizeof buf));
    return 2;
}

// `GameTooltip:HasUnit()` — boolean companion to `GetUnitGUID`. Returns
// true iff the tooltip is currently showing a unit (i.e., the stored
// GUID is non-zero, which the shared tooltip-clear zeroes on every
// new `SetX` call).
static int __fastcall Script_GameTooltipHasUnit(void *L) {
    if (Game::Lua::Type(L, 1) != Game::Lua::TYPE_TABLE) {
        Game::Lua::Error(L, "Usage: GameTooltip:HasUnit()");
        return 0;
    }
    void *tooltipObj = Game::Lua::ResolveObject(L, 1);
    if (tooltipObj == nullptr) {
        Game::Lua::PushBool(L, false);
        return 1;
    }
    const auto *base = static_cast<const uint8_t *>(tooltipObj);
    const uint32_t guidLo = *reinterpret_cast<const uint32_t *>(
        base + Offsets::OFF_TOOLTIP_UNIT_GUID_LO);
    const uint32_t guidHi = *reinterpret_cast<const uint32_t *>(
        base + Offsets::OFF_TOOLTIP_UNIT_GUID_HI);
    Game::Lua::PushBool(L, guidLo != 0 || guidHi != 0);
    return 1;
}

static const Game::Lua::FrameMethodEntry g_methods[] = {
    {"SetUnitAura", &Script_GameTooltipSetUnitAura},
    {"GetUnitGUID", &Script_GameTooltipGetUnitGUID},
    {"HasUnit", &Script_GameTooltipHasUnit},
};

static void RegisterLuaFunctions() {
    Game::Lua::RegisterFrameMethods(
        reinterpret_cast<void *>(Offsets::VAR_GAMETOOLTIP_METHOD_REGISTRY),
        g_methods,
        static_cast<int>(sizeof(g_methods) / sizeof(g_methods[0])));
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace Unit::Tooltip
