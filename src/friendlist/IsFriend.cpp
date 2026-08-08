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
// The friend list lives inline in the social singleton (`VAR_SOCIAL_SYSTEM`):
// up to 50 entries from offset 0, stride 0x20, each carrying a GUID (u64 at
// +0x08) and a name pointer (+0x04). Offsets verified from GetNumFriends /
// GetFriendInfo (name) and the count walker FUN_005AE490 (the +0x08 GUID is
// its populated-key). We match the input GUID against +0x08 directly — the
// same GUID `SMSG_FRIEND_LIST` populates for online AND offline friends — and
// also fall back to a name compare (resolving the GUID through the NameCache)
// so the answer holds even if a build stores the GUID differently. As a
// ClassicAPI convenience the arg may also be a bare character name, since
// vanilla's friends list is fundamentally name-keyed (AddFriend(name)).

#include "Game.h"
#include "Offsets.h"
#include "guid/Guid.h"
#include "unit/Identity.h"

#include <cstdint>
#include <cstring>

namespace FriendList::IsFriend {

namespace {

// Friend entry layout — read only here (single-file struct).
constexpr int OFF_ENTRY_NAME = 0x04; // char*
constexpr int OFF_ENTRY_GUID = 0x08; // u64
constexpr int ENTRY_STRIDE = 0x20;

using CountFn = uint32_t(__fastcall *)(const void *social);

const uint8_t *SocialSingleton() {
    return *reinterpret_cast<const uint8_t *const *>(
        static_cast<uintptr_t>(Offsets::VAR_SOCIAL_SYSTEM));
}

int FriendCount(const uint8_t *social) {
    auto fn = reinterpret_cast<CountFn>(
        static_cast<uintptr_t>(Offsets::FUN_FRIEND_LIST_COUNT));
    return static_cast<int>(fn(social));
}

// Resolve a player GUID to its cached character name, or nullptr if the GUID
// isn't in the NameCache (never seen this session).
const char *NameForGuid(uint64_t guid) {
    const uint8_t *rec = Unit::Identity::PlayerInfoRecord(guid);
    if (rec == nullptr)
        return nullptr;
    const char *name =
        reinterpret_cast<const char *>(rec + Offsets::OFF_PLAYER_INFO_NAME);
    return (name != nullptr && *name != '\0') ? name : nullptr;
}

bool IsFriendByGuidOrName(uint64_t guid, const char *name) {
    const uint8_t *social = SocialSingleton();
    if (social == nullptr)
        return false;
    const int count = FriendCount(social);
    for (int i = 0; i < count; ++i) {
        const uint8_t *entry = social + i * ENTRY_STRIDE;
        if (guid != 0 &&
            *reinterpret_cast<const uint64_t *>(entry + OFF_ENTRY_GUID) == guid)
            return true;
        if (name != nullptr) {
            const char *fname =
                *reinterpret_cast<const char *const *>(entry + OFF_ENTRY_NAME);
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
        name = NameForGuid(guid); // GUID input → name for the fallback compare
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
