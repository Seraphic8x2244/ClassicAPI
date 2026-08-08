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

// `C_FriendList.IsFriend(guid)` — is the given player on the friends list?
//
// Matches the input GUID against each friend entry's GUID (+0x08) — the same
// GUID `SMSG_FRIEND_LIST` populates for online AND offline friends — and also
// falls back to a name compare (resolving the GUID through the NameCache) so
// the answer holds even if a build stores the GUID differently. As a
// ClassicAPI convenience the arg may also be a bare character name, since
// vanilla's friends list is fundamentally name-keyed (AddFriend(name)). See
// `FriendList.h` for the entry layout.

#include "FriendList.h"

#include "Game.h"
#include "guid/Guid.h"
#include "unit/Identity.h"

#include <cstdint>
#include <cstring>

namespace FriendList::IsFriend {

namespace {

bool IsFriendByGuidOrName(uint64_t guid, const char *name) {
    const int count = FriendList::Count();
    for (int i = 0; i < count; ++i) {
        const uint8_t *entry = FriendList::EntryByIndex(i);
        if (entry == nullptr)
            break;
        if (guid != 0 &&
            *reinterpret_cast<const uint64_t *>(entry + FriendList::OFF_FRIEND_GUID) == guid)
            return true;
        if (name != nullptr) {
            const char *fname = *reinterpret_cast<const char *const *>(
                entry + FriendList::OFF_FRIEND_NAME);
            if (fname != nullptr && _stricmp(fname, name) == 0)
                return true;
        }
    }
    return false;
}

} // namespace

int __fastcall Script_IsFriend(void *L) {
    const char *arg = Game::Lua::IsString(L, 1) ? Game::Lua::ToString(L, 1) : nullptr;
    if (arg == nullptr || *arg == '\0') {
        Game::Lua::PushBool(L, false);
        return 1;
    }

    uint64_t guid = 0;
    const char *name = nullptr;
    if (Guid::Parse(arg, &guid) && guid != 0)
        name = Unit::Identity::NameForGuid(guid); // GUID input → name for the fallback compare
    else
        name = arg; // not a GUID → treat the string as a character name

    Game::Lua::PushBool(L, IsFriendByGuidOrName(guid, name));
    return 1;
}

void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_FriendList", "IsFriend", &Script_IsFriend);
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace FriendList::IsFriend
