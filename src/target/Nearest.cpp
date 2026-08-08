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

// Backports the post-vanilla TargetScript selection functions that 1.12.1
// doesn't ship, built on the engine's own tab-targeting internals:
//
//   TargetNearest()                  — cycle nearest attackable/assistable unit
//   TargetNearestEnemyPlayer([rev])  — cycle nearest hostile player (PvP)
//   TargetNearestFriendPlayer([rev]) — cycle nearest friendly player
//   TargetDirectionEnemy(facing[,cone])  — nearest hostile in a facing cone
//   TargetDirectionFriend(facing[,cone]) — nearest friendly in a facing cone
//
// Vanilla already ships `TargetNearestEnemy/Friend/PartyMember/RaidMember`;
// they run through `FUN_00493f60(reverse, mode)` — enumerate visible units,
// keep those `FUN_00493e40(player, cand, mode)` accepts, sort by distance,
// cycle, and commit via `FUN_00493540(guidLo, guidHi)`. We reuse the two
// load-bearing pieces of that machinery directly:
//
//   * `FUN_00493e40` (Offsets::FUN_TARGET_CANDIDATE_VALID) — the exact
//     per-mode validity predicate (mode 1 = enemy: attackable + alive +
//     not-critter; mode 2 = friend: assistable + alive). Calling it gives
//     us the engine's real reaction/hostility semantics with zero
//     reimplementation.
//   * `FUN_00493540` (Offsets::FUN_SET_SELECTION_BY_GUID) — the canonical
//     setter the native cycle commits through (CMSG_SET_SELECTION + client
//     target + event 0xC3), which also maintains the current-selection
//     globals we read back for cycle continuity.
//
// So the only genuinely new logic here is (a) an extra player-GUID filter
// for the `*Player` variants and (b) the cone geometry for the direction
// variants — everything reaction-related defers to the engine.

#include "Game.h"
#include "Offsets.h"
#include "guid/Guid.h"
#include "object/Resolve.h"
#include "unit/Position.h"

#include <cmath>
#include <cstdint>

namespace Target::Nearest {

namespace {

// Candidate class each function selects from.
enum class Filter { Enemy, Friend, EnemyPlayer, FriendPlayer, Any };
constexpr int kFilterCount = 5;

// Default cone (full width, radians) when `coneAngle` is omitted — 90°, so
// 45° to either side of the facing. The engine's own default constant
// wasn't cleanly recoverable from the 3.3.5 reference binary; this is a
// deliberate, documented choice that matches the "roughly in front of me"
// intent of a direction-target keybind.
constexpr float kDefaultConeAngle = 1.5707963f; // pi/2

// Re-cycle window: repeated presses within this many ms advance through the
// frozen candidate snapshot (no ping-pong as distances shift); a longer gap
// rebuilds and re-targets the nearest. Mirrors the ~1s reset the native
// cycle uses.
constexpr uint32_t kCycleWindowMs = 1000;

constexpr int kMaxCandidates = 64;

// Engine entry points ─────────────────────────────────────────────────
using Predicate_t = int(__fastcall *)(void *player, void *cand, int mode);
using SetTargetByGuid_t = void(__fastcall *)(const uint64_t *guid);
using EnumCallback_t = int(__fastcall *)(void *ctx, void *unusedEdx, uint64_t guid);
using EnumVisibleObjects_t = int(__fastcall *)(EnumCallback_t cb, void *ctx);
using TickMs_t = uint32_t(__fastcall *)();

bool PassesFilter(void *player, void *cand, uint64_t guid, Filter f) {
    auto valid = reinterpret_cast<Predicate_t>(Offsets::FUN_TARGET_CANDIDATE_VALID);
    switch (f) {
    case Filter::Enemy:        return valid(player, cand, 1) != 0;
    case Filter::Friend:       return valid(player, cand, 2) != 0;
    case Filter::EnemyPlayer:  return Guid::IsPlayer(guid) && valid(player, cand, 1) != 0;
    case Filter::FriendPlayer: return Guid::IsPlayer(guid) && valid(player, cand, 2) != 0;
    case Filter::Any:          return valid(player, cand, 1) != 0 ||
                                      valid(player, cand, 2) != 0;
    }
    return false;
}

struct Candidate {
    uint64_t guid;
    float x, y;   // world XY (for the direction cone)
    float distSq; // 3D squared distance from the player (for nearest)
};

struct CollectCtx {
    void *player;
    Filter filter;
    float px, py, pz;
    Candidate list[kMaxCandidates];
    int count;
};

int __fastcall CollectCb(CollectCtx *ctx, void * /*edx*/, uint64_t guid) {
    if (ctx->count >= kMaxCandidates)
        return 1;
    // Players and creatures both resolve under TYPEMASK_UNIT (CGPlayer
    // derives from CGUnit); non-unit objects resolve to null and drop out.
    void *cand = Object::ByGuid(Offsets::TYPEMASK_UNIT, guid, nullptr, 0);
    if (cand == nullptr || cand == ctx->player)
        return 1;
    if (!PassesFilter(ctx->player, cand, guid, ctx->filter))
        return 1;

    float pos[3];
    if (!Unit::Position::Read(cand, pos))
        return 1;
    const float dx = pos[0] - ctx->px;
    const float dy = pos[1] - ctx->py;
    const float dz = pos[2] - ctx->pz;
    Candidate &c = ctx->list[ctx->count++];
    c.guid = guid;
    c.x = pos[0];
    c.y = pos[1];
    c.distSq = dx * dx + dy * dy + dz * dz;
    return 1;
}

// Enumerate visible units matching `f` into `out`. Returns false (empty)
// pre-world or on a player-position miss.
bool Collect(Filter f, CollectCtx *out) {
    out->count = 0;
    // The enumerator derefs the object manager unconditionally; null on the
    // glue / character-select screen.
    if (*reinterpret_cast<void *volatile *>(Offsets::VAR_LOCAL_PLAYER_PTR) ==
        nullptr)
        return false;
    void *player = Unit::Position::ResolveToken("player");
    float ppos[3];
    if (!Unit::Position::Read(player, ppos))
        return false;

    out->player = player;
    out->filter = f;
    out->px = ppos[0];
    out->py = ppos[1];
    out->pz = ppos[2];
    auto Enum = reinterpret_cast<EnumVisibleObjects_t>(
        Offsets::FUN_CLNT_OBJ_MGR_ENUM_VISIBLE_OBJECTS);
    Enum(reinterpret_cast<EnumCallback_t>(&CollectCb), out);
    return true;
}

// Commit the selection through the engine's `Script_TargetUnit` helper
// rather than the tab-cycle's raw `FUN_00493540`: this one first checks the
// GUID resolves to a live unit and bails otherwise, so a stale GUID left in
// a cycle snapshot (unit despawned / went out of range between presses)
// gets rejected instead of committed. Committing a dead GUID would leave
// `"target"` pointing at freed unit data and crash the next handler that
// reads it. `FUN_TARGET_BY_GUID` still routes through `FUN_00493540`
// internally, so the current-selection globals we read back are maintained.
void SetTarget(uint64_t guid) {
    reinterpret_cast<SetTargetByGuid_t>(Offsets::FUN_TARGET_BY_GUID)(&guid);
}

uint64_t CurrentSelection() {
    const uint32_t lo = *reinterpret_cast<volatile uint32_t *>(
        Offsets::VAR_CURRENT_SELECTION_GUID_LO);
    const uint32_t hi = *reinterpret_cast<volatile uint32_t *>(
        Offsets::VAR_CURRENT_SELECTION_GUID_HI);
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

uint32_t NowMs() {
    return reinterpret_cast<TickMs_t>(Offsets::FUN_OS_TICKCOUNT_MS)();
}

// Ascending insertion sort by 3D distance (candidate counts are tiny).
void SortByDistance(Candidate *a, int n) {
    for (int i = 1; i < n; ++i) {
        Candidate key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].distSq > key.distSq) {
            a[j + 1] = a[j];
            --j;
        }
        a[j + 1] = key;
    }
}

// Per-filter tab-cycle state. A frozen snapshot of GUIDs (sorted nearest→
// farthest) plus a cursor; presses within `kCycleWindowMs` that haven't had
// the target changed out from under them advance the cursor over the frozen
// list, so the order stays stable through a cycle burst.
struct CycleState {
    bool valid = false;
    uint64_t snapshot[kMaxCandidates];
    int count = 0;
    int index = 0;
    uint32_t lastPressMs = 0;
    uint64_t lastSetGuid = 0;
};
CycleState g_cycle[kFilterCount];

void DoCycle(void *L, Filter f) {
    const bool reverse = Game::Lua::ToBoolean(L, 1) != 0;
    CycleState &st = g_cycle[static_cast<int>(f)];
    const uint32_t now = NowMs();

    const bool cont = st.valid && st.count > 0 &&
                      (now - st.lastPressMs) <= kCycleWindowMs &&
                      CurrentSelection() == st.lastSetGuid;

    if (cont) {
        st.index = reverse ? (st.index + st.count - 1) % st.count
                           : (st.index + 1) % st.count;
    } else {
        CollectCtx c;
        if (!Collect(f, &c) || c.count == 0) {
            st.valid = false;
            return;
        }
        SortByDistance(c.list, c.count);
        st.count = c.count;
        for (int i = 0; i < c.count; ++i)
            st.snapshot[i] = c.list[i].guid;
        st.index = 0;
        st.valid = true;
    }

    const uint64_t guid = st.snapshot[st.index];
    SetTarget(guid);
    st.lastSetGuid = guid;
    st.lastPressMs = now;
}

void DoDirection(void *L, bool hostile, const char *usage) {
    if (!Game::Lua::IsNumber(L, 1)) {
        Game::Lua::Error(L, usage);
        return;
    }
    const float facing = static_cast<float>(Game::Lua::ToNumber(L, 1));
    float cone = Game::Lua::IsNumber(L, 2)
                     ? static_cast<float>(Game::Lua::ToNumber(L, 2))
                     : kDefaultConeAngle;
    if (cone <= 0.0f)
        cone = kDefaultConeAngle;
    const float cosHalf = std::cos(cone * 0.5f);
    const float dirX = std::cos(facing);
    const float dirY = std::sin(facing);

    CollectCtx c;
    if (!Collect(hostile ? Filter::Enemy : Filter::Friend, &c) || c.count == 0)
        return;

    int best = -1;
    float bestDistSq = 0.0f;
    for (int i = 0; i < c.count; ++i) {
        const float vx = c.list[i].x - c.px;
        const float vy = c.list[i].y - c.py;
        const float horizSq = vx * vx + vy * vy;
        if (horizSq <= 0.0001f)
            continue;
        // cos(angle between facing and the horizontal vector to the unit).
        const float cosAngle = (dirX * vx + dirY * vy) / std::sqrt(horizSq);
        if (cosAngle < cosHalf)
            continue; // outside the cone
        if (best < 0 || c.list[i].distSq < bestDistSq) {
            best = i;
            bestDistSq = c.list[i].distSq;
        }
    }
    if (best >= 0)
        SetTarget(c.list[best].guid);
}

// Lua entry points ─────────────────────────────────────────────────────
int __fastcall Script_TargetNearest(void *L) {
    DoCycle(L, Filter::Any);
    return 0;
}
int __fastcall Script_TargetNearestEnemyPlayer(void *L) {
    DoCycle(L, Filter::EnemyPlayer);
    return 0;
}
int __fastcall Script_TargetNearestFriendPlayer(void *L) {
    DoCycle(L, Filter::FriendPlayer);
    return 0;
}
int __fastcall Script_TargetDirectionEnemy(void *L) {
    DoDirection(L, /*hostile*/ true,
                "Usage: TargetDirectionEnemy(facing [, coneAngle])");
    return 0;
}
int __fastcall Script_TargetDirectionFriend(void *L) {
    DoDirection(L, /*hostile*/ false,
                "Usage: TargetDirectionFriend(facing [, coneAngle])");
    return 0;
}

void RegisterLuaFunctions() {
    Game::Lua::RegisterGlobalFunction("TargetNearest", &Script_TargetNearest);
    Game::Lua::RegisterGlobalFunction("TargetNearestEnemyPlayer",
                                      &Script_TargetNearestEnemyPlayer);
    Game::Lua::RegisterGlobalFunction("TargetNearestFriendPlayer",
                                      &Script_TargetNearestFriendPlayer);
    Game::Lua::RegisterGlobalFunction("TargetDirectionEnemy",
                                      &Script_TargetDirectionEnemy);
    Game::Lua::RegisterGlobalFunction("TargetDirectionFriend",
                                      &Script_TargetDirectionFriend);
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace

} // namespace Target::Nearest
