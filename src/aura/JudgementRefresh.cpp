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

// Paladin judgement melee refresh — client mirror of the server's white-swing
// rule (pfUI#45: the judgement debuff's timer ran to zero while the debuff
// stayed up through the paladin's auto-attacks).
//
// The server rule (tortoise-wow Unit::DealMeleeDamage, vmangos lineage): a
// melee swing that dealt ANY damage (`totalDamage > 0` — misses/dodges/parries
// deal none) refreshes every aura on the victim that the attacker cast whose
// spell is SPELLFAMILY_PALADIN with SPELL_ATTR_EX3_ALWAYS_HIT — the judgement
// marker (all 14 Judgement of Light/Wisdom/Justice/Crusader debuff ranks carry
// it, verified in the client Spell.dbc). RefreshHolder resets the holder to
// its own full duration.
//
// The edit itself is packet-silent (UpdateAuraDuration is self-scoped — the
// same blindness every Turtle::DurationMods rule works around), but its
// TRIGGER is not: DealMeleeDamage's only caller is Unit::AttackerStateUpdate
// (white swings), which then broadcasts SMSG_ATTACKERSTATEUPDATE to everyone
// in range — attacker, victim, and totalDamage are its first fields (wire
// format verified against the server's own SendAttackStateUpdate writer; see
// Offsets.h at SMSG_ATTACKERSTATEUPDATE). So the swing packet is an exact
// client-side stand-in for the refresh, for ANY paladin's judgements — ours
// or a raid member's.
//
// Ability damage (Crusader/Holy Strike) does NOT reach that server block —
// melee-class spell damage never routes through DealMeleeDamage on this core
// — so no rule is registered for it. Re-judging is covered separately: the
// judgement debuff is cast as a real triggered spell, its SMSG_SPELL_GO
// refreshes the cache entry via StoreFromCast.
//
// Not gated on Turtle::Detected(): the block is vmangos-lineage core (the
// classic "judgements are refreshed by the paladin's melee attacks"
// mechanic), and the refresh only ever touches existing judgement entries
// with a matching known caster — on a server without the mechanic the debuff
// would genuinely expire, removing the entry via OnAuraRemoved before drift
// could show.

#include "Offsets.h"
#include "aura/Source.h"
#include "net/PacketDispatch.h"
#include "net/PacketReader.h"

#include <cstdint>

namespace Aura::JudgementRefresh {

namespace {

void OnPacket(uint32_t opcode, Net::CDataStore *packet) {
    if (opcode != Offsets::SMSG_ATTACKERSTATEUPDATE || packet == nullptr)
        return;
    Net::Read<uint32_t>(packet); // hitInfo — the damage gate below subsumes it
    const uint64_t attacker = Net::ReadPackedGuid(packet);
    const uint64_t victim = Net::ReadPackedGuid(packet);
    const uint32_t totalDamage = Net::Read<uint32_t>(packet);
    // The server's exact gate: any nonzero melee damage refreshes; a fully
    // avoided/absorbed swing (totalDamage 0) does not.
    if (totalDamage == 0 || attacker == 0 || victim == 0)
        return;
    Aura::Source::RefreshJudgements(victim, attacker);
}

const Net::PacketDispatch::AutoSubscribe _sub{&OnPacket};

} // namespace

} // namespace Aura::JudgementRefresh
