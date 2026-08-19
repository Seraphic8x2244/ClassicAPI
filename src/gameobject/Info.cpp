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

// `C_GameObjectInfo.GetGameObjectInfoByID(gameObjectID)` -> table | nil
// `C_GameObjectInfo.RequestLoadGameObjectByID(gameObjectID)` -> bool
//
// The gameobject analog of `C_CreatureInfo`. Reads the client-side
// gameobject cache (gameobjectcache.wdb, fed by
// SMSG_GAMEOBJECT_QUERY_RESPONSE) by GO entry ID:
//
//   {
//     gameObjectID = <id>,
//     name         = "Mesa Elevator",
//     type         = 11,      -- GameObjectType (0=door,3=chest,…)
//     displayID    = 360,     -- model display ID
//   }
//
// `GetGameObjectInfoByID` is a synchronous peek (nil if not cached);
// `RequestLoadGameObjectByID` fetches an uncached GO and fires
// `GAMEOBJECT_DATA_LOAD_RESULT(gameObjectID, success)` when it lands.
//
// The GO cache is a sibling class to the creature cache (its own
// `_GetRecord` and response parser), but it rides the same shared
// `Cache::QueryLoad` dispatcher. Field offsets: name@+0x08 verified
// (FUN_0062dc10 reads `[block+0x08]`), type@+0x00 / displayID@+0x04 from
// the wire/WDB field order. See Offsets.h `OFF_GAMEOBJECT_*`.

#include "Game.h"
#include "Offsets.h"
#include "cache/QueryLoad.h"
#include "event/Custom.h"

#include <cstdint>

namespace GameObject::Info {

namespace {

constexpr const char *kGameObjectDataLoadResult = "GAMEOBJECT_DATA_LOAD_RESULT";
const Event::Custom::AutoReserve _reserveLoadResult{kGameObjectDataLoadResult};

using GetCacheRecord_t = const uint8_t *(__thiscall *)(void *cache, uint32_t id,
                                                       const uint64_t *guid, void *callback,
                                                       void *userData, char dedup);

// Peek the GO cache (no network query).
const uint8_t *PeekGameObject(uint32_t goID) {
    auto fn = reinterpret_cast<GetCacheRecord_t>(Offsets::FUN_GAMEOBJECT_GET_RECORD);
    auto *cache = reinterpret_cast<void *>(Offsets::VAR_GAMEOBJECT_CACHE);
    const uint64_t zeroGuid = 0;
    return fn(cache, goID, &zeroGuid, nullptr, nullptr, 0);
}

int __fastcall Script_GetGameObjectInfoByID(void *L) {
    if (!Game::Lua::IsNumber(L, 1))
        return 0; // nil
    const int goID = static_cast<int>(Game::Lua::ToNumber(L, 1));
    if (goID <= 0)
        return 0;

    const uint8_t *rec = PeekGameObject(static_cast<uint32_t>(goID));
    if (rec == nullptr)
        return 0; // not cached -> nil

    const uint32_t type = Game::Read<uint32_t>(rec, Offsets::OFF_GAMEOBJECT_TYPE);
    const uint32_t displayID =
        Game::Read<uint32_t>(rec, Offsets::OFF_GAMEOBJECT_DISPLAYID);
    const char *name = Game::Read<const char *>(rec, Offsets::OFF_GAMEOBJECT_NAME);

    Game::Lua::NewTable(L);
    Game::Lua::SetFieldNumber(L, "gameObjectID", goID);
    Game::Lua::SetFieldString(L, "name", name); // NULL coerced to ""
    Game::Lua::SetFieldNumber(L, "type", type);
    Game::Lua::SetFieldNumber(L, "displayID", displayID);
    return 1;
}

int __fastcall Script_RequestLoadGameObjectByID(void *L) {
    if (!Game::Lua::IsNumber(L, 1)) {
        Game::Lua::PushBool(L, false);
        return 1;
    }
    const int goID = static_cast<int>(Game::Lua::ToNumber(L, 1));
    if (goID <= 0) {
        Game::Lua::PushBool(L, false);
        return 1;
    }
    auto *cache = reinterpret_cast<void *>(Offsets::VAR_GAMEOBJECT_CACHE);
    const bool accepted = Cache::QueryLoad::RequestLoad(cache, static_cast<uint32_t>(goID));
    Game::Lua::PushBool(L, accepted);
    return 1;
}

void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_GameObjectInfo", "GetGameObjectInfoByID",
                                     &Script_GetGameObjectInfoByID);
    Game::Lua::RegisterTableFunction("C_GameObjectInfo", "RequestLoadGameObjectByID",
                                     &Script_RequestLoadGameObjectByID);
    Cache::QueryLoad::Register(reinterpret_cast<void *>(Offsets::VAR_GAMEOBJECT_CACHE),
                               kGameObjectDataLoadResult, Offsets::FUN_GAMEOBJECT_GET_RECORD);
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace

} // namespace GameObject::Info
