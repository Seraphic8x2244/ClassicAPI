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
#include "spell/Lookup.h"

#include <cstdint>

namespace Spell::CastByID {


using NameToSlot_t = int(__fastcall *)(const char *name, void *out);
static NameToSlot_t NameToSlot_o = nullptr;

// Parse leading digits of `name` into an int. `*end` is set to the first
// non-digit. Caps at 100_000_000 (well above any plausible spellID) to
// avoid signed-int overflow on garbage input.
static int ParseLeadingInt(const char *name, const char **end) {
    int value = 0;
    while (*name >= '0' && *name <= '9') {
        if (value > 100000000) {
            value = 0;
            break;
        }
        value = value * 10 + (*name - '0');
        ++name;
    }
    *end = name;
    return value;
}

static const char *LocaleNameForID(int spellID) {
    const uint8_t *record = Spell::Lookup::RecordForID(spellID);
    if (record == nullptr)
        return nullptr;
    const int locale = Game::Read<int>(
        static_cast<uintptr_t>(Offsets::VAR_LOCALE_INDEX));
    return Game::Read<const char *>(record, Offsets::OFF_SPELL_NAMES + locale * 4);
}

// Hook on the single name → spellbook-slot resolver
// (`FUN_RESOLVE_SPELL_NAME_TO_SLOT`). When the input is a pure-numeric
// string, look the ID up in `Spell.dbc`, swap in the locale-resolved
// name, and call the original. Non-numeric input passes through
// unchanged. Same player-spellbook constraint as the original: spell
// IDs the player doesn't have in their spellbook still fail to resolve
// (the engine's downstream `[VAR_PLAYER_SPELLBOOK][slot]` deref returns
// 0 just like for unknown names).
//
// Covers two paths in one hook:
//   - `Script_CastSpellByName("5019")` at runtime → casts the spellID 5019
//     spell if known, the same way `CastSpellByName("Shoot")` does.
//   - Macro parser at `FUN_004EFE00` tagging a `/cast 5019` line — its
//     call to `FUN_004B3BC0` flows through us, so the macro gets
//     tagged with the resolved spellID in the spell-state cache.
static int __fastcall NameToSlot_h(const char *name, void *out) {
    if (name != nullptr) {
        const char *p = name;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p >= '0' && *p <= '9') {
            const char *afterDigits = nullptr;
            const int id = ParseLeadingInt(p, &afterDigits);
            const char *tail = afterDigits;
            while (*tail == ' ' || *tail == '\t')
                ++tail;
            // Only intercept if the entire input is digits (plus optional
            // surrounding whitespace). Inputs like "5019(Rank 1)" or
            // "5019foo" fall through to the original — the engine's name
            // resolver handles those as-is (and a numeric stem with a
            // `(Rank N)` suffix has no meaning anyway).
            if (id > 0 && *tail == '\0') {
                const char *resolvedName = LocaleNameForID(id);
                if (resolvedName != nullptr && *resolvedName != '\0') {
                    return NameToSlot_o(resolvedName, out);
                }
            }
        }
    }
    return NameToSlot_o(name, out);
}

static const Game::HookAutoRegister _hookreg{
    Offsets::FUN_RESOLVE_SPELL_NAME_TO_SLOT,
    reinterpret_cast<void *>(&NameToSlot_h),
    reinterpret_cast<void **>(&NameToSlot_o)};

} // namespace Spell::CastByID
