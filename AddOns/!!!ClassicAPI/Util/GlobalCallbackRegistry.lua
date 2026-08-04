-- Backport of 3.3.5 FrameXML/GlobalCallbackRegistry.lua to vanilla 1.12 / Lua 5.0.
--
-- Implementation differences from the 3.3.5 source:
--   - 3.3.5 ref-counts WoW frame events via a SetAttribute/OnAttributeChanged
--     dance because attribute mutations are taint-safe in the secure-template
--     model. 1.12 has neither secure templates nor SetAttribute on frames,
--     so we keep an ordinary {[event]=count} table and Register/Unregister
--     on 0<->1 transitions.

EventRegistry = CreateFromMixins(CallbackRegistryMixin)

function EventRegistry:OnLoad()
    CallbackRegistryMixin.OnLoad(self)
    self:SetUndefinedEventsAllowed(true)

    self.eventCounts = {}
    self.frameEventFrame = CreateFrame("Frame")
    self.frameEventFrame.registry = self
    self.frameEventFrame:SetScript("OnEvent", function()
        -- 1.12 globals: `event` is the event name; `arg1..argN` are payload.
        -- We don't repack them here; CallbackRegistryMixin:TriggerEvent uses
        -- Lua-5.0 varargs, so we forward through a small wrapper.
        self:DispatchGameEvent(event, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9)
    end)

    -- Stop delivering events once logout begins: a callback firing during the
    -- engine's teardown can touch state that's already gone. A dedicated frame
    -- owns PLAYER_LOGOUT so this fires regardless of what consumers registered
    -- — folding the check into frameEventFrame's OnEvent would depend on the
    -- ref-counted event set still holding PLAYER_LOGOUT at logout time.
    self.logoutFrame = CreateFrame("Frame")
    self.logoutFrame:RegisterEvent("PLAYER_LOGOUT")
    self.logoutFrame:SetScript("OnEvent", function()
        self.frameEventFrame:UnregisterAllEvents()
        self.frameEventFrame:SetScript("OnEvent", nil)
    end)
end

-- Helper so we can forward up to nine event args (the most any vanilla
-- event currently emits) into TriggerEvent's vararg machinery.
function EventRegistry:DispatchGameEvent(event, ...)
    self:TriggerEvent(event, unpack(arg))
end

function EventRegistry:RegisterFrameEvent(frameEvent)
    local n = (self.eventCounts[frameEvent] or 0) + 1
    self.eventCounts[frameEvent] = n
    if n == 1 then
        self.frameEventFrame:RegisterEvent(frameEvent)
    end
end

function EventRegistry:UnregisterFrameEvent(frameEvent)
    local n = self.eventCounts[frameEvent] or 0
    if n > 0 then
        n = n - 1
        self.eventCounts[frameEvent] = n
        if n == 0 then
            self.frameEventFrame:UnregisterEvent(frameEvent)
        end
    end
end

function EventRegistry:RegisterFrameEventAndCallback(frameEvent, ...)
    self:RegisterFrameEvent(frameEvent)
    return self:RegisterCallback(frameEvent, unpack(arg))
end

local function CreateCallbackHandle(cbr, cbrHandle, frameEvent)
    local handle = {
        Unregister = function()
            cbr:UnregisterFrameEvent(frameEvent)
            cbrHandle:Unregister()
        end,
    }
    return handle
end

function EventRegistry:RegisterFrameEventAndCallbackWithHandle(frameEvent, ...)
    self:RegisterFrameEvent(frameEvent)
    local cbrHandle = self:RegisterCallbackWithHandle(frameEvent, unpack(arg))
    return CreateCallbackHandle(self, cbrHandle, frameEvent)
end

function EventRegistry:UnregisterFrameEventAndCallback(frameEvent, ...)
    self:UnregisterFrameEvent(frameEvent)
    self:UnregisterCallback(frameEvent, unpack(arg))
end

function EventRegistry:GetEventCounts(...)
    local counts = {}
    for i = 1, arg.n do
        local frameEvent = arg[i]
        local count = self.eventCounts[frameEvent] or "?"
        table.insert(counts, frameEvent .. " : " .. tostring(count))
    end

    return table.concat(counts, "\n")
end

EventRegistry:OnLoad()
