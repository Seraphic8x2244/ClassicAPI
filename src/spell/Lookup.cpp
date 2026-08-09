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

#include "Lookup.h"

#include "Offsets.h"
#include "dbc/Lookup.h"

namespace Spell::Lookup {

const uint8_t *RecordForID(int spellID) {
    if (spellID <= 0)
        return nullptr;
    return DBC::Record(Offsets::VAR_SPELL_RECORDS,
                       Offsets::VAR_SPELL_RECORD_COUNT,
                       static_cast<uint32_t>(spellID));
}

bool IsFitToFamily(const uint8_t *spellRecord, uint32_t family,
                   uint64_t flagMask) {
    if (spellRecord == nullptr)
        return false;
    if (*reinterpret_cast<const uint32_t *>(
            spellRecord + Offsets::OFF_SPELL_RECORD_FAMILY_NAME) != family)
        return false;
    const uint64_t flags = *reinterpret_cast<const uint64_t *>(
        spellRecord + Offsets::OFF_SPELL_RECORD_FAMILY_FLAGS);
    return (flags & flagMask) != 0;
}

int SpellbookSlotToID(int slot1Based, int bookType) {
    const int slot = slot1Based - 1;
    if (slot < 0 || slot >= Offsets::SPELLBOOK_MAX_SLOTS)
        return 0;
    const auto base = (bookType == 1) ? Offsets::VAR_PET_SPELLBOOK
                                      : Offsets::VAR_PLAYER_SPELLBOOK;
    auto *array = reinterpret_cast<const int *>(static_cast<uintptr_t>(base));
    return array[slot];
}

int RecipeSlotSpellID(uintptr_t entriesVar, uintptr_t countVar, int slotIndex0) {
    if (slotIndex0 < 0)
        return 0;
    const int count = static_cast<int>(
        *reinterpret_cast<const uint32_t *>(countVar));
    if (slotIndex0 >= count)
        return 0;
    auto **entries = *reinterpret_cast<const uint8_t ***>(entriesVar);
    if (entries == nullptr)
        return 0;
    auto *entry = entries[slotIndex0];
    if (entry == nullptr)
        return 0;
    return static_cast<int>(*reinterpret_cast<const uint32_t *>(
        entry + Offsets::OFF_CRAFT_ENTRY_SPELL_ID));
}

int NthRecipeReagentItemID(const uint8_t *spellRecord, int reagentIndex1) {
    if (spellRecord == nullptr || reagentIndex1 < 1)
        return 0;
    auto *reagents = reinterpret_cast<const uint32_t *>(
        spellRecord + Offsets::OFF_SPELL_REAGENT_ID);
    int found = 0;
    for (int i = 0; i < Offsets::SPELL_RECIPE_MAX_REAGENTS; ++i) {
        const int itemID = static_cast<int>(reagents[i]);
        if (itemID == 0)
            return 0;
        if (++found == reagentIndex1)
            return itemID;
    }
    return 0;
}

int FindSpellbookSlot(int spellID, int *outBookType) {
    if (spellID <= 0)
        return 0;
    // Walk the full SPELLBOOK_MAX_SLOTS range rather than break-on-zero
    // — the BSS arrays are zero-padded past the populated count, so any
    // 0 entry is "empty slot" and won't match a positive spellID. Cost
    // is 1024 dword reads per book = 8KB of cache-friendly memory.
    auto *playerArray = reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_PLAYER_SPELLBOOK));
    for (int i = 0; i < Offsets::SPELLBOOK_MAX_SLOTS; i++) {
        if (playerArray[i] == spellID) {
            if (outBookType != nullptr)
                *outBookType = 0;
            return i + 1;
        }
    }
    auto *petArray = reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_PET_SPELLBOOK));
    for (int i = 0; i < Offsets::SPELLBOOK_MAX_SLOTS; i++) {
        if (petArray[i] == spellID) {
            if (outBookType != nullptr)
                *outBookType = 1;
            return i + 1;
        }
    }
    return 0;
}

} // namespace Spell::Lookup
