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

#include "Range.h"

#include "Offsets.h"
#include "dbc/Lookup.h"
#include "unit/Position.h"

namespace Spell::Range {

namespace {

// Spell.dbc RangeIndex + SpellRange.dbc min/max offsets (verified,
// documented in CLAUDE.md under "Sub-DBC record layouts").
constexpr int OFF_SPELLRANGE_MIN = 0x04;
constexpr int OFF_SPELLRANGE_MAX = 0x08;

// FUN_SPELL_RANGE_CHECK: char __fastcall(caster, target, spellID, outFlag).
using SpellRangeCheck_t = char(__fastcall *)(void *caster, void *target,
                                             int spellID, char *outFlag);

} // namespace

bool SpellIDHasRange(int spellID) {
    if (spellID <= 0)
        return false;
    const uint8_t *rec = DBC::Record(Offsets::VAR_SPELL_RECORDS,
                                     Offsets::VAR_SPELL_RECORD_COUNT,
                                     static_cast<uint32_t>(spellID));
    if (rec == nullptr)
        return false;
    const uint32_t rangeIdx =
        *reinterpret_cast<const uint32_t *>(rec + Offsets::OFF_SPELL_RECORD_RANGE_INDEX);
    if (rangeIdx == 0)
        return false;
    const uint8_t *rangeRec = DBC::Record(Offsets::VAR_SPELL_RANGE_RECORDS,
                                          Offsets::VAR_SPELL_RANGE_COUNT, rangeIdx);
    if (rangeRec == nullptr)
        return false;
    const float minRange =
        *reinterpret_cast<const float *>(rangeRec + OFF_SPELLRANGE_MIN);
    const float maxRange =
        *reinterpret_cast<const float *>(rangeRec + OFF_SPELLRANGE_MAX);
    return minRange > 0.0f || maxRange > 0.0f;
}

int PlayerVsUnit(int spellID, const char *unitToken) {
    // No answer when the range check can't apply: unknown/rangeless spell,
    // or no unit token given.
    if (spellID <= 0 || unitToken == nullptr || !SpellIDHasRange(spellID))
        return -1;

    void *caster = Unit::Position::ResolveToken("player");
    void *target = Unit::Position::ResolveToken(unitToken);

    // Guard positions before handing the objects to the engine core:
    // FUN_SPELL_RANGE_CHECK dereferences each GetPosition result without a
    // null check, and a resolved-but-position-less object (or an absent
    // token → null) would otherwise crash. A failed read → no answer.
    float casterPos[3];
    float targetPos[3];
    if (!Unit::Position::Read(caster, casterPos) ||
        !Unit::Position::Read(target, targetPos))
        return -1;

    char minMaxFlag = 0;
    auto check =
        reinterpret_cast<SpellRangeCheck_t>(Offsets::FUN_SPELL_RANGE_CHECK);
    return check(caster, target, spellID, &minMaxFlag) != 0 ? 1 : 0;
}

} // namespace Spell::Range
