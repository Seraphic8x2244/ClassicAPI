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

#pragma once

#include "Offsets.h"

#include <cstdint>

// Shared access to the client's friend list. The friend entries live inline
// in the social singleton (`VAR_SOCIAL_SYSTEM`, shared with the ignore list
// and /who): up to 50 entries from offset 0, stride 0x20. The field offsets
// are verified from GetNumFriends (FUN_005AD000), GetFriendInfo
// (FUN_005AD060), and the count walker FUN_005AE490.

namespace FriendList {

constexpr int FRIEND_ENTRY_STRIDE = 0x20;
constexpr int MAX_FRIENDS = 50;

constexpr int OFF_FRIEND_CONNECTED = 0x00; // u8  — 0 = offline
constexpr int OFF_FRIEND_STATUS = 0x01;    // u8  — bit 0x02 AFK, 0x04 DND
constexpr int OFF_FRIEND_NAME = 0x04;      // char*
constexpr int OFF_FRIEND_GUID = 0x08;      // u64
constexpr int OFF_FRIEND_LEVEL = 0x10;     // i32
constexpr int OFF_FRIEND_CLASS_INDEX = 0x14; // i32 → ChrClasses.dbc
constexpr int OFF_FRIEND_AREA_INDEX = 0x18;  // i32 → AreaTable.dbc

constexpr uint8_t FRIEND_STATUS_AFK = 0x02;
constexpr uint8_t FRIEND_STATUS_DND = 0x04;

// The social singleton, or null before login.
inline const uint8_t *Social() {
    return *reinterpret_cast<const uint8_t *const *>(
        static_cast<uintptr_t>(Offsets::VAR_SOCIAL_SYSTEM));
}

// Number of populated friend slots (what GetNumFriends returns).
inline int Count() {
    const uint8_t *social = Social();
    if (social == nullptr)
        return 0;
    using CountFn = uint32_t(__fastcall *)(const void *social);
    auto fn = reinterpret_cast<CountFn>(
        static_cast<uintptr_t>(Offsets::FUN_FRIEND_LIST_COUNT));
    return static_cast<int>(fn(social));
}

// Friend entry at 0-based `index`, or null if there is no list or the index
// is out of the fixed slot range. Does not bounds-check against the live
// count — callers that iterate should use `Count()`.
inline const uint8_t *EntryByIndex(int index) {
    const uint8_t *social = Social();
    if (social == nullptr || index < 0 || index >= MAX_FRIENDS)
        return nullptr;
    return social + index * FRIEND_ENTRY_STRIDE;
}

} // namespace FriendList
