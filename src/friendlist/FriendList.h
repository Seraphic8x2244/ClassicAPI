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
//
// The ignore list lives in the same singleton: a flat array of u64 GUIDs at
// `+0x650`, stride 8, up to 25 entries, terminated by the first zero GUID.
// Vanilla ignores are GUID-keyed, not name-keyed — GetIgnoreName (0x005AD460)
// reads the GUID via FUN_005AE570 (`social + 0x650 + index*8`) and resolves it
// to a name through the NameCache, and GetNumIgnores (0x005AD400 → the count
// walker FUN_005AE550) stops at the first zero GUID and caps at 25.

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

constexpr int OFF_IGNORE_LIST = 0x650;   // u64 GUID array in the social singleton
constexpr int IGNORE_ENTRY_STRIDE = 0x8; // one packed u64 GUID per entry
constexpr int MAX_IGNORES = 25;          // 0x19 — GetNumIgnores' cap

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

// The ignore-list GUID at 0-based `index`, or 0 if there is no list or the
// index is past the fixed cap. Mirrors FUN_005AE570.
inline uint64_t IgnoreGuidByIndex(int index) {
    const uint8_t *social = Social();
    if (social == nullptr || index < 0 || index >= MAX_IGNORES)
        return 0;
    return *reinterpret_cast<const uint64_t *>(
        social + OFF_IGNORE_LIST + index * IGNORE_ENTRY_STRIDE);
}

// True if `guid` is on the ignore list. Walks the GUID array, stopping at the
// first zero entry (the list is contiguous — same terminate-on-zero contract
// GetNumIgnores relies on).
inline bool IsGuidIgnored(uint64_t guid) {
    if (guid == 0)
        return false;
    for (int i = 0; i < MAX_IGNORES; ++i) {
        const uint64_t entry = IgnoreGuidByIndex(i);
        if (entry == 0)
            break;
        if (entry == guid)
            return true;
    }
    return false;
}

} // namespace FriendList
