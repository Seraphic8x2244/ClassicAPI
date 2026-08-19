// This file is part of ClassicAPI.
//
// ClassicAPI is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// ClassicAPI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.

// FontString metric backports.
//
// 1.12's FontString method table (32 entries at 0x0087C1D8, registry context
// VAR_FONTSTRING_METHOD_REGISTRY) ships GetStringWidth but none of the later
// metric bindings. The engine already has every internal getter — these are
// the thin Lua bindings Blizzard never wrote for 1.12:
//
//   GetStringHeight()         — 2.3.0 binding; internal getter
//                               FUN_FONTSTRING_STRING_HEIGHT (wrap-aware,
//                               lines×fontH + (lines−1)×spacing, cache
//                               fs+0x100, 0 for empty text).
//   GetUnboundedStringWidth() — the substring measure with the WHOLE string
//                               (FUN_FONTSTRING_MEASURE_SUBSTRING, len 0 =
//                               strlen): the string's width with no wrap cap.
//                               Icon-aware for free — the call routes through
//                               text/InlineTexture.cpp's co-hook.
//   GetNumLines()             — the node's built line count (render truth,
//                               OFF_TEXT_NODE_BUILT_LINES) once painted; for
//                               an unbuilt node, the engine's own break-array
//                               computer (FUN_FONTSTRING_BREAK_ARRAY — also
//                               icon-aware via the wrap-stepper co-hook).
//   GetLineHeight()           — the fs font height via the same getter the
//                               rebuild feeds the text node (FUN_007727b0);
//                               excludes spacing, like retail (GetSpacing is
//                               its own method).
//   SetFormattedText(fmt,...) — string.format + SetText convenience; the
//                               format runs through Lua's own string.format
//                               under pcall, the set through the engine's
//                               FUN_FONTSTRING_SET_TEXT (keeping the text
//                               sanitizer/pipe handling identical to SetText).
//
// Push conversion mirrors Script_GetStringWidth (0x0079E510): resolve self,
// call the internal getter, convert anchor units → UI pixels, push.
//
// No inline-icon adjustment lives HERE: the width/height internal getters are
// already icon-adjusted by their co-hooks (calling the hooked VA routes
// through the detour), and icons never change GetLineHeight.

#include "Game.h"
#include "Offsets.h"

#include <cstdint>

namespace FontString::Metrics {
namespace {

// FUN_FONTSTRING_STRING_HEIGHT — ECX = fs, no stack args, float in x87 ST0.
using StringHeightInternal_t = float(__fastcall *)(void *fs);
// FUN_FONTSTRING_MEASURE_SUBSTRING — __thiscall(fs, text, len); dummy-EDX form.
using MeasureSubstring_t = float(__fastcall *)(void *fs, void *edx, const char *text, int len);
// FUN_FONTSTRING_FONT_HEIGHT — __thiscall(fs, mode-on-stack); dummy-EDX form.
using FsFontHeight_t = float(__fastcall *)(void *fs, void *edx, int mode);
// FUN_FONTSTRING_BREAK_ARRAY — __thiscall(fs, text, wrapWidth, outBreaks, cap).
using BreakArray_t = uint32_t(__fastcall *)(void *fs, void *edx, const char *text, float width,
                                            int *outBreaks, int cap);
// FUN_FONTSTRING_SET_TEXT — __thiscall(fs, text, flag = 0); dummy-EDX form.
using FsSetText_t = void(__fastcall *)(void *fs, void *edx, const char *text, int flag);

// Anchor units → UI pixels: `FUN_0041AE40(FUN_0041AD70() × DAT_007FFD68 × v)`
// = v × [VAR_UI_COORD_SCALE_DIV] × 1024 / [VAR_UI_COORD_SCALE_MUL] — the exact
// push chain Script_GetStringWidth uses (the inverse of
// Tooltip::LinePool::PixelToInternal).
double InternalToPixel(float v) {
    const float mul = Game::Read<float>(Offsets::VAR_UI_COORD_SCALE_MUL);
    const float div = Game::Read<float>(Offsets::VAR_UI_COORD_SCALE_DIV);
    if (mul == 0.0f)
        return 0.0;
    return static_cast<double>(v) * div * Offsets::UI_COORD_SCALE_UNIT / mul;
}

// Shared self-resolve for every method here.
void *ResolveSelf(void *L, const char *usage) {
    void *fs = nullptr;
    if (Game::Lua::Type(L, 1) == Game::Lua::TYPE_TABLE)
        fs = Game::Lua::ResolveObject(L, 1);
    if (fs == nullptr)
        Game::Lua::Error(L, "%s", usage);
    return fs;
}

int __fastcall Script_GetStringHeight(void *L) {
    void *fs = ResolveSelf(L, "Usage: fontstring:GetStringHeight()");
    if (fs == nullptr)
        return 0;
    const float h = reinterpret_cast<StringHeightInternal_t>(
        Offsets::FUN_FONTSTRING_STRING_HEIGHT)(fs);
    Game::Lua::PushNumber(L, InternalToPixel(h));
    return 1;
}

int __fastcall Script_GetUnboundedStringWidth(void *L) {
    void *fs = ResolveSelf(L, "Usage: fontstring:GetUnboundedStringWidth()");
    if (fs == nullptr)
        return 0;
    const char *text = Game::Read<const char *>(fs, Offsets::OFF_FONTSTRING_TEXT);
    if (text == nullptr || *text == '\0') {
        Game::Lua::PushNumber(L, 0.0);
        return 1;
    }
    const float w = reinterpret_cast<MeasureSubstring_t>(
        Offsets::FUN_FONTSTRING_MEASURE_SUBSTRING)(fs, nullptr, text, 0);
    Game::Lua::PushNumber(L, InternalToPixel(w));
    return 1;
}

int __fastcall Script_GetNumLines(void *L) {
    void *fs = ResolveSelf(L, "Usage: fontstring:GetNumLines()");
    if (fs == nullptr)
        return 0;
    const char *text = Game::Read<const char *>(fs, Offsets::OFF_FONTSTRING_TEXT);
    if (text == nullptr || *text == '\0') {
        Game::Lua::PushNumber(L, 0.0);
        return 1;
    }
    // Render truth first: the built node's line count. Zero until the node
    // paints once (hidden fs, mid-rebuild) — fall back below.
    void *block = Game::Read<void *>(fs, Offsets::OFF_FONTSTRING_TEXT_BLOCK);
    if (block != nullptr) {
        void *node = Game::Read<void *>(block, Offsets::OFF_TEXTBLOCK_NODE);
        if (node != nullptr) {
            const int built = Game::Read<int>(node, Offsets::OFF_TEXT_NODE_BUILT_LINES);
            if (built > 0) {
                Game::Lua::PushNumber(L, static_cast<double>(built));
                return 1;
            }
        }
    }
    // Unbuilt: the engine's own break-array computer, fed the fs's current
    // rect width (its wrap budget). An unresolved rect (0 width) means no
    // wrap constraint exists yet — one line, like a fresh single-anchor fs.
    const float *rc = Game::Ptr<const float>(fs, Offsets::OFF_REGION_RECT);
    const float width = (rc[3] > rc[1]) ? (rc[3] - rc[1]) : (rc[1] - rc[3]);
    if (width <= 0.0f) {
        Game::Lua::PushNumber(L, 1.0);
        return 1;
    }
    int breaks[64];
    uint32_t lines = reinterpret_cast<BreakArray_t>(Offsets::FUN_FONTSTRING_BREAK_ARRAY)(
        fs, nullptr, text, width, breaks, 64);
    if (lines > 64u)
        lines = 64u;
    if (lines == 0u)
        lines = 1u;
    Game::Lua::PushNumber(L, static_cast<double>(lines));
    return 1;
}

int __fastcall Script_GetLineHeight(void *L) {
    void *fs = ResolveSelf(L, "Usage: fontstring:GetLineHeight()");
    if (fs == nullptr)
        return 0;
    const float h = reinterpret_cast<FsFontHeight_t>(Offsets::FUN_FONTSTRING_FONT_HEIGHT)(
        fs, nullptr, 1);
    Game::Lua::PushNumber(L, InternalToPixel(h));
    return 1;
}

int __fastcall Script_SetFormattedText(void *L) {
    void *fs = nullptr;
    if (Game::Lua::Type(L, 1) == Game::Lua::TYPE_TABLE)
        fs = Game::Lua::ResolveObject(L, 1);
    if (fs == nullptr || !Game::Lua::IsString(L, 2)) {
        Game::Lua::Error(L, "Usage: fontstring:SetFormattedText(\"format\"[, ...])");
        return 0;
    }
    const int top = Game::Lua::GetTop(L);
    Game::Lua::CheckStack(L, top + 4);
    // _G["string"]["format"] — the live library function, so any addon
    // replacement of string.format is honoured, matching a Lua-side
    // SetText(format(...)).
    Game::Lua::PushString(L, "string");
    Game::Lua::GetTable(L, Game::Lua::GLOBALS_INDEX);
    if (Game::Lua::Type(L, -1) != Game::Lua::TYPE_TABLE) {
        Game::Lua::SetTop(L, top);
        Game::Lua::Error(L, "SetFormattedText: string library unavailable");
        return 0;
    }
    Game::Lua::PushString(L, "format");
    Game::Lua::GetTable(L, -2);
    Game::Lua::Remove(L, -2);
    if (Game::Lua::Type(L, -1) != Game::Lua::TYPE_FUNCTION) {
        Game::Lua::SetTop(L, top);
        Game::Lua::Error(L, "SetFormattedText: string.format unavailable");
        return 0;
    }
    for (int i = 2; i <= top; ++i)
        Game::Lua::PushValue(L, i);
    if (Game::Lua::PCall(L, top - 1, 1, 0) != 0) {
        const char *msg = Game::Lua::ToString(L, -1);
        Game::Lua::Error(L, "%s", (msg != nullptr) ? msg : "SetFormattedText: format failed");
        return 0;
    }
    const char *result = Game::Lua::ToString(L, -1);
    reinterpret_cast<FsSetText_t>(Offsets::FUN_FONTSTRING_SET_TEXT)(
        fs, nullptr, (result != nullptr) ? result : "", 0);
    Game::Lua::SetTop(L, top);
    return 0;
}

const Game::Lua::FrameMethodEntry g_methods[] = {
    {"GetStringHeight", &Script_GetStringHeight},
    {"GetUnboundedStringWidth", &Script_GetUnboundedStringWidth},
    {"GetNumLines", &Script_GetNumLines},
    {"GetLineHeight", &Script_GetLineHeight},
    {"SetFormattedText", &Script_SetFormattedText},
};

void RegisterLuaFunctions() {
    Game::Lua::RegisterFrameMethods(
        reinterpret_cast<void *>(Offsets::VAR_FONTSTRING_METHOD_REGISTRY), g_methods,
        static_cast<int>(sizeof(g_methods) / sizeof(g_methods[0])));
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace
} // namespace FontString::Metrics
