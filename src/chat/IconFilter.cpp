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

// Inline-texture (`|T`, `|A`) chat anti-spoof.
//
// Vanilla neutralizes player-*typed* escapes in chat (a typed `|cff…|r`,
// `|Hitem…|h` or `|T…|t` shows as raw text) — only *trusted* text renders them
// (addon `AddMessage`, shift-clicked links). Our Text::InlineTexture backport
// breaks that parity for the two texture markers: it detects the neutralized
// `||T` / `||A` form and draws the art anyway, so a player could `/say`
// `|Tpath|t` and have a real icon appear in everyone's chat AND in the speech
// bubble over their head.
//
// This restores vanilla's behavior for both markers by defanging them in the
// chat path, while leaving `|c`/`|r`/`|H`/`|h` exactly as vanilla (already raw
// for typed input). It CANNOT be done in the text emitter: addon `|T` and player
// `|T` reach the emitter as byte-identical `||T` (vanilla doubles the
// unrecognized escape for both), so the only place they're distinguishable is
// the source — server-delivered chat flows through the chat dispatcher, addon
// `print` does not. Chat::Dispatch owns that hook and calls this on the message.
//
// Defang = replace the `|` of each opener with a space, breaking the pipe→letter
// adjacency the inline-texture detector keys on (`|T` and `||T` both contain the
// `|T` substring, so this catches the doubled form too). The payload text stays
// readable, the closer is inert without an opener, and every other escape is
// untouched.
//
// ANY new marker the emitter learns MUST be added here in the same change, or it
// becomes a chat-spoof vector.

#include "IconFilter.h"

namespace Chat::IconFilter {

namespace {

// True if `s` contains an inline-texture opener anywhere. Both markers count:
// `|T` names a texture path and `|A` names an atlas, and either one would render
// art from player-typed text if it survived to the fontstring.
bool IsIconOpener(const char *s) {
    return s[0] == '|' && (s[1] == 'T' || s[1] == 'A');
}

bool HasIconEscape(const char *s) {
    for (; *s != '\0'; ++s)
        if (IsIconOpener(s))
            return true;
    return false;
}

} // namespace

const char *Sanitize(const char *msg, char *buf, size_t bufSize) {
    if (msg == nullptr || !HasIconEscape(msg))
        return msg; // common case: forward untouched, no copy

    // NEVER write `msg` in place: several of the chat handler's ~50 callers pass
    // read-only `.rdata` literals (system notifications), so an in-place edit
    // would fault. We copy only when a `|T` is actually present.
    //
    // Defanging must not leave a LONE `|` followed by a non-escape char — that's
    // an invalid escape, which the chat frame tolerates but the speech-bubble
    // text setter chokes on, producing an empty bubble (verified in-game). By the
    // time chat reaches us the escape is usually the doubled `||T` (vanilla's own
    // neutralization of typed escapes), where `||` is a VALID literal-pipe escape.
    // So: keep the pipe(s) and blank the `T` when the pipe completes a `||`; blank
    // BOTH chars for a lone `|T`. Either way no dangling `|` remains, and no `|T`
    // opener survives for the inline-texture detector to fire on.
    size_t j = 0;
    for (size_t i = 0; msg[i] != '\0' && j + 1 < bufSize;) {
        if (IsIconOpener(msg + i)) {
            const bool doubledPipe = (i > 0 && msg[i - 1] == '|');
            buf[j++] = doubledPipe ? '|' : ' '; // keep pipe only if it completes `||`
            if (j + 1 < bufSize)
                buf[j++] = ' '; // blank the `T` / `A`
            i += 2;             // consumed the opener
        } else {
            buf[j++] = msg[i++];
        }
    }
    buf[j] = '\0';
    return buf;
}

} // namespace Chat::IconFilter
