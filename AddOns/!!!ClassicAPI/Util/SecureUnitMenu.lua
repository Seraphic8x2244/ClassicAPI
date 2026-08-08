-- Unit right-click menu for attribute-driven unit frames.
--
-- Backs the `menu` / `togglemenu` click verb of the Frame:SetAttribute
-- backport (see src/frame/Attributes.cpp). When a frame with a `unit`
-- attribute resolves a click to the `menu` verb, the DLL calls
-- ClassicAPI_ToggleUnitMenu(unit), which pops the standard unit dropdown at
-- the cursor -- the same UnitPopup menu Blizzard's PlayerFrame / TargetFrame /
-- PartyMemberFrame show on right-click. Lets a unit frame express its
-- right-click menu purely as an attribute (e.g. type2 = "menu") instead of a
-- hand-rolled OnClick handler. pfUI's unit frames use exactly this path
-- (api/unitframes.lua -> ClassicAPI_ToggleUnitMenu).
--
-- The unit -> menu-type resolution mirrors Blizzard's own generic resolver
-- (TargetFrameDropDown_Initialize in FrameXML): self -> SELF, pet -> PET, a
-- grouped player -> PARTY (whisper / inspect / trade / follow / promote / ...),
-- any other player -> PLAYER (adds INVITE), anything else (NPC) ->
-- RAID_TARGET_ICON. Vanilla's stock UI never wires a unit to the "RAID" menu
-- (there are no clickable raid unit frames in 1.12), and PARTY carries the
-- options players actually want on a grouped member, so grouped players (party
-- or raid) use PARTY -- matching how pfUI drives its raid menu.
--
-- Focus / Clear Focus: we add one contextual entry -- "Set Focus" when the
-- unit is not the current focus, "Clear Focus" when it is -- for real,
-- non-self units. This is done entirely inside our own dropdown: no
-- hooksecurefunc, and no mutation of Blizzard's shared UnitPopupMenus tables
-- (so default frames and other addons are unaffected). Because UnitPopup_ShowMenu
-- hardwires every button's click to UnitPopup_OnClick (which does not know our
-- keys), we add the Focus button ourselves with our own func. To keep Cancel
-- last, we show the standard menu MINUS its trailing Cancel, then append Focus
-- and Cancel. Backed by the DLL's FocusUnit / ClearFocus globals and the
-- `focus` unit token (src/unit/Focus.cpp). Labels come from the localized
-- SET_FOCUS / CLEAR_FOCUS globals (locales/*.lua).

local dropdown;

-- Menu types that describe a real, targetable unit (as opposed to the NPC
-- raid-marker submenu). Only these get a Focus entry.
local FOCUSABLE_MENU = { SELF = true, PET = true, PARTY = true, PLAYER = true };

-- Scratch menu key we own, rebuilt each open as a copy of the resolved base
-- menu with its trailing CANCEL removed. Kept out of Blizzard's tables.
local SCRATCH_MENU = "CLASSICAPI_UNITMENU";

local function EnsureDropdown()
    if not dropdown then
        dropdown = CreateFrame("Frame", "ClassicAPIUnitMenuDropDown", UIParent, "UIDropDownMenuTemplate");
        dropdown.displayMode = "MENU";
    end
    return dropdown;
end

local function ResolveMenu(unit)
    if UnitIsUnit(unit, "player") then
        return "SELF";
    elseif UnitIsUnit(unit, "pet") then
        return "PET";
    elseif UnitIsPlayer(unit) then
        if UnitInParty(unit) or UnitInRaid(unit) then
            return "PARTY";
        end
        return "PLAYER";
    end
    return "RAID_TARGET_ICON";
end

-- Append the contextual Focus / Clear Focus button for `unit`, with our own
-- click func. Skipped for the player themselves (focusing yourself is a no-op).
local function AddFocusButton(unit)
    if UnitIsUnit(unit, "player") then
        return;
    end
    local info = UIDropDownMenu_CreateInfo();
    info.notCheckable = 1;
    if UnitExists("focus") and UnitIsUnit(unit, "focus") then
        info.text = CLEAR_FOCUS;
        info.func = function() ClearFocus(); end;
    else
        info.text = SET_FOCUS;
        info.func = function() FocusUnit(unit); end;
    end
    UIDropDownMenu_AddButton(info);
end

local function AddCancelButton()
    local info = UIDropDownMenu_CreateInfo();
    info.text = CANCEL;
    info.notCheckable = 1;
    UIDropDownMenu_AddButton(info);
end

function ClassicAPI_ToggleUnitMenu(unit)
    if not unit or not UnitExists(unit) then
        return;
    end
    local which = ResolveMenu(unit);
    local dd = EnsureDropdown();

    if not FOCUSABLE_MENU[which] then
        -- NPC / raid-marker menu: no Focus entry, show it verbatim.
        local name = (which == "RAID_TARGET_ICON") and RAID_TARGET_ICON or nil;
        dd.initialize = function()
            UnitPopup_ShowMenu(dd, which, unit, name);
        end
    else
        dd.initialize = function()
            -- Copy the base menu without its trailing CANCEL so we can place
            -- Focus and Cancel ourselves, in the right order. Blizzard's own
            -- UnitPopupMenus[which] is left untouched.
            local base = UnitPopupMenus[which];
            local scratch = {};
            for _, value in ipairs(base) do
                if value ~= "CANCEL" then
                    table.insert(scratch, value);
                end
            end
            UnitPopupMenus[SCRATCH_MENU] = scratch;
            UnitPopup_ShowMenu(dd, SCRATCH_MENU, unit);
            AddFocusButton(unit);
            AddCancelButton();
        end
    end
    ToggleDropDownMenu(1, nil, dd, "cursor");
end
