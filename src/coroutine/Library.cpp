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
//
// Restores Lua 5.0's stripped `coroutine.*` library to the in-game Lua
// state. WoW 1.12.1 ships with the C-level coroutine machinery linked
// in (`lua_resume`, `lua_yield`, `lua_newthread`, threads as a real
// type tag 8) but the `coroutine` global table was never registered.
// The five trampolines below mirror Lua 5.0's `lbaselib.c` shapes,
// cross-checked against 5.4.8's wired-up library via Ghidra (see the
// "Cross-binary reference" technique in CLAUDE.md).

#include "Game.h"

#include <cstdint>

namespace Coroutine {

// Lua 5.0 result codes returned by `lua_resume`.
static constexpr int LUA_OK = 0;
static constexpr int LUA_YIELD = 1;

// Field offsets in `lua_State`, confirmed from `lua_resume`'s error
// cleanup path (`L->ci = L->base_ci; L->base = base_ci->base`):
//   *(L+0x14) = *(L+0x28)
//   *(L+0x0C) = **(L+0x28)
static constexpr uintptr_t OFF_L_CI = 0x14;
static constexpr uintptr_t OFF_L_BASE_CI = 0x28;

// Reads `L->ci` (the current CallInfo). When greater than `L->base_ci`
// the coroutine's body function is mid-execution (paused at a yield);
// when equal, no Lua function is on this thread's call stack.
static void *LuaCI(void *L) {
    return *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(L) + OFF_L_CI);
}
static void *LuaBaseCI(void *L) {
    return *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(L) + OFF_L_BASE_CI);
}

// Coroutine status codes returned by `CoStatus` below — fed into the
// `kStatusNames` table for the `status` API. The dense 0..3 encoding
// also lets us label "unsuspended" states for resume's error path.
static constexpr int CO_RUNNING = 0;
static constexpr int CO_SUSPENDED = 1;
static constexpr int CO_NORMAL = 2;
static constexpr int CO_DEAD = 3;

static const char *const kStatusNames[] = {
    "running",
    "suspended",
    "normal",
    "dead",
};

// Inner status classifier. Equivalent to Lua 5.0's `auxstatus` from
// lbaselib.c, but we can't rely on `L->status` here: this build's
// `lua_yield` doesn't write status (it sets a CI state flag bit
// `0x10` instead), so the canonical "ttype(status) == LUA_YIELD"
// check fails. We use the `ci > base_ci` signal instead — true iff
// a Lua function is currently paused on this thread's call stack.
//
// State map:
//   co->ci  >  co->base_ci         → SUSPENDED (yielded; body paused)
//   co->ci ==  co->base_ci, top==0 → DEAD (returned, all consumed)
//   co->ci ==  co->base_ci, top>0  → SUSPENDED (initial: fn pushed,
//                                    never resumed)
// After AuxResume's XMove cycle, both "returned" and "yielded" leave
// co's stack empty; the ci comparison is what distinguishes them.
// "Normal" (coroutine that resumed a deeper coroutine — appears as
// the parent in the call chain) is rare and would require walking
// the global thread list; we collapse it into "suspended".
static int CoStatus(void *L, void *co) {
    if (co == L)
        return CO_RUNNING;
    if (LuaCI(co) > LuaBaseCI(co))
        return CO_SUSPENDED;
    return Game::Lua::GetTop(co) == 0 ? CO_DEAD : CO_SUSPENDED;
}

// Common back-end for resume / wrap. Returns nres ≥ 0 on success
// (with values already xmove'd onto L), or -1 on error (with the
// error message left on L's stack).
//
// WoW's modified Lua doesn't follow the standard `lua_resume` status
// contract: it returns `0` for normal completion and `1` for both
// "yielded" AND "errored mid-execution". We disambiguate the two
// `s==1` cases by checking the call-stack depth — a yielded
// coroutine still has its body paused (`ci > base_ci`), while an
// errored coroutine has been unwound back to `base_ci`. Errors hit
// from the "dead/already-resumed" early-out (e.g. the inner
// `cannot resume dead coroutine` path) also land in `s != 0` with
// `ci == base_ci`, so the same branch covers them.
static int AuxResume(void *L, void *co, int nargs) {
    const int st = CoStatus(L, co);
    if (st != CO_SUSPENDED) {
        Game::Lua::PushString(L, st == CO_DEAD
                                     ? "cannot resume dead coroutine"
                                     : "cannot resume non-suspended coroutine");
        return -1;
    }
    if (Game::Lua::CheckStack(co, nargs) == 0) {
        Game::Lua::PushString(L, "too many arguments to resume coroutine");
        return -1;
    }
    Game::Lua::XMove(L, co, nargs);
    const int s = Game::Lua::Resume(co, nargs);
    const bool yielded = LuaCI(co) > LuaBaseCI(co);
    if (s == LUA_OK || yielded) {
        const int nres = Game::Lua::GetTop(co);
        if (Game::Lua::CheckStack(L, nres) == 0) {
            Game::Lua::PushString(L, "too many results to resume coroutine");
            return -1;
        }
        Game::Lua::XMove(co, L, nres);
        return nres;
    }
    // Error path: engine left a single error message on co's stack.
    Game::Lua::XMove(co, L, 1);
    return -1;
}

static int __fastcall Script_Coroutine_Create(void *L) {
    if (Game::Lua::Type(L, 1) != Game::Lua::TYPE_FUNCTION || Game::Lua::IsCFunction(L, 1)) {
        Game::Lua::ArgError(L, 1, "Lua function expected");
        return 0; // unreachable
    }
    Game::Lua::NewThread(L);                                     // [arg1, NL]
    void *NL = Game::Lua::ToThread(L, -1);
    if (NL == nullptr) {
        Game::Lua::Error(L, "coroutine.create: failed to allocate thread");
        return 0; // unreachable
    }
    Game::Lua::PushValue(L, 1);                                  // [arg1, NL, fn]
    Game::Lua::XMove(L, NL, 1);                                  // [arg1, NL]; NL has fn
    return 1;
}

static int __fastcall Script_Coroutine_Yield(void *L) {
    return Game::Lua::Yield(L, Game::Lua::GetTop(L));
}

static int __fastcall Script_Coroutine_Resume(void *L) {
    if (Game::Lua::Type(L, 1) != Game::Lua::TYPE_THREAD) {
        Game::Lua::ArgError(L, 1, "coroutine expected");
        return 0; // unreachable
    }
    void *co = Game::Lua::ToThread(L, 1);
    const int nargs = Game::Lua::GetTop(L) - 1;
    const int r = AuxResume(L, co, nargs);
    if (r < 0) {
        // Error message is on top of L's stack. Push `false` and
        // shuffle it below the message: [..., msg] -> [..., false, msg].
        Game::Lua::PushBool(L, false);
        Game::Lua::Insert(L, -2);
        return 2;
    }
    // Success: `r` values on top of L's stack. Push `true` and slide
    // it into position so returns are [..., true, v1, v2, ..., vr].
    Game::Lua::PushBool(L, true);
    Game::Lua::Insert(L, -(r + 1));
    return r + 1;
}

static int __fastcall Script_Coroutine_Status(void *L) {
    if (Game::Lua::Type(L, 1) != Game::Lua::TYPE_THREAD) {
        Game::Lua::ArgError(L, 1, "coroutine expected");
        return 0; // unreachable
    }
    void *co = Game::Lua::ToThread(L, 1);
    Game::Lua::PushString(L, kStatusNames[CoStatus(L, co)]);
    return 1;
}

// `coroutine.wrap`'s returned closure. The captured coroutine lives
// in upvalue 1; each invocation forwards args via resume and either
// returns the yielded/returned values directly (no { ok, ... }
// wrapping like resume does) or re-raises on error.
static int __fastcall WrapAux(void *L) {
    void *co = Game::Lua::ToThread(L, Game::Lua::UpvalueIndex(1));
    if (co == nullptr) {
        Game::Lua::Error(L, "coroutine.wrap: missing thread upvalue");
        return 0; // unreachable
    }
    const int nargs = Game::Lua::GetTop(L);
    const int r = AuxResume(L, co, nargs);
    if (r < 0) {
        // The error message is at -1. `lua_error`'s 1.12 wrapper takes
        // a printf-format string + va args; we pass `%s` so any `%`
        // characters in the message are treated as literal.
        const char *msg = Game::Lua::ToString(L, -1);
        Game::Lua::Error(L, "%s", msg != nullptr ? msg : "coroutine error");
        return 0; // unreachable
    }
    return r;
}

static int __fastcall Script_Coroutine_Wrap(void *L) {
    // Reuse cocreate to build the thread (validates arg, allocates,
    // moves fn into the new state). Leaves the thread on top of L.
    Script_Coroutine_Create(L);
    Game::Lua::PushCClosure(L, &WrapAux, 1); // captures thread as upvalue 1
    return 1;
}

// `coroutine.running()` — Lua 5.1 addition: the running coroutine, or
// nil when called from the main thread. The 5.0 C API has no
// `lua_pushthread` (also a 5.1 addition), so the thread TValue is
// written to the stack raw: this build's TValue is 16 bytes with
// {tt @ +0, value @ +8} (see LuaSyntax::StringMethods for the layout
// derivation), tag 8 = thread, and the value is the running `lua_State`
// itself — exactly what `lua_newthread` stores and what LUA_TO_POINTER
// reads back at tvalue[2]. No stack-overflow check needed: every C
// function is guaranteed LUA_MINSTACK free slots and this pushes one.
// The running thread is trivially GC-alive, so no anchoring either.
// Main-thread detection: module callbacks run on the state's main
// thread, so the L captured at registration IS the main thread;
// coroutine bodies run on their own lua_State.
static void *g_mainL = nullptr;
static constexpr uintptr_t OFF_L_TOP = 0x8; // L->top (push helpers' [ecx+8])

static int __fastcall Script_Coroutine_Running(void *L) {
    if (L == g_mainL) {
        Game::Lua::PushNil(L);
        return 1;
    }
    uint8_t *top = Game::Read<uint8_t *>(L, OFF_L_TOP);
    *reinterpret_cast<int *>(top) = Game::Lua::TYPE_THREAD;
    *reinterpret_cast<void **>(top + 8) = L;
    Game::Ref<uint8_t *>(L, OFF_L_TOP) = top + 16;
    return 1;
}

static void RegisterLuaFunctions() {
    g_mainL = Game::Lua::State(); // the state's main thread (see Running)
    Game::Lua::RegisterTableFunction("coroutine", "create", &Script_Coroutine_Create);
    Game::Lua::RegisterTableFunction("coroutine", "resume", &Script_Coroutine_Resume);
    Game::Lua::RegisterTableFunction("coroutine", "yield", &Script_Coroutine_Yield);
    Game::Lua::RegisterTableFunction("coroutine", "status", &Script_Coroutine_Status);
    Game::Lua::RegisterTableFunction("coroutine", "wrap", &Script_Coroutine_Wrap);
    Game::Lua::RegisterTableFunction("coroutine", "running", &Script_Coroutine_Running);
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace Coroutine
