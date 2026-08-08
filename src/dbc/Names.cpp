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

#include "Names.h"

#include "Lookup.h"
#include "Offsets.h"
#include "baselib/Ascii.h"

namespace DBC {

namespace {

// Reverse lookup shared by ClassIdForToken / RaceIdForToken: the id whose
// single-string field at `offset` matches `token` (case-insensitive), or 0.
uint32_t IdForStringField(uintptr_t recordsVar, uintptr_t countVar, int offset,
                          const char *token) {
    if (token == nullptr || *token == '\0')
        return 0;
    const int count = *reinterpret_cast<const int *>(countVar);
    const uint8_t *const *records =
        *reinterpret_cast<const uint8_t *const *const *>(recordsVar);
    if (records == nullptr)
        return 0;
    for (int i = 1; i <= count; ++i) {
        const uint8_t *rec = records[i];
        if (rec == nullptr)
            continue;
        const char *field =
            *reinterpret_cast<const char *const *>(rec + offset);
        if (field != nullptr && Ascii::EqualCI(field, token))
            return static_cast<uint32_t>(i);
    }
    return 0;
}

} // namespace

const char *ClassName(uint32_t classID) {
    if (classID == 0)
        return nullptr;
    return LocalizedField(Offsets::VAR_CHRCLASSES_RECORDS,
                          Offsets::VAR_CHRCLASSES_COUNT, classID,
                          Offsets::OFF_CHRCLASSES_NAMES);
}

const char *ClassToken(uint32_t classID) {
    if (classID == 0)
        return nullptr;
    return StringField(Offsets::VAR_CHRCLASSES_RECORDS,
                       Offsets::VAR_CHRCLASSES_COUNT, classID,
                       Offsets::OFF_CHRCLASSES_FILENAME);
}

const char *RaceName(uint32_t raceID) {
    if (raceID == 0)
        return nullptr;
    return LocalizedField(Offsets::VAR_CHRRACES_RECORDS,
                          Offsets::VAR_CHRRACES_COUNT, raceID,
                          Offsets::OFF_CHRRACES_NAMES);
}

const char *RaceToken(uint32_t raceID) {
    if (raceID == 0)
        return nullptr;
    return StringField(Offsets::VAR_CHRRACES_RECORDS,
                       Offsets::VAR_CHRRACES_COUNT, raceID,
                       Offsets::OFF_CHRRACES_FILENAME);
}

uint32_t ClassIdForToken(const char *token) {
    return IdForStringField(Offsets::VAR_CHRCLASSES_RECORDS,
                            Offsets::VAR_CHRCLASSES_COUNT,
                            Offsets::OFF_CHRCLASSES_FILENAME, token);
}

uint32_t RaceIdForToken(const char *token) {
    return IdForStringField(Offsets::VAR_CHRRACES_RECORDS,
                            Offsets::VAR_CHRRACES_COUNT,
                            Offsets::OFF_CHRRACES_FILENAME, token);
}

const char *AreaName(uint32_t areaID, bool resolveToParent) {
    if (areaID == 0)
        return nullptr;
    // Only the parent-resolving path needs the record; the direct read goes
    // straight through LocalizedField (which bounds-checks the id itself), so
    // the common !resolveToParent case is a single lookup.
    uint32_t use = areaID;
    if (resolveToParent) {
        const uint8_t *rec = Record(Offsets::VAR_AREATABLE_RECORDS,
                                    Offsets::VAR_AREATABLE_COUNT, areaID);
        if (rec != nullptr) {
            const uint32_t parent = *reinterpret_cast<const uint32_t *>(
                rec + Offsets::OFF_AREATABLE_PARENT_ID);
            if (parent != 0 &&
                Record(Offsets::VAR_AREATABLE_RECORDS,
                       Offsets::VAR_AREATABLE_COUNT, parent) != nullptr)
                use = parent;
        }
    }
    return LocalizedField(Offsets::VAR_AREATABLE_RECORDS,
                          Offsets::VAR_AREATABLE_COUNT, use,
                          Offsets::OFF_AREATABLE_NAMES);
}

} // namespace DBC
