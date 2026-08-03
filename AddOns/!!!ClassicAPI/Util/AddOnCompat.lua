if C_AddOns.DoesAddOnExist('pfUI') then
    EventUtil.ContinueOnAddOnLoaded('pfUI', function()
        -- pfUI\pfUI.lua redeclares the RAID_CLASS_COLORS object at least 14 times (insane!!)
        -- This stops it from replacing all our ColorMixin values
        pfUI.UpdateColors = function() end
        RAID_CLASS_COLORS = setmetatable(RAID_CLASS_COLORS, { __index = function()
            local unknownColor = CreateColor(0.6, 0.6, 0.6)
            unknownColor.colorStr = unknownColor:GenerateHexColor()
            return unknownColor
        end})

        -- Compatibility with OLDER forks of pfUI. Our DLL adds the modern
        -- HookScript widget method; older pfUI forks branch on
        -- `if button.HookScript` and take a modern code path they were never
        -- written for on their pfActionBar buttons, which breaks them. While
        -- the actionbar module builds those buttons, temporarily wrap
        -- CreateFrame to shadow HookScript with a falsy field so those forks
        -- fall back to their vanilla SetScript path. The maintained "brues"
        -- fork handles HookScript itself, so it's detected via the TOC
        -- X-Website tag and left alone. The wrapper is removed as soon as the
        -- actionbar module finishes loading so it affects nothing else.
        local website = GetAddOnMetadata("pfUI", "X-Website")
        if not website or not strfind(website, 'brues') then
            local _createFrame = CreateFrame
            local HookedCreateFrame = function(frameType, name, parent, template)
                local frame = _createFrame(frameType, name, parent, template)
                if frameType == "Button" and strfind(name or "", "pfActionBar") then
                    frame.HookScript = false
                end
                return frame
            end
            hooksecurefunc(pfUI, 'LoadModule', function(frame, moduleName)
                if moduleName == "updatenotify" then
                    CreateFrame = HookedCreateFrame
                elseif moduleName == "actionbar" then
                    CreateFrame = _createFrame
                end
            end)
        end
    end)
end

if C_AddOns.DoesAddOnExist("SpecialTalentUI") then
    EventUtil.ContinueOnAddOnLoaded("SpecialTalentUI", CAPI_ApplyStandardColorGlobals)
end

-- ShaguTweaks libpredict register's TBC events that ClassicAPI backports.
if C_AddOns.DoesAddOnExist("ShaguTweaks") then
    EventUtil.ContinueOnAddOnLoaded("ShaguTweaks", function()
        local libp = ShaguTweaks.libpredict
        if libp then
            libp.sender:UnregisterEvent("COMBAT_LOG_EVENT_UNFILTERED")
            libp.sender:UnregisterEvent("UNIT_SPELLCAST_START")
            libp.sender:UnregisterEvent("UNIT_SPELLCAST_STOP")
            libp.sender:UnregisterEvent("UNIT_SPELLCAST_FAILED")
            libp.sender:UnregisterEvent("UNIT_SPELLCAST_INTERRUPTED")
            libp.sender:UnregisterEvent("UNIT_SPELLCAST_SENT")
        end
    end)
end
