/*
* This file is part of Project SkyFire https://www.projectskyfire.org. 
* See LICENSE.md file for Copyright information
*/

/*
Name: Boss_Anzu
%Complete: 80%
Comment:
Category: Auchindoun, Sethekk Halls
*/

#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "sethekk_halls.h"
#include <set>

enum Says
{
    SAY_SUMMON_BROOD            = 0,
    SAY_SPELL_BOMB              = 1
};

enum Spells
{
    SPELL_PARALYZING_SCREECH    = 40184,
    SPELL_SPELL_BOMB            = 40303,
    SPELL_CYCLONE_OF_FEATHERS   = 40321,
    SPELL_BANISH_SELF           = 42354,
    SPELL_FLESH_RIP             = 40199,
    SPELL_DIVE                  = 40279,    // [lab] charge + physical damage + knockback (5.4.8 SpellEffect: 96/2/98)
    SPELL_BROOD_SCREECH         = 31273     // [lab] Brood of Anzu: -15% attack power, 8s
};

enum Events
{
    EVENT_PARALYZING_SCREECH    = 1,
    EVENT_SPELL_BOMB            = 2,
    EVENT_CYCLONE_OF_FEATHERS   = 3,
    EVENT_SUMMON                = 4,
    EVENT_FLESH_RIP             = 5,        // [lab]
    EVENT_DIVE                  = 6,        // [lab]
    EVENT_BROOD_SCREECH         = 7         // [lab] Brood of Anzu
};

Position const PosSummonBrood[7] =
{
    { -118.1717f, 284.5299f, 121.2287f, 2.775074f },
    { -98.15528f, 293.4469f, 109.2385f, 0.174533f },
    { -99.70160f, 270.1699f, 98.27389f, 6.178465f },
    { -69.25543f, 303.0768f, 97.84479f, 5.532694f },
    { -87.59662f, 263.5181f, 92.70478f, 1.658063f },
    { -73.54323f, 276.6267f, 94.25807f, 2.802979f },
    { -81.70527f, 280.8776f, 44.58830f, 0.526849f }
};

class boss_anzu : public CreatureScript
{
public:
    boss_anzu() : CreatureScript("boss_anzu") { }

    struct boss_anzuAI : public BossAI
    {
        boss_anzuAI(Creature* creature) : BossAI(creature, DATA_ANZU)
        {
            _under33Percent = false;
            _under66Percent = false;
        }

        void Reset() OVERRIDE
        {
            // [lab] restore BossAI reset now that Anzu is a permanent spawn: boss state NOT_STARTED,
            // brood despawned, events cleared on spawn and on every wipe/evade.
            _Reset();
            events.Reset();
            _under33Percent = false;
            _under66Percent = false;
            _brood.clear(); // [lab]
        }

        void EnterCombat(Unit* /*who*/) OVERRIDE
        {
            _EnterCombat();
            events.ScheduleEvent(EVENT_PARALYZING_SCREECH, 14000);
            events.ScheduleEvent(EVENT_CYCLONE_OF_FEATHERS, 5000);
            // [lab] Flesh Rip and Dive are listed in the 5.4.8 Encounter Journal (sections 5249, 5250)
            // but were never scheduled. No MoP-era source gives their timers; these are estimates.
            events.ScheduleEvent(EVENT_FLESH_RIP, std::rand() % 4000 + 8000);
            events.ScheduleEvent(EVENT_DIVE, std::rand() % 6000 + 18000);
        }

        // [lab] Encounter Journal 5253: "Defeating all Brood of Anzu will cause Anzu to cancel the
        // banish effect early." Track the current wave and drop Banish Self when it is cleared.
        void JustSummoned(Creature* summon) OVERRIDE
        {
            BossAI::JustSummoned(summon);
            if (summon->GetEntry() == NPC_BROOD_OF_ANZU)
                _brood.insert(summon->GetGUID());
        }

        void SummonedCreatureDies(Creature* summon, Unit* /*killer*/) OVERRIDE
        {
            OnBroodGone(summon);
        }

        void SummonedCreatureDespawn(Creature* summon) OVERRIDE
        {
            BossAI::SummonedCreatureDespawn(summon);
            OnBroodGone(summon);
        }

        void OnBroodGone(Creature* summon)
        {
            if (summon->GetEntry() != NPC_BROOD_OF_ANZU || !_brood.erase(summon->GetGUID()))
                return;
            if (_brood.empty() && me->HasAura(SPELL_BANISH_SELF))
                me->RemoveAurasDueToSpell(SPELL_BANISH_SELF);
        }

        void JustDied(Unit* /*killer*/) OVERRIDE
        {
            _JustDied();
        }

        void DamageTaken(Unit* /*killer*/, uint32& damage) OVERRIDE
        {
            // [lab] retail thresholds are 75% and 35% ("When Anzu reaches 75% and 35% remaining
            // health, he banishes himself"), not 66/33. Flag names kept to keep the diff small.
            if (me->HealthBelowPctDamaged(35, damage) && !_under33Percent)
            {
                _under33Percent = true;
                Talk(SAY_SUMMON_BROOD);
                events.ScheduleEvent(EVENT_SUMMON, 3000);
            }

            if (me->HealthBelowPctDamaged(75, damage) && !_under66Percent)
            {
                _under66Percent = true;
                Talk(SAY_SUMMON_BROOD);
                events.ScheduleEvent(EVENT_SUMMON, 3000);
            }
        }

        // [lab] Player-only target selection. Every targeted Anzu ability is player-targeted in the
        // 5.4.8 Encounter Journal ("cyclones a player", "claws at a player", "charges a distant
        // player"); selecting from the threat list without playerOnly picked pets / guardians
        // (Cyclone 40321 then fails with SPELL_FAILED_TARGET_NOT_PLAYER).
        Unit* SelectPlayer(float dist = 0.0f)
        {
            return SelectTarget(SELECT_TARGET_RANDOM, 0, dist, true);
        }

        Unit* SelectManaPlayer()
        {
            return SelectTarget(SELECT_TARGET_RANDOM, 0, [](Unit* u)
            {
                return u->GetTypeId() == TypeID::TYPEID_PLAYER && u->getPowerType() == POWER_MANA;
            });
        }

        void UpdateAI(uint32 diff) OVERRIDE
        {
            // [lab] Encounter Journal 5253: "While banished, Anzu will continue to use abilities".
            // UpdateVictim() fails while banished, which used to freeze every timer for the whole
            // banish; keep running the events then and only skip melee.
            bool const banished = me->HasAura(SPELL_BANISH_SELF);
            if (!UpdateVictim() && !banished)
                return;

            events.Update(diff);

            if (me->HasUnitState(UNIT_STATE_CASTING))
                return;

            while (uint32 eventId = events.ExecuteEvent())
            {
                switch (eventId)
                {
                    case EVENT_PARALYZING_SCREECH:
                    {
                        DoCastAOE(SPELL_PARALYZING_SCREECH); // [lab] caster-centred AoE (targets 22/15); works with no victim while banished
                        events.ScheduleEvent(EVENT_PARALYZING_SCREECH, 26000);
                        break;
                    }
                    case EVENT_CYCLONE_OF_FEATHERS:
                    {
                        if (Unit* target = SelectPlayer()) // [lab] players only
                            DoCast(target, SPELL_CYCLONE_OF_FEATHERS);
                        events.ScheduleEvent(EVENT_CYCLONE_OF_FEATHERS, 21000);
                        break;
                    }
                    case EVENT_SUMMON:
                    {
                        // TODO: Add pathing for Brood of Anzu
                        for (uint8 i = 0; i < 7; i++)
                            me->SummonCreature(NPC_BROOD_OF_ANZU, PosSummonBrood[i], TempSummonType::TEMPSUMMON_TIMED_DESPAWN_OUT_OF_COMBAT, 46000);

                        // [lab] triggered: a plain cast is rejected if Anzu is mid-cast (Paralyzing
                        // Screech fires 14s into the fight, right when 66% usually lands), which
                        // silently skipped the first banish while the brood still spawned.
                        DoCast(me, SPELL_BANISH_SELF, true);
                        events.ScheduleEvent(EVENT_SPELL_BOMB, 12000);
                        break;
                    }
                    case EVENT_FLESH_RIP: // [lab] "claws at a player": current victim if it is a player in melee, else a player in melee
                    {
                        Unit* target = me->GetVictim();
                        if (!target || target->GetTypeId() != TypeID::TYPEID_PLAYER || !me->IsWithinMeleeRange(target))
                            target = SelectPlayer(NOMINAL_MELEE_RANGE);
                        if (!target)
                        {
                            events.ScheduleEvent(EVENT_FLESH_RIP, 2000);
                            break;
                        }
                        DoCast(target, SPELL_FLESH_RIP);
                        events.ScheduleEvent(EVENT_FLESH_RIP, std::rand() % 5000 + 15000);
                        break;
                    }
                    case EVENT_DIVE: // [lab] "charges a distant player" - a player at least 8yd away
                    {
                        // banished = rooted, so a charge can't happen; retry shortly after
                        if (me->HasAura(SPELL_BANISH_SELF))
                        {
                            events.ScheduleEvent(EVENT_DIVE, 5000);
                            break;
                        }
                        if (Unit* target = SelectPlayer(-8.0f))
                            DoCast(target, SPELL_DIVE);
                        events.ScheduleEvent(EVENT_DIVE, std::rand() % 10000 + 20000);
                        break;
                    }
                    case EVENT_SPELL_BOMB:
                    {
                        // [lab] pick a mana-using player directly (was: any threat target, then silently
                        // skipped unless it happened to use mana)
                        if (Unit* target = SelectManaPlayer())
                        {
                            DoCast(target, SPELL_SPELL_BOMB);
                            Talk(SAY_SPELL_BOMB, target);
                        }
                        break;
                    }
                    default:
                        break;
                        
                }
            }

            if (!banished) // [lab] pacified while banished
                DoMeleeAttackIfReady();
        }

    private:
        bool _under33Percent;
        bool _under66Percent;
        std::set<uint64> _brood; // [lab] GUIDs of the live brood wave
    };

    CreatureAI* GetAI(Creature* creature) const OVERRIDE
    {
        return GetSethekkHallsAI<boss_anzuAI>(creature);
    }
};

// [lab] Brood of Anzu (23132). Encounter Journal 5255: "The Brood of Anzu screeches, reducing
// attack power of all enemies". It had no AI at all before. Timer is an estimate (no MoP source).
class npc_brood_of_anzu : public CreatureScript
{
public:
    npc_brood_of_anzu() : CreatureScript("npc_brood_of_anzu") { }

    struct npc_brood_of_anzuAI : public ScriptedAI
    {
        npc_brood_of_anzuAI(Creature* creature) : ScriptedAI(creature) { }

        void Reset() OVERRIDE
        {
            events.Reset();
        }

        void EnterCombat(Unit* /*who*/) OVERRIDE
        {
            events.ScheduleEvent(EVENT_BROOD_SCREECH, std::rand() % 4000 + 4000);
        }

        void UpdateAI(uint32 diff) OVERRIDE
        {
            if (!UpdateVictim())
                return;

            events.Update(diff);

            if (events.ExecuteEvent() == EVENT_BROOD_SCREECH)
            {
                DoCast(me, SPELL_BROOD_SCREECH);
                events.ScheduleEvent(EVENT_BROOD_SCREECH, std::rand() % 5000 + 10000);
            }

            DoMeleeAttackIfReady();
        }

    private:
        EventMap events;
    };

    CreatureAI* GetAI(Creature* creature) const OVERRIDE
    {
        return GetSethekkHallsAI<npc_brood_of_anzuAI>(creature);
    }
};

void AddSC_boss_anzu()
{
    new boss_anzu();
    new npc_brood_of_anzu(); // [lab]
}
