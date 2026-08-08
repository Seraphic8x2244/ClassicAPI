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

#include "Game.h"
#include "Offsets.h"
#include "event/Custom.h"
#include "item/Arg.h"
#include "item/CGItem.h"
#include "item/Data.h"
#include "item/Location.h"
#include "tick/WorldTick.h"

#include <cstdint>

namespace Item::Data {

using GetItemRecord_t = const uint8_t *(__thiscall *)(void *cache, uint32_t itemID,
                                                       const uint64_t *guid, void *callback,
                                                       void *userData, int unused);

// Two events fire on item-data arrival; they are **mutually exclusive**
// based on what triggered the cache fill:
//
//   `GET_ITEM_INFO_RECEIVED(itemID, success)` — added in 5.4. Fires for
//       **implicit / transparent** cache fills: `GetItemInfo` warmup,
//       hyperlink hover, chat-link resolution, our `SetItemByID` warmup,
//       etc. This is the event most existing addons listen to.
//
//   `ITEM_DATA_LOAD_RESULT(itemID, success)`  — added in 8.0. Fires for
//       **explicit** request paths only: `C_Item.RequestLoadItemData(ByID)`
//       and friends. The completion event for callers who explicitly
//       asked the modern Item-data API to load a specific item.
//
// Behavior verified empirically against 1.15.x: a single cache fill
// fires exactly one event, never both, depending on what initiated
// the request. We honor the split with two distinct callbacks.
//
// Names are reserved at static-init time via `Event::Custom::AutoReserve`;
// the `RebuildEventTable` hook appends them to the engine's input array
// at table-population time, so the engine itself allocates and tracks
// our entries identically to its own.
static constexpr const char *kItemDataLoadResult = "ITEM_DATA_LOAD_RESULT";
static constexpr const char *kGetItemInfoReceived = "GET_ITEM_INFO_RECEIVED";
static const Event::Custom::AutoReserve _reserveItemDataLoadResult{kItemDataLoadResult};
static const Event::Custom::AutoReserve _reserveGetItemInfoReceived{kGetItemInfoReceived};

static void FireGetItemInfoReceived(int itemID, bool success) {
    Event::Custom::FireIdSuccess(Event::Custom::Lookup(kGetItemInfoReceived),
                                 itemID, success);
}

static void FireItemDataLoadResult(int itemID, bool success) {
    Event::Custom::FireIdSuccess(Event::Custom::Lookup(kItemDataLoadResult),
                                 itemID, success);
}

// Engine callback for **implicit** cache fills (transparent warmup
// triggered by our `Script_GetItemInfo` hook or `SetItemByID`). Fires
// only the broad `GET_ITEM_INFO_RECEIVED` event — `ITEM_DATA_LOAD_RESULT`
// is reserved for explicit `RequestLoadItemData(ByID)` paths to match
// modern API semantics.
//
// Calling convention is __stdcall, 2 args / 8 bytes — matching the
// callback at 0x004FDC30 (used by 4 sites in the binary) which ends in
// `ret 8`. If the engine called us with a different convention, that
// callback would crash the engine, so stdcall is verified by induction.
static void __stdcall ItemLoadCallback_Implicit(void *userData, int success) {
    const auto itemID = static_cast<int>(reinterpret_cast<uintptr_t>(userData));
    FireGetItemInfoReceived(itemID, success != 0);
}

using ItemLoadCallback_t = void(__stdcall *)(void *userData, int success);

// Calls DBCache_ItemStats_C_GetRecord. With `callback=nullptr`,
// performs only the hash-table lookup; returns the cached record or
// nullptr. With a non-null callback, also kicks off the
// SMSG_ITEM_QUERY_SINGLE if the item isn't cached — the engine fills
// the cache asynchronously and invokes the supplied callback with the
// itemID smuggled through `userData` (the engine just stores and
// replays it without dereferencing).
//
// Disassembly of `_GetRecord` at 0x55BAAC shows it branches on the
// callback arg (stack slot +0x10), not on the 6th `bool` arg that
// VanillaHelpers documented as `requestIfMissing` — that slot is reused
// as a local immediately after entry, never read. The trigger for the
// request path is `callback != NULL`.
//
// The 6th arg IS used elsewhere though: when non-zero, the engine
// walks the entry's existing pending-callback list and skips the
// append if a node with matching `(callback, userData)` is already
// queued. We always pass `1` so that bursts of `WarmCache` calls for
// the same itemID (e.g. Bagnon enumerating the same slot repeatedly
// while the SMSG_ITEM_QUERY_SINGLE_RESPONSE is in flight) collapse to
// a single callback registration — otherwise the response fires our
// `GET_ITEM_INFO_RECEIVED` once per call.
static const uint8_t *CacheFetch(uint32_t itemID, ItemLoadCallback_t callback) {
    auto fn = reinterpret_cast<GetItemRecord_t>(Offsets::FUN_DBCACHE_ITEMSTATS_GET_RECORD);
    auto *cache = reinterpret_cast<void *>(Offsets::VAR_ITEMDB_CACHE);
    const uint64_t zeroGuid = 0;
    void *cb = reinterpret_cast<void *>(callback);
    void *userData =
        callback ? reinterpret_cast<void *>(static_cast<uintptr_t>(itemID)) : nullptr;
    return fn(cache, itemID, &zeroGuid, cb, userData, 1);
}

// Resolves an item-location table at stack idx to the itemID by walking
// `Location::Resolve` -> CGItem -> instance block (+0x08) -> itemID (+0x0C).
// Returns 0 if the slot is empty or the table is malformed.
static int ResolveLocationToItemID(void *L, int idx) {
    const uint8_t *item = Item::Location::Resolve(L, idx);
    if (item == nullptr)
        return 0;
    auto *instance = Item::InstanceBlock(item);
    if (instance == nullptr)
        return 0;
    return static_cast<int>(*reinterpret_cast<const uint32_t *>(
        instance + Offsets::OFF_INSTANCE_BLOCK_ITEM_ID));
}

static int __fastcall Script_IsItemDataCachedByID(void *L) {
    const int itemID = Item::Arg::ResolveItemID(L, 1);
    const bool cached =
        (itemID > 0) && (CacheFetch(static_cast<uint32_t>(itemID), nullptr) != nullptr);
    Game::Lua::PushBool(L, cached);
    return 1;
}

static int __fastcall Script_IsItemDataCached(void *L) {
    if (!Item::Location::IsLocationArg(L, 1)) {
        Game::Lua::Error(L, "Usage: C_Item.IsItemDataCached(itemLocation)");
        return 0;
    }
    const int itemID = ResolveLocationToItemID(L, 1);
    const bool cached =
        (itemID > 0) && (CacheFetch(static_cast<uint32_t>(itemID), nullptr) != nullptr);
    Game::Lua::PushBool(L, cached);
    return 1;
}

// Pending-request tracking. We deliberately do NOT eagerly create cache
// entries during the startup window — empirically (pfQuest), pre-creating
// the entry at addon-load / PLAYER_ENTERING_WORLD time, before the
// engine's network/opcode dispatch has settled, blocks the engine's
// natural prefetch from issuing a fresh `SMSG_ITEM_QUERY_SINGLE`. The
// entry stays permanently pending because nothing ever sends the query.
// This applies to BOTH paths into the cache:
//   - explicit `C_Item.RequestLoadItemData(ByID)`
//   - implicit `WarmCache` (the `GetItemInfo` hook + every `GetItem*`
//     getter's cache-miss warmup) — see `WarmCache`.
//
// Both route through `Track`, which records the item without touching
// the cache while `g_settled` is false. We hook the engine's item-cache
// response handler at `FUN_ITEMSTATS_CACHE_RESPONSE` (the function that
// fills cache entries on `SMSG_ITEM_QUERY_SINGLE_RESPONSE`); after the
// engine finishes filling entries and walking its own callback lists, we
// sweep our pending set and fire the per-item completion event for any
// tracked item that now resolves. The engine's natural prefetch (for
// player-inventory items) takes care of the actual query for us.
//
// For items the engine *doesn't* naturally prefetch (e.g. quest reward
// IDs the player has never owned), `OnWorldTick` escalates once the
// startup gate has settled by issuing our own `CacheFetch` with a no-op
// callback — that creates the entry and queues the query, and the
// response handler hook still fires our event when the data lands.
// `kMaxWaitTicks` is the hard timeout that gives up and fires the
// completion event with failure.
struct Pending {
    uint32_t itemID;
    int ticksWaiting;
    bool requestIssued;
    bool implicit; // true: fire GET_ITEM_INFO_RECEIVED; false: ITEM_DATA_LOAD_RESULT
    bool failed;   // set by the escalation callback on a server "no such item" reply
};

constexpr int kMaxPending = 512;
constexpr int kMaxWaitTicks = 1200;
// World ticks after which we assume the engine's startup
// network/opcode-dispatch state has settled and eager queries are safe.
// Also latched the moment the engine's own response handler fires (a
// response proves dispatch is live) — see `ItemStatsCacheResponse_h`.
// One-way latch; see the relog note in `WarmCache`.
constexpr int kSettleTicks = 60;

static Pending g_pending[kMaxPending];
static int g_pendingCount = 0;
static bool g_settled = false;
static int g_worldTicks = 0;

static int FindPending(uint32_t itemID) {
    for (int i = 0; i < g_pendingCount; ++i) {
        if (g_pending[i].itemID == itemID)
            return i;
    }
    return -1;
}

static void RemovePendingAt(int idx) {
    --g_pendingCount;
    if (idx != g_pendingCount)
        g_pending[idx] = g_pending[g_pendingCount];
}

// Callback for the escalation path. The engine queues the query when we
// pass a non-null callback to `_GetRecord`; it then invokes this with the
// itemID smuggled through `userData`. On a successful fill the engine has
// already set the entry's loaded flag, so the response-handler sweep
// reports success — we do nothing here. On a server failure (high-bit
// itemID in `SMSG_ITEM_QUERY_SINGLE_RESPONSE`) the engine does NOT set the
// loaded flag (verified in `FUN_0055BDB0`), so the sweep can't detect it;
// we mark the pending entry failed so the sweep / tick fires failure
// immediately instead of waiting out `kMaxWaitTicks`.
static void __stdcall ItemLoadCallback_Escalated(void *userData, int success) {
    if (success)
        return;
    const auto itemID = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(userData));
    const int idx = FindPending(itemID);
    if (idx >= 0)
        g_pending[idx].failed = true;
}

static void FireByTag(uint32_t itemID, bool success, bool implicit) {
    if (implicit)
        FireGetItemInfoReceived(static_cast<int>(itemID), success);
    else
        FireItemDataLoadResult(static_cast<int>(itemID), success);
}

// Records an item we owe a completion event for, WITHOUT eagerly
// creating the cache entry. During the startup window (before
// `g_settled`) this is the only safe way to "request" an item: calling
// `CacheFetch` with a callback now would create an orphan entry the
// engine's prefetch then skips, leaving it permanently pending (see the
// block comment above). `OnWorldTick` issues the actual query once
// settled, and the response-handler sweep fires the event — tagged
// implicit (GET_ITEM_INFO_RECEIVED) or explicit (ITEM_DATA_LOAD_RESULT).
// Returns false only when the request was dropped because the pending
// set is full (the caller is still tracking nothing). All other outcomes
// — already-loaded, already-tracking, newly-tracked — return true.
static bool Track(uint32_t itemID, bool implicit) {
    if (CacheFetch(itemID, nullptr) != nullptr) {
        // Already loaded. Explicit RequestLoadItemData fires its
        // completion event synchronously (modern API contract). Implicit
        // warmup mirrors the engine's already-loaded shortcut, which does
        // not re-invoke the callback — so a cache hit fires nothing.
        if (!implicit)
            FireItemDataLoadResult(static_cast<int>(itemID), true);
        return true;
    }
    const int idx = FindPending(itemID);
    if (idx >= 0) {
        // Already tracking. An explicit request supersedes a pending
        // implicit one so the explicit caller's ITEM_DATA_LOAD_RESULT is
        // honored (the two events are mutually exclusive per fill).
        if (!implicit)
            g_pending[idx].implicit = false;
        return true;
    }
    if (g_pendingCount >= kMaxPending)
        return false;
    g_pending[g_pendingCount].itemID = itemID;
    g_pending[g_pendingCount].ticksWaiting = 0;
    g_pending[g_pendingCount].requestIssued = false;
    g_pending[g_pendingCount].implicit = implicit;
    g_pending[g_pendingCount].failed = false;
    ++g_pendingCount;
    return true;
}

static bool RequestAndMaybeNotify(uint32_t itemID) { return Track(itemID, /*implicit=*/false); }

// Fires + removes a pending entry if it has resolved either way: loaded
// (engine set the flag) → success, or `failed` (escalation callback saw a
// server rejection) → failure. Returns true if it fired and removed the
// entry at `i`, so callers must NOT advance their index when it does.
static bool TryCompletePending(int i) {
    const uint32_t itemID = g_pending[i].itemID;
    const bool implicit = g_pending[i].implicit;
    if (CacheFetch(itemID, nullptr) != nullptr) {
        RemovePendingAt(i);
        FireByTag(itemID, true, implicit);
        return true;
    }
    if (g_pending[i].failed) {
        RemovePendingAt(i);
        FireByTag(itemID, false, implicit);
        return true;
    }
    return false;
}

// Response handler hook. After the engine fills cache entries from
// `SMSG_ITEM_QUERY_SINGLE_RESPONSE` and walks the per-entry callback
// lists, we sweep `g_pending` for anything now-loaded and fire the
// event. This is the engine-aligned alternative to per-tick polling:
// we observe fills exactly when they happen, with zero added latency
// and no risk of racing the engine's own callback walk.
//
// `__thiscall` is mimicked as `__fastcall(ecx, edx_unused, ...)` per
// the convention used elsewhere in this codebase (see
// `KeyringDescChange_h`, `FrameRegisterEvent_h`).
using ItemStatsCacheResponse_t = void(__thiscall *)(void *cache, void *packetReader, int flag);
static ItemStatsCacheResponse_t ItemStatsCacheResponse_o = nullptr;

static void __fastcall ItemStatsCacheResponse_h(void *cache, void * /*edx_unused*/,
                                                 void *packetReader, int flag) {
    using Trampoline_t = void(__fastcall *)(void *cache, void *edx, void *packetReader, int flag);
    reinterpret_cast<Trampoline_t>(ItemStatsCacheResponse_o)(cache, nullptr, packetReader, flag);
    // The engine just processed an item-query response, which proves its
    // network/opcode dispatch is live — eager queries are safe from here
    // on. Latch the startup gate so `WarmCache` stops deferring.
    g_settled = true;
    // Sweep pending — the engine just finished filling any matching
    // entries (and, on the failure branch, invoked our escalation
    // callback which marked any rejected items `failed`). Fire whatever
    // resolved this response, success or failure.
    for (int i = 0; i < g_pendingCount;) {
        if (TryCompletePending(i))
            continue;
        ++i;
    }
}

static const Game::HookAutoRegister _hookCacheResponse{
    Offsets::FUN_ITEMSTATS_CACHE_RESPONSE,
    reinterpret_cast<void *>(&ItemStatsCacheResponse_h),
    reinterpret_cast<void **>(&ItemStatsCacheResponse_o)};

// Escalation + timeout. The hook covers the happy path (engine fills
// cache → we fire). This tick is only responsible for:
//   1. Latching the startup gate via a tick-count fallback, in case no
//      engine response arrives early enough to latch it first.
//   2. Issuing a query for items the engine doesn't naturally prefetch
//      (e.g. quest rewards) — but only once `g_settled`, so we don't
//      race the engine's startup-time state machine. The engine sends
//      the SMSG, the response handler hook fires the event when the
//      response lands.
//   3. Firing failure immediately for items the server rejected (the
//      escalation callback marked them `failed`), and as a last resort
//      giving up after `kMaxWaitTicks` for items that never resolve at
//      all (lost query, server silent) — fire failure.
static void OnWorldTick() {
    if (!g_settled && ++g_worldTicks >= kSettleTicks)
        g_settled = true;
    for (int i = 0; i < g_pendingCount;) {
        if (!g_pending[i].requestIssued && g_settled) {
            CacheFetch(g_pending[i].itemID, &ItemLoadCallback_Escalated);
            g_pending[i].requestIssued = true;
        }
        // A server rejection (marked by the escalation callback) resolves
        // the item as failure without waiting out the timeout.
        if (TryCompletePending(i))
            continue;
        if (g_pending[i].ticksWaiting >= kMaxWaitTicks) {
            const uint32_t itemID = g_pending[i].itemID;
            const bool implicit = g_pending[i].implicit;
            RemovePendingAt(i);
            FireByTag(itemID, false, implicit);
            continue;
        }
        ++g_pending[i].ticksWaiting;
        ++i;
    }
}

static const Tick::WorldTick::AutoSubscribe _tickSub{&OnWorldTick};

// Public C++ API — see Item/Data.h. Implicit (transparent) warmup
// path used by the `Script_GetItemInfo` hook and `SetItemByID`. Fires
// the cache request only if the item isn't already cached; uses the
// implicit callback which fires only `GET_ITEM_INFO_RECEIVED` (not
// `ITEM_DATA_LOAD_RESULT`), matching modern semantics for ambient
// cache fills.
void WarmCache(uint32_t itemID) {
    // Post-settle (steady state): query immediately for lowest latency.
    // The engine invokes our implicit callback when the data lands.
    if (g_settled) {
        CacheFetch(itemID, &ItemLoadCallback_Implicit);
        return;
    }
    // Startup window: do NOT eagerly create the cache entry — that
    // orphans it (the engine's prefetch skips already-existing entries,
    // and our SMSG goes into a hole while dispatch isn't ready), leaving
    // the item permanently nil for the session. This is the exact race
    // that broke pfQuest/Questie scanning at PLAYER_ENTERING_WORLD. Defer
    // through the pending set instead; OnWorldTick issues the query once
    // settled and the response-handler sweep fires GET_ITEM_INFO_RECEIVED.
    //
    // NOTE: g_settled is a one-way latch per process. The race this
    // guards is the initial-login cold-WDB window. On relog the WDB is
    // already warm so cold-fetch pressure is minimal; we don't re-arm it.
    Track(itemID, /*implicit=*/true);
}

// Hook for `Script_GetItemInfo`. Pre-warms the cache from arg 1, then
// dispatches to the original. Effect: the original still returns nil
// for cache misses (vanilla behavior), but a query is now in flight,
// so subsequent calls will return valid data and
// GET_ITEM_INFO_RECEIVED will fire.
Script_GetItemInfo_t Script_GetItemInfo_o = nullptr;

int __fastcall Script_GetItemInfo_h(void *L) {
    const int itemID = Item::Arg::ResolveItemID(L, 1);
    if (itemID > 0)
        WarmCache(static_cast<uint32_t>(itemID));
    return Script_GetItemInfo_o(L);
}

static int __fastcall Script_RequestLoadItemDataByID(void *L) {
    const int itemID = Item::Arg::ResolveItemID(L, 1);
    if (itemID <= 0) {
        Game::Lua::PushBoolean(L, 0);
        return 1;
    }
    const bool accepted = RequestAndMaybeNotify(static_cast<uint32_t>(itemID));
    Game::Lua::PushBoolean(L, accepted ? 1 : 0);
    return 1;
}

static int __fastcall Script_RequestLoadItemData(void *L) {
    if (!Item::Location::IsLocationArg(L, 1)) {
        Game::Lua::Error(L, "Usage: C_Item.RequestLoadItemData(itemLocation)");
        return 0;
    }
    const int itemID = ResolveLocationToItemID(L, 1);
    if (itemID <= 0) {
        Game::Lua::PushBoolean(L, 0);
        return 1;
    }
    const bool accepted = RequestAndMaybeNotify(static_cast<uint32_t>(itemID));
    Game::Lua::PushBoolean(L, accepted ? 1 : 0);
    return 1;
}

static void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_Item", "IsItemDataCachedByID",
                                     &Script_IsItemDataCachedByID);
    Game::Lua::RegisterTableFunction("C_Item", "IsItemDataCached", &Script_IsItemDataCached);
    Game::Lua::RegisterTableFunction("C_Item", "RequestLoadItemDataByID",
                                     &Script_RequestLoadItemDataByID);
    Game::Lua::RegisterTableFunction("C_Item", "RequestLoadItemData", &Script_RequestLoadItemData);
    // Event names are reserved at file scope via `AutoReserve`; nothing
    // to do here for events.
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

// Auto-warm the item cache on `GetItemInfo(uncached_id)` calls, so
// subsequent calls return valid data and GET_ITEM_INFO_RECEIVED
// fires when the response arrives — matches modern WoW (5.x+)
// behavior. Without this, vanilla 1.12's `GetItemInfo` returns nil
// for misses and never fires a query, forcing addons to roll their
// own warmup hacks.
static const Game::HookAutoRegister _hookreg{
    Offsets::FUN_SCRIPT_GET_ITEM_INFO,
    reinterpret_cast<void *>(&Script_GetItemInfo_h),
    reinterpret_cast<void **>(&Script_GetItemInfo_o)};

} // namespace Item::Data
