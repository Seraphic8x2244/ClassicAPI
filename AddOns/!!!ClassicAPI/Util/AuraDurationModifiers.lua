-- Server-side DoT-duration adjustments the 1.12 client is never told about.
--
-- ClassicAPI derives C_UnitAuras expirationTime for a debuff on another unit
-- from the cast packet (SMSG_SPELL_GO). The vanilla server never retransmits a
-- LATER server-side duration change on a non-self unit -- verified in the
-- server source: the change runs through SetAuraDuration / RefreshHolder, and
-- the only duration packet (SMSG_UPDATE_AURA_DURATION) is self-scoped to the
-- aura-bearer, so an observing caster hears nothing (and on a mob the packet
-- isn't even built). We can't hear the change, but we DO see the *triggering*
-- cast, so C_UnitAuras.RegisterAuraDurationModifier mirrors the server's edit
-- off that trigger.
--
-- A trigger is matched by exact spellID (stable across ranks -- these are the
-- IDs the server binds its spell script to); the affected aura by
-- SpellFamilyName + a family-flag overlap (rank-proof, exactly how the server
-- scripts locate it). Flags below are from the client's Spell.dbc. When the
-- trigger is a whole class of spells rather than a named ability,
-- RegisterAuraDurationModifierByTrigger matches it by SpellFamilyName + school
-- index instead (see Shadow Weaving below).
--
-- NOT included on purpose: Carnage's Rip/Rake refresh fires behind a
-- server-side probability roll on Ferocious Bite. The client can't know the
-- roll's outcome, so inferring it would show wrong timers. If you accept that,
-- register it yourself, e.g. (DRUID = 7):
--   C_UnitAuras.RegisterAuraDurationModifier(<ferociousBiteID>, 7, tonumber("800000",16), 108, "refresh") -- Rip
--   C_UnitAuras.RegisterAuraDurationModifier(<ferociousBiteID>, 7, tonumber("1000",16),   494, "refresh") -- Rake
-- Conflagrate's *full*-consume variant (other servers) removes Immolate
-- outright; that clears the aura slot and ClassicAPI evicts it via the normal
-- removal path, so the reduce rule below is harmless there and correct on
-- Turtle (where Immolate keeps ticking with 3s shaved off).

if TURTLE_WOW_VERSION then
    local WARLOCK, SHAMAN = 5, 11
    -- Lua 5.0 has no 0x hex literals; tonumber(hex, 16) keeps the flags readable.
    local IMMOLATE_FLAG   = tonumber("4", 16)        -- CF_WARLOCK_IMMOLATE  (bit 2)
    local FLAMESHOCK_FLAG = tonumber("10000000", 16) -- CF_SHAMAN_FLAME_SHOCK (bit 28)

    -- Conflagrate (all ranks) -> shave 3s off the caster's Immolate.
    local CONFLAGRATE = { 17962, 18930, 18931, 18932 }
    for i = 1, table.getn(CONFLAGRATE) do
        C_UnitAuras.RegisterAuraDurationModifier(CONFLAGRATE[i], WARLOCK, IMMOLATE_FLAG, 0, "reduce", 3)
    end

    -- Molten Blast (all ranks) -> refresh the caster's Flame Shock to full.
    local MOLTEN_BLAST = { 36916, 36917, 36918, 36919, 36920, 36921 }
    for i = 1, table.getn(MOLTEN_BLAST) do
        C_UnitAuras.RegisterAuraDurationModifier(MOLTEN_BLAST[i], SHAMAN, FLAMESHOCK_FLAG, 0, "refresh")
    end
end
-- Shadow Weaving (priest): any shadow-school priest cast refreshes the caster's
-- Shadow Vulnerability (15258, 15s, stacks to 5). The 1->5 stack changes fire
-- OnAuraStacksChanged (already handled); the 5->5 refresh is the silent blind
-- spot this covers. Matched by family(6) + school(5) so it spans every priest
-- shadow spell/rank incl. Turtle additions, excluding holy/discipline casts
-- (Smite, heals) which are priest-family but not shadow-school.
--
-- Gated on the 5/5 talent (15334), where the proc is 100% -> deterministic;
-- below 5/5 it's chance-based and inferring on every cast would over-refresh.
-- Deferred to SPELLS_CHANGED because talent/spell state isn't ready at load,
-- and re-checked there so a respec INTO 5/5 registers without a reload.
-- Caveat: DoT *ticks* (SW:P / Devouring Plague) refresh it server-side too but
-- emit no cast packet, so a pure DoT-only phase (no direct casts) under-counts.
local PRIEST, SHADOW = 6, 5
local SW_FLAG, SW_ICON = tonumber("4000000", 16), 9 -- 15258 SpellFamilyFlags + icon

EventUtil.RegisterOnceFrameEventAndCallback("SPELLS_CHANGED", function()
    if IsPlayerSpell(15334) then
        C_UnitAuras.RegisterAuraDurationModifierByTrigger(PRIEST, SHADOW, PRIEST, SW_FLAG, SW_ICON, "refresh")
    end
end)
