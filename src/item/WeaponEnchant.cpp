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
#include "item/CGItem.h"
#include "item/Location.h"

#include <cstdint>

namespace Item::WeaponEnchant {

namespace {

constexpr int INVSLOT_MAINHAND = 16;
constexpr int INVSLOT_OFFHAND = 17;
constexpr int INVSLOT_RANGED = 18;

// ITEM_FIELD_ENCHANTMENT layout: 7 slots × 3 dwords each, starting
// at descriptor +0x40. Each slot is `{ id, duration, charges }`.
// Slot 0 is the PERMANENT enchant (Crusader, Mongoose, …);
// slot 1 is the TEMPORARY enchant (poisons, oils, sharpening
// stones, etc.) — what `GetWeaponEnchantInfo` measures.
constexpr int OFF_TEMP_ENCHANT_SLOT = 0x4C; // slot 1 base offset
constexpr int OFF_ENCHANT_ID = 0x0;
constexpr int OFF_ENCHANT_DURATION = 0x4;
constexpr int OFF_ENCHANT_CHARGES = 0x8;

struct EnchantInfo {
    bool has;
    uint32_t expirationMs;
    uint32_t charges;
    uint32_t enchantID;
};

// Reads the temp-enchant fields off the item at `paperdollSlot`.
// `has` is true iff an enchant is currently active (id non-zero AND
// non-expired) — matches modern `GetWeaponEnchantInfo`'s
// hasMainHandEnchant semantic. Returns a zeroed struct for empty
// slots, missing items, or missing descriptors.
EnchantInfo ReadTempEnchant(int paperdollSlot) {
    EnchantInfo r{false, 0, 0, 0};
    const uint8_t *item = Item::Location::ResolveEquipmentSlot(paperdollSlot);
    if (item == nullptr)
        return r;
    auto *desc = Item::ObjectFields(item);
    if (desc == nullptr)
        return r;
    auto *slot = reinterpret_cast<const uint32_t *>(desc + OFF_TEMP_ENCHANT_SLOT);
    r.enchantID = slot[OFF_ENCHANT_ID / 4];
    r.expirationMs = slot[OFF_ENCHANT_DURATION / 4];
    r.charges = slot[OFF_ENCHANT_CHARGES / 4];
    r.has = (r.enchantID != 0 && r.expirationMs > 0);
    return r;
}

// `C_Item.GetWeaponEnchantInfo()` — modern 12-tuple. Vanilla 1.12's
// global `GetWeaponEnchantInfo` returns 8 values without enchant
// IDs; this returns the modern signature for each weapon slot:
//
//   1.  hasMainHandEnchant     (bool)
//   2.  mainHandExpiration     (ms remaining)
//   3.  mainHandCharges        (int)
//   4.  mainHandEnchantID      (int — 0 if no temp enchant)
//   5.  hasOffHandEnchant
//   6.  offHandExpiration
//   7.  offHandCharges
//   8.  offHandEnchantID
//   9.  hasRangedEnchant
//   10. rangedExpiration
//   11. rangedCharges
//   12. rangedEnchantID
//
// Reads from CGItem descriptor +0x4C (the TEMPORARY enchantment
// slot — slot 1 in the ITEM_FIELD_ENCHANTMENT 7-slot array at
// +0x40). The PERMANENT enchant (Crusader, Mongoose, etc.) lives
// in slot 0 at +0x40 and isn't reported here — modern's
// `GetWeaponEnchantInfo` is specifically about the timed
// temp-enchant data, which is what this function returns.
int __fastcall Script_C_Item_GetWeaponEnchantInfo(void *L) {
    const EnchantInfo main = ReadTempEnchant(INVSLOT_MAINHAND);
    const EnchantInfo off = ReadTempEnchant(INVSLOT_OFFHAND);
    const EnchantInfo ranged = ReadTempEnchant(INVSLOT_RANGED);

    Game::Lua::PushBool(L, main.has);
    Game::Lua::PushNumber(L, static_cast<double>(main.expirationMs));
    Game::Lua::PushNumber(L, static_cast<double>(main.charges));
    Game::Lua::PushNumber(L, static_cast<double>(main.enchantID));

    Game::Lua::PushBool(L, off.has);
    Game::Lua::PushNumber(L, static_cast<double>(off.expirationMs));
    Game::Lua::PushNumber(L, static_cast<double>(off.charges));
    Game::Lua::PushNumber(L, static_cast<double>(off.enchantID));

    Game::Lua::PushBool(L, ranged.has);
    Game::Lua::PushNumber(L, static_cast<double>(ranged.expirationMs));
    Game::Lua::PushNumber(L, static_cast<double>(ranged.charges));
    Game::Lua::PushNumber(L, static_cast<double>(ranged.enchantID));

    return 12;
}

void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_Item", "GetWeaponEnchantInfo",
                                      &Script_C_Item_GetWeaponEnchantInfo);
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace

} // namespace Item::WeaponEnchant
