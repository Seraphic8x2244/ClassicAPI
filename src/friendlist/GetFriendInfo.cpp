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

// `C_FriendList.GetFriendInfo(name)` / `GetFriendInfoByIndex(index)` — the
// modern FriendInfo table, plus `C_FriendList.GetNumFriends()`.
//
// Reads the friend entries directly (see `FriendList.h`). The class and zone
// names come from the same DBC lookups the engine's own GetFriendInfo uses:
// ChrClasses.dbc by the entry's class index, and AreaTable.dbc by the area
// index — resolved to the parent zone so a sub-area reports its zone. Both
// are locale-applied through `DBC::LocalizedField`.
//
// vanilla 1.12 stores no friend notes, so `notes` is omitted (nil). `mobile`
// and `referAFriend` are always false, and `rafLinkType` is 0 — vanilla has
// none of those systems; the fields exist so modern consumers do not nil-out.

#include "FriendList.h"

#include "Game.h"
#include "Offsets.h"
#include "dbc/Lookup.h"
#include "guid/Guid.h"

#include <cstdint>
#include <cstring>

namespace FriendList::Info {

namespace {

// Localized class name for the entry's ChrClasses index, or null if unknown.
const char *ClassName(uint32_t classIndex) {
    if (classIndex == 0)
        return nullptr;
    return DBC::LocalizedField(Offsets::VAR_CHRCLASSES_RECORDS,
                               Offsets::VAR_CHRCLASSES_COUNT, classIndex,
                               Offsets::OFF_CHRCLASSES_NAMES);
}

// Localized zone name for the entry's AreaTable index, resolved up to the
// parent zone (so "Goldshire" reports "Elwynn Forest"). Null if unknown.
const char *AreaName(uint32_t areaIndex) {
    if (areaIndex == 0)
        return nullptr;
    const uint8_t *rec = DBC::Record(Offsets::VAR_AREATABLE_RECORDS,
                                     Offsets::VAR_AREATABLE_COUNT, areaIndex);
    if (rec == nullptr)
        return nullptr;
    uint32_t use = areaIndex;
    const uint32_t parent = *reinterpret_cast<const uint32_t *>(
        rec + Offsets::OFF_AREATABLE_PARENT_ID);
    if (parent != 0 &&
        DBC::Record(Offsets::VAR_AREATABLE_RECORDS, Offsets::VAR_AREATABLE_COUNT,
                    parent) != nullptr)
        use = parent;
    return DBC::LocalizedField(Offsets::VAR_AREATABLE_RECORDS,
                               Offsets::VAR_AREATABLE_COUNT, use,
                               Offsets::OFF_AREATABLE_NAMES);
}

// Push the modern FriendInfo table for `entry` on top of the Lua stack.
void PushFriendInfo(void *L, const uint8_t *entry) {
    const bool connected = *(entry + OFF_FRIEND_CONNECTED) != 0;
    const char *name =
        *reinterpret_cast<const char *const *>(entry + OFF_FRIEND_NAME);
    const int level = *reinterpret_cast<const int *>(entry + OFF_FRIEND_LEVEL);
    const uint64_t guid =
        *reinterpret_cast<const uint64_t *>(entry + OFF_FRIEND_GUID);
    const uint8_t status = *(entry + OFF_FRIEND_STATUS);
    const uint32_t classIndex =
        *reinterpret_cast<const uint32_t *>(entry + OFF_FRIEND_CLASS_INDEX);
    const uint32_t areaIndex =
        *reinterpret_cast<const uint32_t *>(entry + OFF_FRIEND_AREA_INDEX);

    Game::Lua::NewTable(L);
    Game::Lua::SetFieldString(L, "name", name);
    Game::Lua::SetFieldBool(L, "connected", connected);
    Game::Lua::SetFieldNumber(L, "level", static_cast<double>(level));
    Game::Lua::SetFieldString(L, "className", ClassName(classIndex));
    Game::Lua::SetFieldString(L, "area", AreaName(areaIndex));
    if (guid != 0) {
        char buf[Guid::STRING_SIZE];
        Game::Lua::SetFieldString(L, "guid",
                                  Guid::FormatAsString(guid, buf, sizeof buf));
    }
    Game::Lua::SetFieldBool(L, "afk", (status & FRIEND_STATUS_AFK) != 0);
    Game::Lua::SetFieldBool(L, "dnd", (status & FRIEND_STATUS_DND) != 0);
    Game::Lua::SetFieldBool(L, "mobile", false);
    Game::Lua::SetFieldBool(L, "referAFriend", false);
    Game::Lua::SetFieldNumber(L, "rafLinkType", 0);
}

int __fastcall Script_GetNumFriends(void *L) {
    Game::Lua::PushNumber(L, static_cast<double>(FriendList::Count()));
    return 1;
}

int __fastcall Script_GetFriendInfoByIndex(void *L) {
    if (!Game::Lua::IsNumber(L, 1)) {
        Game::Lua::PushNil(L);
        return 1;
    }
    const int index = static_cast<int>(Game::Lua::ToNumber(L, 1)); // 1-based
    if (index < 1 || index > FriendList::Count()) {
        Game::Lua::PushNil(L);
        return 1;
    }
    const uint8_t *entry = FriendList::EntryByIndex(index - 1);
    if (entry == nullptr) {
        Game::Lua::PushNil(L);
        return 1;
    }
    PushFriendInfo(L, entry);
    return 1;
}

int __fastcall Script_GetFriendInfo(void *L) {
    const char *name = Game::Lua::IsString(L, 1) ? Game::Lua::ToString(L, 1) : nullptr;
    if (name == nullptr || *name == '\0') {
        Game::Lua::PushNil(L);
        return 1;
    }
    const int count = FriendList::Count();
    for (int i = 0; i < count; ++i) {
        const uint8_t *entry = FriendList::EntryByIndex(i);
        if (entry == nullptr)
            break;
        const char *fname =
            *reinterpret_cast<const char *const *>(entry + OFF_FRIEND_NAME);
        if (fname != nullptr && _stricmp(fname, name) == 0) {
            PushFriendInfo(L, entry);
            return 1;
        }
    }
    Game::Lua::PushNil(L);
    return 1;
}

void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_FriendList", "GetNumFriends",
                                     &Script_GetNumFriends);
    Game::Lua::RegisterTableFunction("C_FriendList", "GetFriendInfoByIndex",
                                     &Script_GetFriendInfoByIndex);
    Game::Lua::RegisterTableFunction("C_FriendList", "GetFriendInfo",
                                     &Script_GetFriendInfo);
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace

} // namespace FriendList::Info
