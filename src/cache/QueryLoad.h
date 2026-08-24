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

namespace Event::Custom {
struct AutoReserve;
}

// Shared async-load machinery for the client query caches that aren't
// item/quest — creature, gameobject, and (potentially) the other
// SMSG_*_QUERY caches. Modeled on `Item::Data`'s pending/timeout/event
// logic, generalized so several caches share one set of code.
//
// These caches are sibling classes: they share the record/entry hash-map
// shape but differ in `_GetRecord` and response-parser per class (the
// creature class reads its loaded flag at entry+0x50, the gameobject
// class at +0x98, etc.). So a module registers BOTH its cache's
// `_GetRecord` address and the dispatcher routes completion by cache
// instance. The dispatcher hooks each class's response parser (MinHook
// allows only one hook per target, but the parsers are distinct
// functions, so hooking creature's and gameobject's separately is fine);
// item/quest have their own parsers and aren't touched.
//
//   static const Event::Custom::AutoReserve _r{"CREATURE_DATA_LOAD_RESULT"};
//   ...
//   Cache::QueryLoad::Register(cacheInstance, &_r,
//                              Offsets::FUN_CREATURE_GET_RECORD);  // at init
//   Cache::QueryLoad::RequestLoad(cacheInstance, id);             // fire a load
//
// The completion event fires as `(id: number, success: bool)` — success
// when the cache fills, failure on timeout.

namespace Cache::QueryLoad {

// Register a query cache for async-load tracking. `cacheInstance` is the
// cache global (e.g. `Offsets::VAR_CREATURE_CACHE` cast to void*);
// `reserve` is the caller's `Event::Custom::AutoReserve` for the
// completion event, fired `(id, success)`; `getRecordAddr` is the cache
// class's `_GetRecord` (e.g. `Offsets::FUN_CREATURE_GET_RECORD`).
// `cacheInstance`/`reserve` must outlive the process (pass statics).
// Idempotent — calling again for the same cache is a no-op (safe across
// /reload).
void Register(void *cacheInstance, const Event::Custom::AutoReserve *reserve,
              uintptr_t getRecordAddr);

// True if `id` is already in the cache (pure peek, no network query).
bool IsCached(void *cacheInstance, uint32_t id);

// Issue an async load for `id`. If already cached, fires the completion
// event with success synchronously. Otherwise sends the query and tracks
// the id until the response lands (or it times out). Returns false only
// if the cache isn't registered or its pending set is full; true
// otherwise (already-cached, already-pending, or newly-tracked).
bool RequestLoad(void *cacheInstance, uint32_t id);

} // namespace Cache::QueryLoad
