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

// Turtle WoW server-side aura duration mods that the client can't hear.
// These mirror server mechanics that change a DoT's remaining time on another
// unit with no client-visible packet (verified in tortoise-wow: the edit goes
// through SetAuraDuration / RefreshHolder, which notifies only the aura's
// target-if-a-player, never the observing caster). We can't hear the change,
// but we see the triggering cast's SMSG_SPELL_GO, so Aura::Source mirrors the
// edit on the cached entry. A rule is a { exact trigger spellID -> affected
// aura by SpellFamilyName + family-flag overlap } pair, applied by
// Aura::Source::ApplyDurationModifiers.
//
//   Conflagrate  -> Immolate:    shave 3s   (Turtle keeps Immolate ticking, -3s;
//                                             stock's full-consume removes it, which
//                                             OnAuraRemoved evicts, so the rule is
//                                             harmless there)
//   Molten Blast -> Flame Shock: refresh    (RefreshHolder -> reset to full)
//
// Gated on Turtle::Detected() and registered once (a WorldTick latch, since the
// gate reads a FrameXML global only present in-world). Stock's Conflagrate
// still exists, so keeping the gate preserves the previous Lua behaviour
// exactly; Molten Blast's spell IDs don't exist off Turtle anyway.

#include "aura/Source.h"
#include "turtle/Detect.h"

#include "tick/WorldTick.h"

#include <cstdint>

namespace Turtle::DurationMods {

namespace {

// SpellFamilyName + family-flag bits of the affected auras (SpellClassMask):
// Immolate is warlock CF_WARLOCK_IMMOLATE (bit 2); Flame Shock is shaman
// CF_SHAMAN_FLAME_SHOCK (bit 28).
constexpr uint32_t kWarlock = 5;
constexpr uint32_t kShaman = 11;
constexpr uint64_t kImmolateFlag = 0x4;
constexpr uint64_t kFlameShockFlag = 0x10000000;

bool g_registered = false;

void RegisterAll() {
    constexpr uint32_t kConflagrate[] = {17962, 18930, 18931, 18932};
    for (uint32_t id : kConflagrate)
        Aura::Source::AddDurationMod(id, kWarlock, kImmolateFlag, /*icon*/ 0,
                                     Aura::Source::DURATION_MOD_REDUCE,
                                     /*valueMs*/ 3000);

    constexpr uint32_t kMoltenBlast[] = {36916, 36917, 36918,
                                         36919, 36920, 36921};
    for (uint32_t id : kMoltenBlast)
        Aura::Source::AddDurationMod(id, kShaman, kFlameShockFlag, /*icon*/ 0,
                                     Aura::Source::DURATION_MOD_REFRESH,
                                     /*valueMs*/ 0);
}

void OnTick() {
    if (g_registered || !Turtle::Detected())
        return;
    RegisterAll();
    g_registered = true;
}

const Tick::WorldTick::AutoSubscribe _tickSub{&OnTick};

} // namespace

} // namespace Turtle::DurationMods
