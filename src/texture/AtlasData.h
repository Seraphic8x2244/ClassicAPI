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

// Built-in atlas bindings, derived from `UiTextureAtlas` + `UiTextureAtlasMember`
// as shipped by Classic Era 1.15.9 (build 69547). Vanilla 1.12 has no atlas table
// at all, so a modern addon's `SetAtlas("name")` has nothing to resolve against;
// each row here binds one Blizzard atlas name to the 1.12 texture that carries the
// same art, plus the sub-rect within it.
//
// WHY THIS TABLE IS FIVE ROWS AND NOT FIVE THOUSAND
//
// The member table has 5,353 named regions. Almost none of them can be bound
// automatically, for three independent reasons established by measurement:
//
//   1. The sheets do not exist here. Resolving all 868 atlas sheet fileDataIDs
//      through the community listfile and testing every path against the client's
//      own art (7,640 files) gives ZERO hits — Classic Era repacked its art into
//      sheets 1.12 has never shipped.
//   2. Only the names carry over. Blizzard kept the original vanilla filename as
//      the atlas name when packing, so 16 member names match a 1.12 file basename.
//      That is the entire mechanical overlap.
//   3. A matching name does not prove matching art. Comparing the real BLP headers
//      against the member table's sprite sizes, only 5 of those 16 agree.
//      `ui-scrollbar-scrollupbutton-up` is 18x16 in the atlas while the 1.12 file
//      is 32x32 — and vanilla's own `UIPanelScrollBarButton` template draws it at
//      TexCoords 0.25,0.75,0.25,0.75, a CENTERED 16x16 sprite that is neither the
//      atlas's size nor a top-left crop. `macropopup-topleft` is 18x71 against a
//      256x256 file: a different asset that happens to share a name. So texcoords
//      cannot be derived from the two tables, and a wrong guess renders wrong art
//      rather than failing loudly.
//
// The five rows below are the ones where the atlas sprite size and the 1.12 file
// dimensions agree exactly, which makes the whole file the sprite and the texcoords
// a trivial 0..1. Everything else needs a human to pick the 1.12 equivalent and
// read its sub-rect out of the client's own FrameXML. That work is driven by the
// miss log (`_classicapi_DumpAtlasMisses`) rather than done speculatively, so the
// table only ever grows toward what real addons actually ask for.
//
// Two source-data traps, handled during extraction:
//   - 28 members have an EMPTY name and must be skipped.
//   - 73 names repeat case-insensitively; keep the higher ID, the same rule
//     `ui/ColorData.h` documents for duplicate color tags.
//
// `tilesHorizontally` / `tilesVertically` come from `CommittedFlags` bits 0 and 1.
// They are stored as resolved booleans rather than the raw flags word because the
// other set bits (4, 8, 16 appear in the data) have no known meaning and must not
// leak into the public contract.

namespace Texture::AtlasData {

struct Entry {
    const char *name;   // Blizzard's canonical casing, returned as `elementName`
    const char *file;   // 1.12 texture path, extension omitted as WoW expects
    int16_t width;      // sprite size, used by SetAtlas's `useAtlasSize`
    int16_t height;
    float left, right, top, bottom; // normalized sub-rect within `file`
    int32_t atlasID;                // Blizzard UiTextureAtlas.ID
    int32_t elementID;              // Blizzard UiTextureAtlasMember.UiTextureAtlasElementID
    bool tilesHorizontally;
    bool tilesVertically;
};

constexpr Entry kAtlases[] = {
    {"MinimapArrow", "Interface\\Minimap\\MinimapArrow", 32, 32, 0.0f, 1.0f, 0.0f, 1.0f, 647,
     9441, false, false},
    {"Repair", "Interface\\CURSOR\\Repair", 32, 32, 0.0f, 1.0f, 0.0f, 1.0f, 647, 9509, false,
     false},
    {"Rotating-MinimapArrow", "Interface\\Minimap\\ROTATING-MINIMAPARROW", 32, 32, 0.0f, 1.0f,
     0.0f, 1.0f, 647, 9442, false, false},
    {"Rotating-MinimapGroupArrow", "Interface\\Minimap\\Rotating-MinimapGroupArrow", 32, 32,
     0.0f, 1.0f, 0.0f, 1.0f, 647, 9443, false, false},
    {"Rotating-MinimapGuideArrow", "Interface\\Minimap\\ROTATING-MINIMAPGUIDEARROW", 32, 32,
     0.0f, 1.0f, 0.0f, 1.0f, 647, 9444, false, false},
};

constexpr int kAtlasCount = sizeof(kAtlases) / sizeof(kAtlases[0]);

} // namespace Texture::AtlasData
