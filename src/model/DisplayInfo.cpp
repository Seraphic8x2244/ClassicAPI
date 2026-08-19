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

// `Model:SetDisplayInfo(creatureDisplayID)` — backport of the modern Model
// method that points a Model frame at a creature by its display ID, instead of
// requiring a `.mdx` file path (`Model:SetModel`). Vanilla already loads models
// by path; this just prepends the DBC resolution modern clients do internally:
//
//   creatureDisplayID → CreatureDisplayInfo[id] +0x04 ModelID
//                     → CreatureModelData[ModelID] +0x08 ModelPath
//                     → Model:SetModel's worker (vtable +0x94)
//                     → apply the display's TextureVariation skins
//
// The load path is the engine's own (`Script_SetModel` FUN_0076d950 calls the
// same vtable +0x94 worker); we add the display-ID front end and the creature
// skin. It is the foundational "display ID → loaded model" step — a rendered
// portrait-by-display-ID (SetPortraitTextureFromCreatureDisplayID) would build
// render-to-texture on top of this, but a live Model frame needs neither.

#include "Game.h"
#include "Offsets.h"
#include "dbc/Lookup.h"

#include <cstdint>
#include <string>

namespace Model::DisplayInfo {
namespace {

// The CFrameScriptObject "is-a" vmethod, at vtable +0x10 for every frame type,
// and the Model model-load worker at vtable +0x94 (the one Script_SetModel
// invokes after resolving self). Both single-use here, so kept local.
constexpr int kVmtIsA = 0x10;
constexpr int kVmtLoadModel = 0x94;

using IsA_t = char(__thiscall *)(void *self, int typeId);
using LoadModel_t = void(__thiscall *)(void *self, const char *path);
using SetReplaceableTexture_t = void(__thiscall *)(void *model, int type, const char *path);

template <typename Fn>
Fn Vmethod(void *obj, int byteOffset) {
    auto *vtbl = *reinterpret_cast<uint8_t **>(obj);
    return *reinterpret_cast<Fn *>(vtbl + byteOffset);
}

// The Model frame-script type id. The engine assigns it lazily on the first
// Model method call; mirror that assignment (FUN_0076d950's prologue) so a
// type-check is valid even if no stock Model method has run yet.
int ModelTypeId() {
    auto &typeId = *reinterpret_cast<int *>(
        static_cast<uintptr_t>(Offsets::VAR_MODEL_LUA_TYPE_ID));
    if (typeId == 0) {
        auto &counter = *reinterpret_cast<int *>(
            static_cast<uintptr_t>(Offsets::VAR_FRAMESCRIPT_TYPE_ID_COUNTER));
        counter += 1;
        typeId = counter;
    }
    return typeId;
}

// Resolve arg1 to a Model object, or nullptr if it is not a Model. Mirrors the
// resolve-then-typecheck prologue every Model method uses: generic object
// resolve, then the IsA vmethod. The type-check matters here because we call a
// Model vtable slot directly — a non-Model region would dispatch a wrong method.
void *ResolveModel(void *L) {
    void *obj = Game::Lua::ResolveObject(L, 1);
    if (obj == nullptr)
        return nullptr;
    if (!Vmethod<IsA_t>(obj, kVmtIsA)(obj, ModelTypeId()))
        return nullptr;
    return obj;
}

// Apply a creature display's TextureVariation skins to the just-loaded model.
// The variation columns are bare filenames that live in the model's own
// directory; they fill the model's MONSTER_1/2/3 replaceable-texture slots
// (M2/MDX texture types 11/12/13). FUN_0076cfe0 no-ops when no model is loaded,
// so this is safe to call unconditionally after the load.
void ApplySkins(void *model, const char *modelPath, const uint8_t *displayRec) {
    std::string dir(modelPath);
    const auto slash = dir.find_last_of("\\/");
    dir = (slash == std::string::npos) ? std::string() : dir.substr(0, slash + 1);

    auto setTexture = reinterpret_cast<SetReplaceableTexture_t>(
        Offsets::FUN_MODEL_SET_REPLACEABLE_TEXTURE);
    constexpr int kMonsterSlot[3] = {11, 12, 13};
    for (int i = 0; i < 3; ++i) {
        const char *variation = *reinterpret_cast<const char *const *>(
            displayRec + Offsets::OFF_CREATUREDISPLAYINFO_TEXTURE_VARIATION + i * 4);
        if (variation == nullptr || variation[0] == '\0')
            continue;
        const std::string skin = dir + variation;
        setTexture(model, kMonsterSlot[i], skin.c_str());
    }
}

int __fastcall Script_SetDisplayInfo(void *L) {
    if (Game::Lua::Type(L, 1) != Game::Lua::TYPE_TABLE || !Game::Lua::IsNumber(L, 2)) {
        Game::Lua::Error(L, "Usage: Model:SetDisplayInfo(creatureDisplayID)");
        return 0;
    }
    void *model = ResolveModel(L);
    if (model == nullptr) {
        Game::Lua::Error(L, "Model:SetDisplayInfo(): self is not a Model");
        return 0;
    }

    const int displayID = static_cast<int>(Game::Lua::ToNumber(L, 2));
    if (displayID <= 0)
        return 0;

    const uint8_t *displayRec = DBC::Record(Offsets::VAR_CREATUREDISPLAYINFO_RECORDS,
                                            Offsets::VAR_CREATUREDISPLAYINFO_COUNT,
                                            static_cast<uint32_t>(displayID));
    if (displayRec == nullptr)
        return 0;
    const uint32_t modelID = *reinterpret_cast<const uint32_t *>(
        displayRec + Offsets::OFF_CREATUREDISPLAYINFO_MODEL_ID);

    const char *modelPath = DBC::StringField(Offsets::VAR_CREATUREMODELDATA_RECORDS,
                                             Offsets::VAR_CREATUREMODELDATA_COUNT,
                                             modelID,
                                             Offsets::OFF_CREATUREMODELDATA_MODEL_PATH);
    if (modelPath == nullptr)
        return 0;

    Vmethod<LoadModel_t>(model, kVmtLoadModel)(model, modelPath);
    ApplySkins(model, modelPath, displayRec);
    return 0;
}

const Game::Lua::FrameMethodEntry g_methods[] = {
    {"SetDisplayInfo", &Script_SetDisplayInfo},
};

void RegisterLuaFunctions() {
    Game::Lua::RegisterFrameMethods(
        reinterpret_cast<void *>(Offsets::VAR_MODEL_METHOD_REGISTRY),
        g_methods,
        static_cast<int>(sizeof(g_methods) / sizeof(g_methods[0])));
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace
} // namespace Model::DisplayInfo
