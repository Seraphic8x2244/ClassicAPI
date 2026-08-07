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

// `C_Spell.IsAutoAttackSpell` / `C_Spell.IsRangedAutoAttackSpell` and
// the spellbook-slot variants.
//
// Melee — there's only one spell that triggers melee swings (`6603` —
// "Auto Attack"), and no `Spell.dbc` attribute uniquely identifies it
// vs. on-next-swing abilities like Heroic Strike or Maul. We test
// against the engine constant directly.
//
// Ranged — the two vanilla ranged auto-attacks (`75` Auto Shot, `5019`
// Shoot wand) are exactly the spells carrying SPELL_ATTR_EX2_AUTOREPEAT_FLAG
// (AttributesEx2 bit 5, `0x20`). Verified from Spell.dbc: Auto Shot and
// Shoot have Ex2 `0x20`; the on-cast ranged abilities (Aimed Shot 19434,
// Multi-Shot 2643) do NOT — they only set the generic RANGED bit
// (Attributes bit 1), which is why an earlier RANGED test wrongly matched
// them. The AUTOREPEAT flag is the exact signal and extends to any future
// auto-repeating spell a private server might add.

#include "Lookup.h"

#include "Game.h"
#include "Offsets.h"
#include "dbc/Lookup.h"

#include <cstdint>
#include <cstring>

namespace Spell::AutoAttack {

namespace {

constexpr int SPELL_AUTO_ATTACK = 6603;

bool IsMelee(int spellID) {
    return spellID == SPELL_AUTO_ATTACK;
}

bool IsRanged(int spellID) {
    if (spellID <= 0)
        return false;
    const uint8_t *rec = DBC::Record(Offsets::VAR_SPELL_RECORDS,
                                     Offsets::VAR_SPELL_RECORD_COUNT,
                                     static_cast<uint32_t>(spellID));
    if (rec == nullptr)
        return false;
    const uint32_t attrEx2 = *reinterpret_cast<const uint32_t *>(
        rec + Offsets::OFF_SPELL_RECORD_ATTRIBUTES_EX2);
    return (attrEx2 & Offsets::SPELL_ATTR_EX2_AUTOREPEAT_FLAG) != 0;
}

int ReadSpellID(void *L) {
    if (!Game::Lua::IsNumber(L, 1))
        return 0;
    return static_cast<int>(Game::Lua::ToNumber(L, 1));
}

int ReadSpellBookSlotID(void *L) {
    if (!Game::Lua::IsNumber(L, 1))
        return 0;
    const int slot = static_cast<int>(Game::Lua::ToNumber(L, 1));
    int bookType = 0;
    if (Game::Lua::IsString(L, 2)) {
        const char *s = Game::Lua::ToString(L, 2);
        if (s != nullptr && _stricmp(s, "pet") == 0)
            bookType = 1;
    } else if (Game::Lua::IsNumber(L, 2)) {
        bookType = static_cast<int>(Game::Lua::ToNumber(L, 2));
    }
    return Spell::Lookup::SpellbookSlotToID(slot, bookType);
}

} // namespace

static int __fastcall Script_IsAutoAttackSpell(void *L) {
    Game::Lua::PushBool(L, IsMelee(ReadSpellID(L)));
    return 1;
}

static int __fastcall Script_IsRangedAutoAttackSpell(void *L) {
    Game::Lua::PushBool(L, IsRanged(ReadSpellID(L)));
    return 1;
}

static int __fastcall Script_IsAutoAttackSpellBookItem(void *L) {
    Game::Lua::PushBool(L, IsMelee(ReadSpellBookSlotID(L)));
    return 1;
}

static int __fastcall Script_IsRangedAutoAttackSpellBookItem(void *L) {
    Game::Lua::PushBool(L, IsRanged(ReadSpellBookSlotID(L)));
    return 1;
}

static void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_Spell", "IsAutoAttackSpell",
                                     &Script_IsAutoAttackSpell);
    Game::Lua::RegisterTableFunction("C_Spell", "IsRangedAutoAttackSpell",
                                     &Script_IsRangedAutoAttackSpell);
    Game::Lua::RegisterTableFunction("C_SpellBook", "IsAutoAttackSpellBookItem",
                                     &Script_IsAutoAttackSpellBookItem);
    Game::Lua::RegisterTableFunction("C_SpellBook", "IsRangedAutoAttackSpellBookItem",
                                     &Script_IsRangedAutoAttackSpellBookItem);
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace Spell::AutoAttack
