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

// `Texture:SetMask(path)` — apply an alpha-mask texture to a Texture region, the
// way the modern client does (a white-with-alpha mask clips the texture to the
// mask's shape). 1.12 has no generic masking, only the special-cased minimap
// disc (Minimap:SetMaskTexture). This backports it to any Texture.
//
// Mechanism (pinned by an in-game probe on the live GL backend — see the
// Offsets.h "Texture masking" block for the full findings): every textured
// region draws through the vertex-stream primitive FUN_0058a2a0, which is
// passed the region's corner array as its `positions`. We co-hook it; when the
// positions belong to a region that has a mask set, we bind the mask on texture
// UNIT 1 (its default MODULATE combiner multiplies the mask's alpha into the
// base's alpha) and re-run the engine's own submit with the second UV stream
// set to the base's UVs, so unit 1 samples the mask at the texture's own
// coordinates. The unit is enabled/disabled around the draw so the mask never
// leaks onto other textures. No projection, vertex, or state setup of our own —
// we ride the engine's real draw and add only unit 1.
//
// A single fs-level Lua method (`SetMask`) stores the mask per region. A cold
// co-hook on the region ctor clears a region's mask when its pooled memory is
// reused, so a recycled region can never inherit a stale mask. Kill switch
// `_classicapi_TextureMaskEnable`; an SEH latch trips it on a draw fault.

#include "Game.h"
#include "Offsets.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Texture::Mask {

namespace {

bool g_enabled = true;

// A region's mask: the loaded HTEXTURE plus the path (so a re-SetMask with the
// same path is a cheap no-op and the value is inspectable). Keyed by the
// CSimpleTexture object pointer — the same pointer the Lua object resolves to
// AND the base of the corner array the draw hook recovers.
struct MaskEntry {
    void *handle = nullptr; // HTEXTURE from the path loader (never null once set)
    std::string path;
};
std::unordered_map<void *, MaskEntry> g_regionMask;

// --- path -> HTEXTURE loader (mirrors FUN_00770200's SetTexture load) --------

using TexFlagsInit_t = void *(__thiscall *)(void *self, uint32_t blend, int, int, int, int, int,
                                            uint32_t, int);
using TextureLoad_t = uint32_t(__fastcall *)(const char *path, void *desc, uint32_t flags, int,
                                             int);
using GetRenderable_t = void *(__fastcall *)(void *hTex, int force, void *);

struct TexLoadDesc {
    void *vtbl;
    int32_t field4;
    void *self8;
    uint32_t fieldC;
    int32_t field10;
};

void *LoadByPath(const char *path) {
    TexLoadDesc desc;
    desc.vtbl = reinterpret_cast<void *>(Offsets::PTR_TEXLOAD_DESC_VTBL);
    desc.field4 = 8;
    desc.self8 = &desc.self8;
    desc.fieldC = reinterpret_cast<uintptr_t>(&desc.self8) | 1u;
    desc.field10 = 0;
    uint32_t flags = 0;
    const uint32_t blend = Game::Read<uint32_t>(Offsets::VAR_TEXTURE_BLEND_DEFAULT);
    reinterpret_cast<TexFlagsInit_t>(Offsets::FUN_GX_TEXFLAGS_INIT)(&flags, blend, 0, 0, 0, 0, 0,
                                                                    1, 0);
    return reinterpret_cast<void *>(reinterpret_cast<TextureLoad_t>(
        Offsets::FUN_TEXTURE_LOAD_BY_PATH)(path, &desc, flags, 0, 1));
}

// --- stream-primitive co-hook: apply the mask on unit 1 ----------------------

using GxRsSet_t = void(__fastcall *)(int selector, int value);
using GxRsSetPtr_t = void(__fastcall *)(int selector, const void *value);
using GxTexUnit_t = void(__fastcall *)(int unit);
using PrimStreams_t = void(__fastcall *)(int count, const void *pos, int posStride, const void *s3,
                                         int s3Stride, const void *colors, int colorStride,
                                         int drop8, int drop9, const void *uv0, int uv0Stride,
                                         const void *uv1, int uv1Stride);

PrimStreams_t g_primStreamsOriginal = nullptr;

// Runs the engine's own submit with the mask bound on unit 1 and uv1 = uv0.
// Returns true if it drew; false if the mask isn't resident yet (caller draws
// the base unmasked this frame and retries next). POD-only body for the SEH
// wrapper.
bool MaskedDrawImpl(void *maskH, int count, const void *pos, int posStride, const void *s3,
                    int s3Stride, const void *colors, int colorStride, int drop8, int drop9,
                    const void *uv0, int uv0Stride) {
    // Per-frame residency reference (force=1), exactly like the minimap's mask.
    void *maskCGx =
        reinterpret_cast<GetRenderable_t>(Offsets::FUN_TEXTURE_GET_RENDERABLE)(maskH, 1, nullptr);
    if (maskCGx == nullptr)
        return false;
    auto rs = reinterpret_cast<GxRsSet_t>(Offsets::FUN_GX_RS_SET);
    auto rsPtr = reinterpret_cast<GxRsSetPtr_t>(Offsets::FUN_GX_RS_SET_PTR);
    reinterpret_cast<GxTexUnit_t>(Offsets::FUN_GX_TEXUNIT_ENABLE)(1);
    rsPtr(Offsets::GXRS_TEXTURE0 + 1, maskCGx); // bind mask on unit 1
    // Force unit 1's combiner to MODULATE, exactly as the engine's own masked
    // draw FUN_004eae10 does (GxRs(0x20, 1)) — robust vs. relying on the
    // backend default.
    rs(Offsets::GXRS_COMBINER0 + 1, Offsets::GXRS_COMBINE_MODULATE);
    // uv1 = uv0 → unit 1 samples the mask at the base's texcoords.
    g_primStreamsOriginal(count, pos, posStride, s3, s3Stride, colors, colorStride, drop8, drop9,
                          uv0, uv0Stride, uv0, uv0Stride);
    reinterpret_cast<GxTexUnit_t>(Offsets::FUN_GX_TEXUNIT_DISABLE)(1); // no leak onto later draws
    return true;
}

bool SafeMaskedDraw(void *maskH, int count, const void *pos, int posStride, const void *s3,
                    int s3Stride, const void *colors, int colorStride, int drop8, int drop9,
                    const void *uv0, int uv0Stride) {
    __try {
        return MaskedDrawImpl(maskH, count, pos, posStride, s3, s3Stride, colors, colorStride,
                              drop8, drop9, uv0, uv0Stride);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_enabled = false; // disable the feature for the session
        return true;       // may have partly drawn — don't double-draw
    }
}

void __fastcall PrimStreams_h(int count, const void *pos, int posStride, const void *s3,
                              int s3Stride, const void *colors, int colorStride, int drop8,
                              int drop9, const void *uv0, int uv0Stride, const void *uv1,
                              int uv1Stride) {
    // Fast path: nothing masked, feature off, or not a plain textured quad.
    if (g_enabled && !g_regionMask.empty() && count == 4 && uv0 != nullptr && uv1 == nullptr &&
        pos != nullptr) {
        // The region draw passes the region's corner array as `positions`, so
        // the owning region is positions - OFF_SIMPLETEXTURE_CORNERS. A false
        // match needs a non-region draw whose positions pointer numerically
        // equals a masked region's corner array — an exact-pointer collision,
        // effectively impossible.
        void *region = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(pos) -
                                                Offsets::OFF_SIMPLETEXTURE_CORNERS);
        auto it = g_regionMask.find(region);
        if (it != g_regionMask.end() && it->second.handle != nullptr) {
            if (SafeMaskedDraw(it->second.handle, count, pos, posStride, s3, s3Stride, colors,
                               colorStride, drop8, drop9, uv0, uv0Stride))
                return;
        }
    }
    g_primStreamsOriginal(count, pos, posStride, s3, s3Stride, colors, colorStride, drop8, drop9,
                          uv0, uv0Stride, uv1, uv1Stride);
}

static const Game::HookAutoRegister _primHook{Offsets::FUN_GX_PRIM_STREAMS,
                                              reinterpret_cast<void *>(&PrimStreams_h),
                                              reinterpret_cast<void **>(&g_primStreamsOriginal)};

// --- region ctor co-hook: drop a stale mask on pooled reuse ------------------

// __thiscall(mem, parent, layer, sublayer) -> region; dummy-EDX __fastcall.
using Ctor_t = void *(__fastcall *)(void *mem, void *edx, void *parent, int layer, int sublayer);
Ctor_t g_ctorOriginal = nullptr;

void *__fastcall Ctor_h(void *mem, void *edx, void *parent, int layer, int sublayer) {
    if (mem != nullptr)
        g_regionMask.erase(mem); // a (re)constructed region owns no mask
    return g_ctorOriginal(mem, edx, parent, layer, sublayer);
}

static const Game::HookAutoRegister _ctorHook{Offsets::FUN_SIMPLETEXTURE_CTOR,
                                              reinterpret_cast<void *>(&Ctor_h),
                                              reinterpret_cast<void **>(&g_ctorOriginal)};

// --- Lua surface -------------------------------------------------------------

// texture:SetMask("path") sets the mask; SetMask(nil) / SetMask("") clears it.
int __fastcall Script_SetMask(void *L) {
    void *region = Game::Lua::ResolveObject(L, 1);
    if (region == nullptr)
        return 0;
    const char *path = (Game::Lua::GetTop(L) >= 2 && Game::Lua::Type(L, 2) == Game::Lua::TYPE_STRING)
                           ? Game::Lua::ToString(L, 2)
                           : nullptr;
    if (path == nullptr || path[0] == '\0') {
        g_regionMask.erase(region);
        return 0;
    }
    auto it = g_regionMask.find(region);
    if (it != g_regionMask.end() && it->second.path == path)
        return 0; // unchanged
    MaskEntry e;
    e.handle = LoadByPath(path); // never null (engine substitutes a fallback)
    e.path = path;
    g_regionMask[region] = std::move(e);
    return 0;
}

// _classicapi_TextureMaskEnable([on]) -> enabled. Kill switch (also what the
// SEH latch trips on a draw fault).
int __fastcall Script_TextureMaskEnable(void *L) {
    if (Game::Lua::GetTop(L) == 0)
        g_enabled = true;
    else
        g_enabled = Game::Lua::ToBoolean(L, 1) != 0;
    Game::Lua::PushBool(L, g_enabled);
    return 1;
}

const Game::Lua::FrameMethodEntry g_methods[] = {
    {"SetMask", &Script_SetMask},
};

void RegisterLuaFunctions() {
    Game::Lua::RegisterFrameMethods(
        reinterpret_cast<void *>(Offsets::VAR_TEXTURE_METHOD_REGISTRY), g_methods,
        static_cast<int>(sizeof(g_methods) / sizeof(g_methods[0])));
    Game::Lua::RegisterGlobalFunction("_classicapi_TextureMaskEnable", &Script_TextureMaskEnable);
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace

void PrepareForReload() {
    // /reload frees every region; forget the mask map so a recycled address
    // can't be mistaken for a still-masked region before its ctor re-clears it.
    g_regionMask.clear();
}

} // namespace Texture::Mask
