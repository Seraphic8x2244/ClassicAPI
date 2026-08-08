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

#include "object/ClosestByEntry.h"

#include "Game.h"
#include "Offsets.h"
#include "object/Resolve.h"
#include "unit/Position.h"

#include <cstdint>

namespace Object::ClosestByEntry {

namespace {

// `ClntObjMgrEnumVisibleObjects(cb, ctx)` — walks the visible-object
// table; the callback's dummy EDX slot absorbs the second __fastcall
// register so the trailing `uint64_t guid` lands where the engine puts it.
// Return non-zero to keep iterating.
using EnumCallback_t = int(__fastcall *)(void *ctx, void *unusedEdx,
                                         uint64_t guid);
using EnumVisibleObjects_t = int(__fastcall *)(EnumCallback_t cb, void *ctx);

struct ScanCtx {
    uint32_t typeMask;
    Guid::Type guidType;
    uint32_t wantEntry;
    float px, py, pz; // player position
    Result r;
};

int __fastcall ScanCallback(ScanCtx *ctx, void * /*unusedEdx*/, uint64_t guid) {
    // Type + entry checks are pure GUID bit-reads, so we skip the (costly)
    // object resolve for everything that can't match.
    if (Guid::Classify(guid) != ctx->guidType)
        return 1;
    const uint32_t entry = static_cast<uint32_t>((guid >> 24) & 0xFFFFFFu);
    if (entry != ctx->wantEntry)
        return 1;

    void *obj = Object::ByGuid(ctx->typeMask, guid, nullptr, 0);
    if (obj == nullptr)
        return 1;

    float pos[3];
    if (!Unit::Position::Read(obj, pos))
        return 1;

    const float dx = pos[0] - ctx->px;
    const float dy = pos[1] - ctx->py;
    const float dz = pos[2] - ctx->pz;
    const float distSq = dx * dx + dy * dy + dz * dz;
    if (!ctx->r.found || distSq < ctx->r.distSq) {
        ctx->r.found = true;
        ctx->r.x = pos[0];
        ctx->r.y = pos[1];
        ctx->r.distSq = distSq;
    }
    return 1;
}

} // namespace

Result Find(uint32_t typeMask, Guid::Type guidType, uint32_t entry) {
    const Result none{false, 0.0f, 0.0f, 0.0f};
    if (entry == 0)
        return none;

    // The enumerator derefs the object manager unconditionally; it's NULL
    // during glue / character-select. Bail rather than crash.
    if (*reinterpret_cast<void *volatile *>(Offsets::VAR_LOCAL_PLAYER_PTR) ==
        nullptr)
        return none;

    float ppos[3];
    if (!Unit::Position::Read(Unit::Position::ResolveToken("player"), ppos))
        return none;

    ScanCtx ctx{typeMask, guidType, entry, ppos[0], ppos[1], ppos[2], none};
    auto Enum = reinterpret_cast<EnumVisibleObjects_t>(
        Offsets::FUN_CLNT_OBJ_MGR_ENUM_VISIBLE_OBJECTS);
    Enum(reinterpret_cast<EnumCallback_t>(&ScanCallback), &ctx);
    return ctx.r;
}

} // namespace Object::ClosestByEntry
