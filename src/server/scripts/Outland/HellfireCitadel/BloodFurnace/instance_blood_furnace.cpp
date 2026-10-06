/*
* This file is part of Project SkyFire https://www.projectskyfire.org. 
* See LICENSE.md file for Copyright information
*/

/* ScriptData
SDName: Instance_Blood_Furnace
SD%Complete: 85
SDComment:
SDCategory: Hellfire Citadel, Blood Furnace
EndScriptData */

#include "ScriptMgr.h"
#include "InstanceScript.h"
#include "blood_furnace.h"
#include "CreatureAI.h"
#include "MotionMaster.h"

#define ENTRY_SEWER1                 181823
#define ENTRY_SEWER2                 181766
#define MAX_ENCOUNTER                   3

// [lab] Broggok pre-event completion. The released prisoners used to be thrown straight into
// combat from inside their cages (RemoveFlag + SetInCombatWithZone), relying on generic chase to
// path them out. On this MoP fork that cage-exit chase produces a zero-duration spline: the client
// teleports a "ghost" onto the player while the server leaves the creature in the cell (~70yd away),
// so the orcs read as unkillable/out-of-range. Fix: on release, straight-line walk each prisoner to
// an open muster point first (generatePath=false bypasses the broken exit path), THEN engage once it
// arrives (see ActivatePrisoners/EngagePrisoner/Update). Chasing from the open room works normally.
#define POINT_PRISONER_MUSTER           1
static float const BroggokMusterX = 455.0f;
static float const BroggokMusterY = 65.0f;
static float const BroggokMusterZ = 9.62f;

class instance_blood_furnace : public InstanceMapScript
{
    public:
        instance_blood_furnace()
            : InstanceMapScript("instance_blood_furnace", 542) { }

        struct instance_blood_furnace_InstanceMapScript : public InstanceScript
        {
            instance_blood_furnace_InstanceMapScript(Map* map) : InstanceScript(map) { }

            uint64 The_MakerGUID;
            uint64 BroggokGUID;
            uint64 Kelidan_The_BreakerGUID;

            uint64 Door1GUID;
            uint64 Door2GUID;
            uint64 Door3GUID;
            uint64 Door4GUID;
            uint64 Door5GUID;
            uint64 Door6GUID;

            uint64 PrisonCell1GUID;
            uint64 PrisonCell2GUID;
            uint64 PrisonCell3GUID;
            uint64 PrisonCell4GUID;
            uint64 PrisonCell5GUID;
            uint64 PrisonCell6GUID;
            uint64 PrisonCell7GUID;
            uint64 PrisonCell8GUID;

            std::set<uint64> PrisonersCell5;
            std::set<uint64> PrisonersCell6;
            std::set<uint64> PrisonersCell7;
            std::set<uint64> PrisonersCell8;

            uint8 PrisonerCounter5;
            uint8 PrisonerCounter6;
            uint8 PrisonerCounter7;
            uint8 PrisonerCounter8;

            uint64 BroggokLeverGUID;

            std::set<uint64> MusteringPrisoners;   // [lab] released prisoners walking to the muster point
            uint32 MusterSafetyTimer;              // [lab] fallback: force-engage anyone still en route

            uint32 m_auiEncounter[MAX_ENCOUNTER];
            std::string str_data;

            void Initialize() OVERRIDE
            {
                memset(&m_auiEncounter, 0, sizeof(m_auiEncounter));

                The_MakerGUID = 0;
                BroggokGUID = 0;
                Kelidan_The_BreakerGUID = 0;

                Door1GUID = 0;
                Door2GUID = 0;
                Door3GUID = 0;
                Door4GUID = 0;
                Door5GUID = 0;
                Door6GUID = 0;

                PrisonCell1GUID = 0;
                PrisonCell2GUID = 0;
                PrisonCell3GUID = 0;
                PrisonCell4GUID = 0;
                PrisonCell5GUID = 0;
                PrisonCell6GUID = 0;
                PrisonCell7GUID = 0;
                PrisonCell8GUID = 0;

                PrisonersCell5.clear();
                PrisonersCell6.clear();
                PrisonersCell7.clear();
                PrisonersCell8.clear();

                PrisonerCounter5 = 0;
                PrisonerCounter6 = 0;
                PrisonerCounter7 = 0;
                PrisonerCounter8 = 0;

                BroggokLeverGUID = 0;

                MusteringPrisoners.clear();        // [lab]
                MusterSafetyTimer = 0;             // [lab]
            }

            void OnCreatureCreate(Creature* creature) OVERRIDE
            {
                switch (creature->GetEntry())
                {
                    case 17381:
                        The_MakerGUID = creature->GetGUID();
                        break;
                    case 17380:
                        BroggokGUID = creature->GetGUID();
                        break;
                    case 17377:
                        Kelidan_The_BreakerGUID = creature->GetGUID();
                        break;
                    case 17398:
                        StorePrisoner(creature);
                        break;
                }
            }

            void OnUnitDeath(Unit* unit) OVERRIDE
            {
                if (unit && unit->GetTypeId() == TypeID::TYPEID_UNIT && unit->GetEntry() == 17398)
                    PrisonerDied(unit->GetGUID());
            }

            void OnGameObjectCreate(GameObject* go) OVERRIDE
            {
                 if (go->GetEntry() == 181766)                //Final exit door
                     Door1GUID = go->GetGUID();
                 if (go->GetEntry() == 181811)               //The Maker Front door
                     Door2GUID = go->GetGUID();
                 if (go->GetEntry() == 181812)                //The Maker Rear door
                     Door3GUID = go->GetGUID();
                 if (go->GetEntry() == 181822)               //Broggok Front door
                     Door4GUID = go->GetGUID();
                 if (go->GetEntry() == 181819)               //Broggok Rear door
                     Door5GUID = go->GetGUID();
                 if (go->GetEntry() == 181823)               //Kelidan exit door
                     Door6GUID = go->GetGUID();

                 if (go->GetEntry() == 181813)               //The Maker prison cell front right
                     PrisonCell1GUID = go->GetGUID();
                 if (go->GetEntry() == 181814)               //The Maker prison cell back right
                     PrisonCell2GUID = go->GetGUID();
                 if (go->GetEntry() == 181816)               //The Maker prison cell front left
                     PrisonCell3GUID = go->GetGUID();
                 if (go->GetEntry() == 181815)               //The Maker prison cell back left
                     PrisonCell4GUID = go->GetGUID();
                 if (go->GetEntry() == 181821)               //Broggok prison cell front right
                     PrisonCell5GUID = go->GetGUID();
                 if (go->GetEntry() == 181818)               //Broggok prison cell back right
                     PrisonCell6GUID = go->GetGUID();
                 if (go->GetEntry() == 181820)               //Broggok prison cell front left
                     PrisonCell7GUID = go->GetGUID();
                 if (go->GetEntry() == 181817)               //Broggok prison cell back left
                     PrisonCell8GUID = go->GetGUID();

                 if (go->GetEntry() == 181982)
                     BroggokLeverGUID = go->GetGUID();       //Broggok lever
            }

            uint64 GetData64(uint32 data) const OVERRIDE
            {
                switch (data)
                {
                     case DATA_THE_MAKER:            return The_MakerGUID;
                     case DATA_BROGGOK:              return BroggokGUID;
                     case DATA_KELIDAN_THE_MAKER:    return Kelidan_The_BreakerGUID;
                     case DATA_DOOR1:                return Door1GUID;
                     case DATA_DOOR2:                return Door2GUID;
                     case DATA_DOOR3:                return Door3GUID;
                     case DATA_DOOR4:                return Door4GUID;
                     case DATA_DOOR5:                return Door5GUID;
                     case DATA_DOOR6:                return Door6GUID;
                     case DATA_PRISON_CELL1:         return PrisonCell1GUID;
                     case DATA_PRISON_CELL2:         return PrisonCell2GUID;
                     case DATA_PRISON_CELL3:         return PrisonCell3GUID;
                     case DATA_PRISON_CELL4:         return PrisonCell4GUID;
                     case DATA_PRISON_CELL5:         return PrisonCell5GUID;
                     case DATA_PRISON_CELL6:         return PrisonCell6GUID;
                     case DATA_PRISON_CELL7:         return PrisonCell7GUID;
                     case DATA_PRISON_CELL8:         return PrisonCell8GUID;
                     case DATA_BROGGOK_LEVER:        return BroggokLeverGUID;
                }
                return 0;
            }

            void SetData(uint32 type, uint32 data) OVERRIDE
            {
                 switch (type)
                 {
                     case TYPE_THE_MAKER_EVENT:
                         m_auiEncounter[0] = data;
                         break;
                     case TYPE_BROGGOK_EVENT:
                         m_auiEncounter[1] = data;
                         UpdateBroggokEvent(data);
                         break;
                     case TYPE_KELIDAN_THE_BREAKER_EVENT:
                         m_auiEncounter[2] = data;
                         break;
                 }

                if (data == DONE)
                {
                    OUT_SAVE_INST_DATA;

                    std::ostringstream saveStream;
                    saveStream << m_auiEncounter[0] << ' ' << m_auiEncounter[1] << ' ' << m_auiEncounter[2];

                    str_data = saveStream.str();

                    SaveToDB();
                    OUT_SAVE_INST_DATA_COMPLETE;
                }
            }

            uint32 GetData(uint32 type) const OVERRIDE
            {
                switch (type)
                {
                    case TYPE_THE_MAKER_EVENT:             return m_auiEncounter[0];
                    case TYPE_BROGGOK_EVENT:               return m_auiEncounter[1];
                    case TYPE_KELIDAN_THE_BREAKER_EVENT:   return m_auiEncounter[2];
                }
                return 0;
            }

            const char* Save()
            {
                return str_data.c_str();
            }

            void Load(const char* in) OVERRIDE
            {
                if (!in)
                {
                    OUT_LOAD_INST_DATA_FAIL;
                    return;
                }

                OUT_LOAD_INST_DATA(in);

                std::istringstream loadStream(in);
                loadStream >> m_auiEncounter[0] >> m_auiEncounter[1] >> m_auiEncounter[2];

                for (uint8 i = 0; i < MAX_ENCOUNTER; ++i)
                    if (m_auiEncounter[i] == IN_PROGRESS || m_auiEncounter[i] == FAIL)
                        m_auiEncounter[i] = NOT_STARTED;

                OUT_LOAD_INST_DATA_COMPLETE;
            }

            void UpdateBroggokEvent(uint32 data)
            {
                switch (data)
                {
                    case IN_PROGRESS:
                        ActivateCell(DATA_PRISON_CELL5);
                        HandleGameObject(Door4GUID, false);
                        break;
                    case NOT_STARTED:
                        ResetPrisons();
                        HandleGameObject(Door5GUID, false);
                        HandleGameObject(Door4GUID, true);
                        if (GameObject* lever = instance->GetGameObject(BroggokLeverGUID))
                            lever->Respawn();
                        break;
                }
            }

            void ResetPrisons()
            {
                PrisonerCounter5 = PrisonersCell5.size();
                ResetPrisoners(PrisonersCell5);
                HandleGameObject(PrisonCell5GUID, false);

                PrisonerCounter6 = PrisonersCell6.size();
                ResetPrisoners(PrisonersCell6);
                HandleGameObject(PrisonCell6GUID, false);

                PrisonerCounter7 = PrisonersCell7.size();
                ResetPrisoners(PrisonersCell7);
                HandleGameObject(PrisonCell7GUID, false);

                PrisonerCounter8 = PrisonersCell8.size();
                ResetPrisoners(PrisonersCell8);
                HandleGameObject(PrisonCell8GUID, false);
            }

            void ResetPrisoners(const std::set<uint64>& prisoners)
            {
                for (std::set<uint64>::const_iterator i = prisoners.begin(); i != prisoners.end(); ++i)
                    if (Creature* prisoner = instance->GetCreature(*i))
                        ResetPrisoner(prisoner);
            }

            void ResetPrisoner(Creature* prisoner)
            {
                if (!prisoner->IsAlive())
                    prisoner->Respawn(true);
                prisoner->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_NON_ATTACKABLE);
            }

            void StorePrisoner(Creature* creature)
            {
                float posX = creature->GetPositionX();
                float posY = creature->GetPositionY();

                if (posX >= 405.0f && posX <= 423.0f)
                {
                    if (posY >= 106.0f && posY <= 123.0f)
                    {
                        PrisonersCell5.insert(creature->GetGUID());
                        ++PrisonerCounter5;
                    }
                    else if (posY >= 76.0f && posY <= 91.0f)
                    {
                        PrisonersCell6.insert(creature->GetGUID());
                        ++PrisonerCounter6;
                    }
                    else return;
                }
                else if (posX >= 490.0f && posX <= 506.0f)
                {
                    if (posY >= 106.0f && posY <= 123.0f)
                    {
                        PrisonersCell7.insert(creature->GetGUID());
                        ++PrisonerCounter7;
                    }
                    else if (posY >= 76.0f && posY <= 91.0f)
                    {
                        PrisonersCell8.insert(creature->GetGUID());
                        ++PrisonerCounter8;
                    }
                    else
                        return;
                }
                else
                    return;

                ResetPrisoner(creature);
            }

            // [lab] True when every prisoner in the cell EXCEPT the one currently dying is dead.
            // At the OnUnitDeath hook the dying unit still reports IsAlive()==true (its death-state
            // is applied after the dispatch), so it must be excluded from the check.
            bool AllPrisonersDead(const std::set<uint64>& prisoners, uint64 dyingGuid)
            {
                for (std::set<uint64>::const_iterator i = prisoners.begin(); i != prisoners.end(); ++i)
                {
                    if (*i == dyingGuid)
                        continue;
                    Creature* prisoner = instance->GetCreature(*i);
                    if (prisoner && prisoner->IsAlive())
                        return false;
                }
                return true;
            }

            void PrisonerDied(uint64 guid)
            {
                // [lab] State-based wave advance. The old PrisonerCounterN (seeded by ++ in
                // StorePrisoner on create, overwritten by ResetPrisons) drifted out of sync -- seeded
                // higher than the real orc count via a double OnCreatureCreate/respawn or a Broggok
                // evade-reset on the lever's SetInCombatWithZone -- so it never reached 0 and the next
                // cell never opened (confirmed in heroic: all 5 cell-5 orcs dead, cell 6 stayed caged).
                // Checking live alive-state instead is immune to any miscount.
                if (PrisonersCell5.find(guid) != PrisonersCell5.end())
                {
                    if (AllPrisonersDead(PrisonersCell5, guid)) ActivateCell(DATA_PRISON_CELL6);
                }
                else if (PrisonersCell6.find(guid) != PrisonersCell6.end())
                {
                    if (AllPrisonersDead(PrisonersCell6, guid)) ActivateCell(DATA_PRISON_CELL7);
                }
                else if (PrisonersCell7.find(guid) != PrisonersCell7.end())
                {
                    if (AllPrisonersDead(PrisonersCell7, guid)) ActivateCell(DATA_PRISON_CELL8);
                }
                else if (PrisonersCell8.find(guid) != PrisonersCell8.end())
                {
                    if (AllPrisonersDead(PrisonersCell8, guid)) ActivateCell(DATA_DOOR5);
                }
            }

            void ActivateCell(uint8 id)
            {
                switch (id)
                {
                    case DATA_PRISON_CELL5:
                        HandleGameObject(PrisonCell5GUID, true);
                        ActivatePrisoners(PrisonersCell5);
                        break;
                    case DATA_PRISON_CELL6:
                        HandleGameObject(PrisonCell6GUID, true);
                        ActivatePrisoners(PrisonersCell6);
                        break;
                    case DATA_PRISON_CELL7:
                        HandleGameObject(PrisonCell7GUID, true);
                        ActivatePrisoners(PrisonersCell7);
                        break;
                    case DATA_PRISON_CELL8:
                        HandleGameObject(PrisonCell8GUID, true);
                        ActivatePrisoners(PrisonersCell8);
                        break;
                    case DATA_DOOR5:
                        HandleGameObject(Door5GUID, true);
                        if (Creature* broggok = instance->GetCreature(BroggokGUID))
                            broggok->AI()->DoAction(ACTION_ACTIVATE_BROGGOK);
                        break;
                }
            }

            void ActivatePrisoners(const std::set<uint64>& prisoners)
            {
                // [lab] Stage-then-engage. Keep each prisoner immune + passive (ResetPrisoner already
                // set the immune flags) and straight-line walk it to the open muster point. The
                // generatePath=false is the crux: it skips the broken cage-exit pathfinding that
                // yielded a zero-duration spline. Prisoners "release" (become attackable + aggressive)
                // on arrival, in Update().
                for (std::set<uint64>::const_iterator i = prisoners.begin(); i != prisoners.end(); ++i)
                    if (Creature* prisoner = instance->GetCreature(*i))
                    {
                        prisoner->SetReactState(REACT_PASSIVE);
                        prisoner->GetMotionMaster()->MovePoint(POINT_PRISONER_MUSTER, BroggokMusterX, BroggokMusterY, BroggokMusterZ, false);
                        MusteringPrisoners.insert(*i);
                    }
                MusterSafetyTimer = 15000;         // [lab] fallback window if someone can't reach the point
            }

            void EngagePrisoner(Creature* prisoner)
            {
                // [lab] clear the cage immunities and let it fight from the open room
                prisoner->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_NON_ATTACKABLE);
                prisoner->SetReactState(REACT_AGGRESSIVE);
                prisoner->SetInCombatWithZone();
            }

            void Update(uint32 diff) OVERRIDE
            {
                if (MusteringPrisoners.empty())
                    return;

                // [lab] force-engage fallback so a prisoner that can't reach the point never deadlocks the wave
                bool force = false;
                if (MusterSafetyTimer <= diff)
                    force = true;
                else
                    MusterSafetyTimer -= diff;

                for (std::set<uint64>::iterator i = MusteringPrisoners.begin(); i != MusteringPrisoners.end(); )
                {
                    Creature* prisoner = instance->GetCreature(*i);
                    if (!prisoner || !prisoner->IsAlive())
                    {
                        MusteringPrisoners.erase(i++);
                        continue;
                    }

                    if (force || prisoner->GetExactDist2d(BroggokMusterX, BroggokMusterY) < 4.0f)
                    {
                        EngagePrisoner(prisoner);
                        MusteringPrisoners.erase(i++);
                    }
                    else
                        ++i;
                }
            }
        };

        InstanceScript* GetInstanceScript(InstanceMap* map) const OVERRIDE
        {
            return new instance_blood_furnace_InstanceMapScript(map);
        }
};

void AddSC_instance_blood_furnace()
{
    new instance_blood_furnace();
}

