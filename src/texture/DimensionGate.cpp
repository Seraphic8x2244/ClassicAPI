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

// Texture dimension gates — make 1.12's allocator enforce the bound that
// actually exists, and let non-power-of-two textures through it safely.
//
// THE INVARIANT. Every texture decode in this engine writes into ONE global
// scratch buffer (VAR_TEXTURE_DECODE_SCRATCH), allocated once at boot for a
// single square texture of side S (0x200 stock; 0x400 once VanillaHelpers has
// patched the boot immediates) and written with no bounds check. So the rule
// every client version upholds is: nothing may reach the decoder larger than
// S on either axis. They just enforce it in different places:
//
//   1.12   allocator FUN_00448450: `< 1024` AND power-of-two, together => <= 512.
//          The create validator's device-caps max is raw hardware (thousands) and
//          protects nothing — a 2560x1080 sailed through it and faulted in the
//          decoder's pixel writer at 0x004492E5.
//   3.3.5  scratch 1024; the allocator's size test is demoted to pool
//          eligibility ([32,512]) and the VALIDATOR carries the bound, because
//          3.3.5 clamps its caps max to 1024. Verified in FUN_004b7f80 /
//          FUN_00681d90 / FUN_004b5bb0. Not grow-on-demand — the same fixed
//          buffer, one size up. VanillaHelpers' patch set is, in effect, this
//          exact backport (size 0x400, scratch 0x400, pool 6x6).
//
// Two consequences this module is built around. The POT rule was PART of 1.12's
// size bound, so relaxing it alone exposes a 600x600 overrun on a stock client.
// And the POT jumps sit ABOVE the size CMPs in the allocator, so retargeting a
// POT failure straight to the pool-skip tail jumps OVER the bound.
//
// DESIGN. A 28-byte code cave in a page we own carries the bound:
//
//     CMP EBX,N ; JA reject ; CMP EAX,N ; JA reject ; JMP pool-skip
//
// and every dimension failure in the allocator — POT or size — is routed into
// it. The cave is the single source of truth for "too big"; the in-function
// size CMPs decide only "poolable". Today both numbers are S. Layer 3 (large
// textures) will grow the scratch, raise N in the cave, and leave the CMPs at
// the pool's maximum — which is precisely 3.3.5's split between allocator and
// validator, minus the dependence on a caps clamp 1.12 doesn't have.
//
// Layers shipped here:
//   1. Gate follows the scratch (always on). S is read back from the boot
//      immediates, so the gate is right on any DLL stack, and additionally
//      capped to what the recycle pool can index (see PoolMaxDimension) —
//      the stock bucket table ends exactly at the scratch pointer.
//   2. VanillaHelpers' pool-index stride (auto, when its 6x6 table is
//      detected). Their table is 6 wide but the index arithmetic stayed at
//      stride 5, so a 1024 dimension collides with a 32-high bucket and the
//      wrong-size texture is recycled: 32x1024 rendered nothing, 64x1024
//      worked by luck. Both index sites are rewritten to stride 6.
//   NPOT (default on, `_classicapi_NonPowerOfTwoTextures`). POT failures go to
//      the cave (bound, then fresh allocation — never the pool, whose log2
//      index is meaningless for NPOT), and the validator's own POT re-test is
//      forced true. Verified in-game for the TGA/UI path (clamped UVs, single
//      mip); NPOT BLPs with DXT blocks or mip chains are untested.
//
// Applied from module registration — glue boot and every FrameXML init — which
// is after every other DLL's load-time patching, so the values read back are
// the ones actually in force.

#include "Common.h"
#include "Game.h"
#include "Offsets.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace Texture::DimensionGate {

namespace {

// ---- byte helpers ------------------------------------------------------------

uint32_t ReadImm32(uintptr_t addr) { return *reinterpret_cast<const uint32_t *>(addr); }

void Put32(uint8_t *dst, uint32_t v) { std::memcpy(dst, &v, sizeof v); }

// rel32 for a jump whose NEXT instruction begins at `next`.
uint32_t Rel32(uintptr_t next, uintptr_t target) {
    return static_cast<uint32_t>(static_cast<int32_t>(target - next));
}

bool WriteImm32(uintptr_t site, uint32_t v) {
    return Common::PatchBytes(reinterpret_cast<void *>(site), &v, sizeof v);
}

// Rewrite a 6-byte `0F <cond> rel32` near jump in engine code. The condition is
// written explicitly, never preserved — VanillaHelpers may have flipped it.
bool WriteNearJump(uintptr_t site, uint8_t cond, uintptr_t target) {
    uint8_t bytes[6] = {0x0F, cond, 0, 0, 0, 0};
    Put32(bytes + 2, Rel32(site + 6, target));
    return Common::PatchBytes(reinterpret_cast<void *>(site), bytes, sizeof bytes);
}

// ---- what the engine was actually configured with ----------------------------

// Side of the boot-allocated decode scratch. Read back from the two immediates
// FUN_00448BD0 feeds FUN_GX_FORMAT_IMAGE_BYTES, so this is what the buffer was
// REALLY sized to, whoever patched them. A mismatch is someone else's bug; the
// smaller is the only safe answer.
uint32_t ScratchDimension() {
    const uint32_t w = ReadImm32(Offsets::PATCH_TEXDECODE_SCRATCH_DIM_W + 1);
    const uint32_t h = ReadImm32(Offsets::PATCH_TEXDECODE_SCRATCH_DIM_H + 1);
    return w < h ? w : h;
}

// Recycle-pool side, from its init-count immediate: 25 = stock 5x5, 36 =
// VanillaHelpers' 6x6. 0 = a layout we don't recognise.
int PoolSide() {
    switch (ReadImm32(Offsets::PATCH_TEXPOOL_INIT_COUNT + 1)) {
    case 25:
        return 5;
    case 36:
        return 6;
    default:
        return 0;
    }
}

// Largest dimension the pool can index: buckets are keyed by log2(dim >> 5)
// over 0..side-1. An unknown layout is treated as stock.
uint32_t PoolMaxDimension(int side) { return 32u << ((side > 0 ? side : 5) - 1); }

// ---- the code cave -----------------------------------------------------------
//
//   +0   81 FB imm32   CMP EBX, N        ; EBX = width  at every entry site
//   +6   0F 87 rel32   JA  reject
//   +12  3D imm32      CMP EAX, N        ; EAX = height
//   +17  0F 87 rel32   JA  reject
//   +23  E9 rel32      JMP pool-skip     ; fresh allocation, descriptor is complete
//
// Entered only by jumps from inside FUN_00448450, where EBX/EAX still hold the
// dimensions, EDI is 0 and [EBP-0x2C] is the finished descriptor — everything
// the pool-skip tail expects. Flags are clobbered; nothing downstream reads
// them. Lives for the process: engine code points into it.

uint8_t *g_cave = nullptr;
constexpr size_t kCaveSize = 28;

bool BuildCave(uint32_t bound) {
    if (g_cave == nullptr) {
        g_cave = static_cast<uint8_t *>(
            VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (g_cave == nullptr)
            return false;
    }
    const auto base = reinterpret_cast<uintptr_t>(g_cave);
    uint8_t c[kCaveSize];
    c[0] = 0x81, c[1] = 0xFB;
    Put32(c + 2, bound);
    c[6] = 0x0F, c[7] = 0x87;
    Put32(c + 8, Rel32(base + 12, Offsets::VA_TEXALLOC_REJECT));
    c[12] = 0x3D;
    Put32(c + 13, bound);
    c[17] = 0x0F, c[18] = 0x87;
    Put32(c + 19, Rel32(base + 23, Offsets::VA_TEXALLOC_REJECT));
    c[23] = 0xE9;
    Put32(c + 24, Rel32(base + 28, Offsets::VA_TEXALLOC_POOL_SKIP));
    std::memcpy(g_cave, c, kCaveSize);
    FlushInstructionCache(GetCurrentProcess(), g_cave, kCaveSize);
    return true;
}

uintptr_t Cave() { return reinterpret_cast<uintptr_t>(g_cave); }

// ---- state -------------------------------------------------------------------

uint32_t g_bound = 0;   // what the cave rejects above
uint32_t g_poolMax = 0; // what the in-function CMPs admit to the pool
int g_poolSide = 0;
bool g_strideFixed = false;
bool g_nonPowerOfTwo = true;

// ---- layer 1: gate follows the scratch --------------------------------------

bool ApplySizeGate() {
    const uint32_t scratch = ScratchDimension();
    g_poolSide = PoolSide();
    const uint32_t poolMax = PoolMaxDimension(g_poolSide);
    // Until layer 3 routes oversize textures around the pool, the bound and the
    // pool limit are the same number: the smaller of what the scratch holds and
    // what the pool can index. (A VanillaHelpers install whose pool patch failed
    // but whose size patch didn't would otherwise index past the stock table —
    // straight into VAR_TEXTURE_DECODE_SCRATCH.)
    g_bound = scratch < poolMax ? scratch : poolMax;
    g_poolMax = g_bound;
    if (!BuildCave(g_bound))
        return false;
    bool ok = WriteImm32(Offsets::PATCH_TEXALLOC_SIZE_W_IMM, g_poolMax);
    ok = WriteImm32(Offsets::PATCH_TEXALLOC_SIZE_H_IMM, g_poolMax) && ok;
    // JA: strictly greater than the pool max leaves the pool path — so exactly
    // poolMax (1024 on a VanillaHelpers stack) remains poolable.
    ok = WriteNearJump(Offsets::PATCH_TEXALLOC_SIZE_W_JMP, 0x87, Cave()) && ok;
    ok = WriteNearJump(Offsets::PATCH_TEXALLOC_SIZE_H_JMP, 0x87, Cave()) && ok;
    return ok;
}

// ---- layer 2: VanillaHelpers' pool-index stride -------------------------------

// `IMUL r,r,6 ; ADD r,other` in place of `LEA t,[other + r*4] ; ADD r,t`:
// same length, same operand roles, stride 5 -> 6. See Offsets.h.
constexpr uint8_t kStride6Alloc[5] = {0x6B, 0xF6, 0x06, 0x03, 0xF3}; // IMUL ESI,ESI,6; ADD ESI,EBX
constexpr uint8_t kStride6Free[5] = {0x6B, 0xDB, 0x06, 0x03, 0xD8};  // IMUL EBX,EBX,6; ADD EBX,EAX

bool ApplyStrideFix() {
    if (g_poolSide != 6) {
        g_strideFixed = false;
        return true; // stock 5x5 (or unknown): stride 5 is correct, leave it
    }
    bool ok = Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_TEXPOOL_INDEX_STRIDE_ALLOC),
                                 kStride6Alloc, sizeof kStride6Alloc);
    ok = Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_TEXPOOL_INDEX_STRIDE_FREE),
                            kStride6Free, sizeof kStride6Free) &&
         ok;
    g_strideFixed = ok;
    return ok;
}

// ---- non-power-of-two ----------------------------------------------------------

struct Site {
    uintptr_t addr;
    unsigned len;
};
constexpr Site kNpotSites[] = {
    {Offsets::PATCH_TEXALLOC_POT_W_JMP, 6},
    {Offsets::PATCH_TEXALLOC_POT_H_JMP, 6},
    {Offsets::PATCH_TEXVALIDATE_POT_W, 4},
    {Offsets::PATCH_TEXVALIDATE_POT_H, 4},
};
constexpr int kNpotSiteCount = sizeof kNpotSites / sizeof kNpotSites[0];
uint8_t g_npotOriginal[kNpotSiteCount][6];
bool g_npotSnapshot = false;

// The validator computes "is a power of two" as
//     EAX = x & (x-1); NEG EAX; SBB EAX,EAX; INC EAX; TEST EAX,EAX; JZ fail
// so replacing NEG/SBB with `XOR EAX,EAX` leaves the INC to produce 1 and the
// test always passes. Patching the arithmetic rather than the shared JZ keeps
// the type-2 branch — which reaches the same TEST through its own SBB — intact.
constexpr uint8_t kPotTestForced[4] = {0x31, 0xC0, 0x90, 0x90}; // XOR EAX,EAX; NOP; NOP

// Nobody else patches these four sites, so the snapshot is stock — but reading
// it back is still cheaper to trust than a byte pattern we assumed.
void SnapshotNpotSites() {
    if (g_npotSnapshot)
        return;
    for (int i = 0; i < kNpotSiteCount; ++i)
        std::memcpy(g_npotOriginal[i], reinterpret_cast<const void *>(kNpotSites[i].addr),
                    kNpotSites[i].len);
    g_npotSnapshot = true;
}

bool ApplyNonPowerOfTwo(bool enable) {
    SnapshotNpotSites();
    bool ok = true;
    if (enable) {
        // JNZ (not a power of two) -> the cave: bound first, then a fresh
        // allocation — never the pool, whose log2 index cannot describe NPOT.
        ok = WriteNearJump(Offsets::PATCH_TEXALLOC_POT_W_JMP, 0x85, Cave()) && ok;
        ok = WriteNearJump(Offsets::PATCH_TEXALLOC_POT_H_JMP, 0x85, Cave()) && ok;
        ok = Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_TEXVALIDATE_POT_W),
                                kPotTestForced, sizeof kPotTestForced) &&
             ok;
        ok = Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_TEXVALIDATE_POT_H),
                                kPotTestForced, sizeof kPotTestForced) &&
             ok;
    } else {
        for (int i = 0; i < kNpotSiteCount; ++i)
            ok = Common::PatchBytes(reinterpret_cast<void *>(kNpotSites[i].addr), g_npotOriginal[i],
                                    kNpotSites[i].len) &&
                 ok;
    }
    if (ok)
        g_nonPowerOfTwo = enable;
    return ok;
}

// Re-asserted on every registration so a `/reload` cannot leave the code
// segment and our recorded state disagreeing. Order matters: the cave must
// exist before anything is pointed at it.
void ApplyCurrent() {
    if (!ApplySizeGate())
        return;
    ApplyStrideFix();
    ApplyNonPowerOfTwo(g_nonPowerOfTwo);
}

// ---- Lua ---------------------------------------------------------------------

// `_classicapi_NonPowerOfTwoTextures([enable])` -> bool
// Returns the state after the call; with no argument it only reports. The engine
// caches textures by path for the session, so a texture that failed under the
// old setting needs a different path to be retried.
int __fastcall Script_NonPowerOfTwoTextures(void *L) {
    if (Game::Lua::Type(L, 1) > Game::Lua::TYPE_NIL)
        ApplyNonPowerOfTwo(Game::Lua::ToBoolean(L, 1) != 0);
    Game::Lua::PushBool(L, g_nonPowerOfTwo);
    return 1;
}

// `_classicapi_TextureLimits()` -> maxDimension, poolSide, nonPowerOfTwo, strideFixed
// What the gate is actually enforcing on this DLL stack, for /dump.
int __fastcall Script_TextureLimits(void *L) {
    Game::Lua::PushNumber(L, static_cast<double>(g_bound));
    Game::Lua::PushNumber(L, static_cast<double>(g_poolSide));
    Game::Lua::PushBool(L, g_nonPowerOfTwo);
    Game::Lua::PushBool(L, g_strideFixed);
    return 4;
}

void RegisterLuaFunctions() {
    ApplyCurrent();
    Game::Lua::RegisterGlobalFunction("_classicapi_NonPowerOfTwoTextures",
                                      &Script_NonPowerOfTwoTextures);
    Game::Lua::RegisterGlobalFunction("_classicapi_TextureLimits", &Script_TextureLimits);
}

// The glue screen loads textures too, and this is the first registration that
// runs after every other DLL's load-time patching.
void RegisterGlue() { ApplyCurrent(); }

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};
const Game::GlueModuleAutoRegister _glueAutoreg{&RegisterGlue};

} // namespace

} // namespace Texture::DimensionGate
