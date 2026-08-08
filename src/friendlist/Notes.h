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

#include <cstdint>

// Client-side friend notes. Vanilla 1.12 has no server-side note field
// (TBC added it), so we persist notes ourselves — the same pattern the
// equipment manager uses for its sets. Backing file:
//   WTF\Account\<acct>\<realm>\<char>\ClassicAPI_FriendNotes.txt
// Per-character, because the friends list itself is per-character in
// vanilla (each character keeps its own friends). GUID-keyed within that
// file — a friend's GUID is stable and unique on the realm.

namespace FriendList::Notes {

// Longest stored note. Longer input is truncated on Set.
constexpr int MAX_NOTE_LEN = 128;

// The note for a friend GUID, or nullptr if none. Loads the per-realm store
// on first use. The pointer is valid until the next Set or realm switch.
const char *Get(uint64_t guid);

// Set the note for a friend GUID, or clear it when `note` is null/empty.
// `name` is stored alongside so the file stays human-readable. Persists to
// disk immediately. Returns true if the store changed.
bool Set(uint64_t guid, const char *name, const char *note);

} // namespace FriendList::Notes
