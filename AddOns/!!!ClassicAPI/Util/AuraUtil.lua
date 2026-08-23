-- AuraUtil: iterate / find auras without allocating a table per aura.
--
-- Backport of FrameXML's AuraUtil, the helper library that sits on top of
-- C_UnitAuras. Its whole value on 1.12 is the zero-allocation path: modern
-- addons that scan many units' auras every frame call AuraUtil.ForEachAura (with
-- usePackedAura false) instead of C_UnitAuras.GetAuraDataByIndex, so no AuraData
-- table is built per aura -- which matters far more under Lua 5.0's mark-sweep
-- GC than it does on retail's generational GC. See ClassicAPI issue #30.
--
-- Ported from the anniversary source (Blizzard_FrameXMLUtil/AuraUtil.lua),
-- keeping the exact public shapes:
--   * ForEachAura(unit, filter, batchSize, func, usePackedAura)
--   * FindAura(predicate, unit, filter, arg1, arg2, arg3)
--   * FindAuraByName(auraName, unit, filter)
--   * UnpackAuraData(auraData)
--
-- Two deliberate differences from that source:
--   1. Retail's ForEachAura/FindAura batch through GetAuraSlots / GetAuraDataBySlot
--      (continuation tokens) -- 1.12 has no slot-batching API, so the bodies here
--      iterate by index (1, 2, ...) until the accessor returns no aura. Same public
--      contract, different internals.
--   2. The non-packed ForEachAura and FindAura source each aura from the positional
--      C_UnitAuras.UnitAura (no table), so both are zero-allocation here -- FindAura
--      in particular, where the retail version allocates a table per aura scanned.
--
-- The comparator / dispel-priority / border helpers (DefaultAuraCompare,
-- UnitFrameDebuffComparator, ShouldDisplayDebuff, SetAuraBorder*, IsPriorityDebuff)
-- are NOT ported: they depend on isPriorityAura / auraInstanceID / the aura
-- visualization pipeline / secure-call machinery, none of which exist on 1.12.

AuraUtil = AuraUtil or {};

-- Returns the AuraData table's fields in the classic `UnitAura` positional order,
-- for code written against that shape. Retail appends `unpack(auraData.points)`;
-- vanilla auras carry no points, so the tuple ends at timeMod (15 values). Field
-- #13 is `isFromPlayerOrPlayerPet` (the table's field) -- note the positional
-- C_UnitAuras.UnitAura reports the higher-fidelity `castByPlayer` at #13 instead.
function AuraUtil.UnpackAuraData(auraData)
    if not auraData then
        return nil;
    end
    return auraData.name,
        auraData.icon,
        auraData.applications,
        auraData.dispelName,
        auraData.duration,
        auraData.expirationTime,
        auraData.sourceUnit,
        auraData.isStealable,
        auraData.nameplateShowPersonal,
        auraData.spellId,
        auraData.canApplyAura,
        auraData.isBossAura,
        auraData.isFromPlayerOrPlayerPet,
        auraData.nameplateShowAll,
        auraData.timeMod;
end

-- Calls `func` for each aura on `unit` matching `filter`, in index order, until
-- `func` returns a truthy value or the auras run out. Non-packed (default) passes
-- the 15 positional `UnitAura` values (no table); `usePackedAura` passes the
-- AuraData table from GetAuraDataByIndex. `batchSize <= 0` does nothing; any other
-- value (or nil) visits every matching aura -- `batchSize` is retail's slot-batch
-- hint, not a cap on how many auras are visited.
function AuraUtil.ForEachAura(unit, filter, batchSize, func, usePackedAura)
    if batchSize and batchSize <= 0 then
        return;
    end
    local index = 1;
    while true do
        local done;
        if usePackedAura then
            local auraData = C_UnitAuras.GetAuraDataByIndex(unit, index, filter);
            if auraData == nil then
                return;
            end
            done = func(auraData);
        else
            local name, icon, count, dispelType, duration, expirationTime, source,
                isStealable, nameplateShowPersonal, spellId, canApplyAura,
                isBossDebuff, castByPlayer, nameplateShowAll, timeMod =
                C_UnitAuras.UnitAura(unit, index, filter);
            if name == nil then
                return;
            end
            done = func(name, icon, count, dispelType, duration, expirationTime,
                source, isStealable, nameplateShowPersonal, spellId, canApplyAura,
                isBossDebuff, castByPlayer, nameplateShowAll, timeMod);
        end
        if done then
            return;
        end
        index = index + 1;
    end
end

-- Recursive worker: `...` is the current aura's positional values (nil when the
-- index is past the last aura). Tail-recurses to the next index. Lua 5.0 has
-- proper tail calls, so depth is constant regardless of aura count.
local function FindAuraRecurse(predicate, unit, filter, index, arg1, arg2, arg3, ...)
    if ... == nil then
        return nil; -- past the last aura -> not found
    end
    if predicate(arg1, arg2, arg3, ...) then
        return ...;
    end
    index = index + 1;
    return FindAuraRecurse(predicate, unit, filter, index, arg1, arg2, arg3,
        C_UnitAuras.UnitAura(unit, index, filter));
end

-- Returns the positional values of the first aura for which `predicate` returns
-- truthy, or nil. `predicate` receives up to three caller args followed by the
-- aura's positional values: predicate(arg1, arg2, arg3, name, icon, ...). Sources
-- auras from the positional accessor, so no table is allocated while scanning.
function AuraUtil.FindAura(predicate, unit, filter, arg1, arg2, arg3)
    return FindAuraRecurse(predicate, unit, filter, 1, arg1, arg2, arg3,
        C_UnitAuras.UnitAura(unit, 1, filter));
end

-- Returns the positional values of the first aura whose name matches `auraName`,
-- or nil. Names are localized and not unique -- this finds the first match.
function AuraUtil.FindAuraByName(auraName, unit, filter)
    return AuraUtil.UnpackAuraData(
        C_UnitAuras.GetAuraDataBySpellName(unit, auraName, filter));
end
