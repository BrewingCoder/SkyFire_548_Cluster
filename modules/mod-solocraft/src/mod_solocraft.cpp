/*
 * mod-solocraft (SkyFire 5.4.8 port)
 *
 * Scales a player's stats up when they enter an instance under-manned, so a
 * solo player or a small group can clear dungeon/raid content built for a full
 * group. Buff is applied on entering an instance and removed on leaving.
 *
 * This is a port of the AzerothCore "mod-solocraft" module to the SkyFire
 * 5.4.8 core. Original concept & implementation: the AzerothCore community
 * (https://github.com/azerothcore). Licensed under the GNU AGPL v3 (same as
 * the original). See README.md for attribution and the list of changes.
 */

#include "Config.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "Map.h"
#include "Chat.h"
#include "Unit.h"
#include "Creature.h"
#include "Common.h"
#include "SharedDefines.h"
#include <unordered_map>

namespace
{
    struct SolocraftConfig
    {
        bool  Enable   = false;
        bool  Dungeons = true;
        bool  Raids    = true;
        float Balance  = 0.6f;    // fraction of linear (maxPlayers/actual) scaling applied
        float MaxMult  = 10.0f;   // hard cap on the stat multiplier
        float HealthMult = 1.0f;  // EXTRA health multiplier applied on top of the core multiplier
        uint32 MinLevel = 1;
        bool  ScaleHealth = true;
        bool  Announce = true;
        bool  InstanceCorpsePersist = true;  // keep mob corpses in instances until reset (loot after a death / clear-then-loot)
    };

    SolocraftConfig g_cfg;

    // TOTAL_PCT amounts currently applied to a player (so we remove exactly what we added).
    struct AppliedMods { float statPct; float healthPct; };
    std::unordered_map<uint32, AppliedMods> g_applied;

    void LoadConfig()
    {
        g_cfg.Enable      = sConfigMgr->GetBoolDefault("Solocraft.Enable", false);
        g_cfg.Dungeons    = sConfigMgr->GetBoolDefault("Solocraft.Dungeons", true);
        g_cfg.Raids       = sConfigMgr->GetBoolDefault("Solocraft.Raids", true);
        g_cfg.Balance     = sConfigMgr->GetFloatDefault("Solocraft.Balance", 0.6f);
        g_cfg.MaxMult     = sConfigMgr->GetFloatDefault("Solocraft.MaxMultiplier", 10.0f);
        g_cfg.HealthMult  = sConfigMgr->GetFloatDefault("Solocraft.HealthMultiplier", 1.0f);
        g_cfg.MinLevel    = uint32(sConfigMgr->GetIntDefault("Solocraft.MinLevel", 1));
        g_cfg.ScaleHealth = sConfigMgr->GetBoolDefault("Solocraft.ScaleHealth", true);
        g_cfg.Announce    = sConfigMgr->GetBoolDefault("Solocraft.Announce", true);
        g_cfg.InstanceCorpsePersist = sConfigMgr->GetBoolDefault("Solocraft.InstanceCorpsePersist", true);

        if (g_cfg.Balance < 0.0f) g_cfg.Balance = 0.0f;
        if (g_cfg.MaxMult < 1.0f) g_cfg.MaxMult = 1.0f;
        if (g_cfg.HealthMult < 0.1f) g_cfg.HealthMult = 0.1f;   // keep health sane; this is an EXTRA multiplier on top of the core mult
    }

    // statPct scales the five primary stats; healthPct scales max health (an independent amount
    // so health can be boosted above and beyond the core multiplier via Solocraft.HealthMultiplier).
    void ApplyMods(Player* player, float statPct, float healthPct, bool apply)
    {
        for (uint8 s = STAT_STRENGTH; s < MAX_STATS; ++s)
            player->HandleStatModifier(UnitMods(UNIT_MOD_STAT_START + s), TOTAL_PCT, statPct, apply);
        if (g_cfg.ScaleHealth)
            player->HandleStatModifier(UNIT_MOD_HEALTH, TOTAL_PCT, healthPct, apply);
    }

    void RemoveBuff(Player* player)
    {
        if (!player)
            return;
        std::unordered_map<uint32, AppliedMods>::iterator it = g_applied.find(player->GetGUIDLow());
        if (it == g_applied.end())
            return;
        ApplyMods(player, it->second.statPct, it->second.healthPct, false);
        g_applied.erase(it);
    }

    // Evaluate the player's current map and (re)apply the correct buff.
    void Evaluate(Player* player)
    {
        if (!player)
            return;

        RemoveBuff(player);   // always clear first (handles leaving / re-entering / regroup)

        if (!g_cfg.Enable || player->getLevel() < g_cfg.MinLevel)
            return;

        Map* map = player->GetMap();
        if (!map || !map->IsDungeon())
            return;   // IsDungeon() is true for both 5-man instances and raids

        bool isRaid = map->IsRaid();
        if (isRaid ? !g_cfg.Raids : !g_cfg.Dungeons)
            return;

        InstanceMap* im = map->ToInstanceMap();
        if (!im)
            return;

        uint32 maxPlayers = im->GetMaxPlayers();
        uint32 actual = map->GetPlayersCountExceptGMs();
        if (maxPlayers <= 1 || actual < 1 || actual >= maxPlayers)
            return;   // full group (or unknown) -> no scaling needed

        float scale = float(maxPlayers) / float(actual);
        float mult = 1.0f + (scale - 1.0f) * g_cfg.Balance;
        if (mult > g_cfg.MaxMult)
            mult = g_cfg.MaxMult;
        if (mult <= 1.0f)
            return;

        float statPct   = (mult - 1.0f) * 100.0f;              // primary stats -> x mult
        float healthMult = mult * g_cfg.HealthMult;            // health gets an extra independent float on top
        float healthPct = (healthMult - 1.0f) * 100.0f;        // TOTAL_PCT amount for max health
        ApplyMods(player, statPct, healthPct, true);
        g_applied[player->GetGUIDLow()] = { statPct, healthPct };

        if (g_cfg.ScaleHealth)
            player->SetFullHealth();

        if (g_cfg.Announce && player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff00ff00[Solocraft]|r scaled your stats x%.1f (health x%.1f) for this %s (%u/%u players).",
                mult, healthMult, isRaid ? "raid" : "dungeon", actual, maxPlayers);
    }
}

class mod_solocraft_playerscript : public PlayerScript
{
public:
    mod_solocraft_playerscript() : PlayerScript("mod_solocraft_playerscript") { }

    void OnMapChanged(Player* player) override { Evaluate(player); }
    void OnLogin(Player* player, bool /*firstLogin*/) override { Evaluate(player); }

    // Keep mob corpses (and their loot) alive in instances until the instance resets, so a solo
    // player can run back after a death and still loot, and can clear a room before looting it.
    // setDeathState(JUST_DIED) has already set the normal corpse-remove time by the time this fires;
    // we push it out so the corpse does not decay while players are still inside the instance.
    // Note: only fires for direct player kills (pet/guardian killing blows go through a different path).
    void OnCreatureKill(Player* /*killer*/, Creature* killed) override
    {
        if (!g_cfg.Enable || !g_cfg.InstanceCorpsePersist || !killed)
            return;
        Map* map = killed->GetMap();
        if (!map || !map->Instanceable())
            return;
        killed->SetCorpseRemoveTime(time(NULL) + WEEK);   // effectively "until instance reset/unload"
    }
    void OnLogout(Player* player) override
    {
        if (player)
            g_applied.erase(player->GetGUIDLow());   // runtime mods vanish on logout anyway
    }
};

class mod_solocraft_worldscript : public WorldScript
{
public:
    mod_solocraft_worldscript() : WorldScript("mod_solocraft_worldscript") { }
    void OnConfigLoad(bool /*reload*/) override { LoadConfig(); }
};

class mod_solocraft_commandscript : public CommandScript
{
public:
    mod_solocraft_commandscript() : CommandScript("mod_solocraft_commandscript") { }

    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> sub =
        {
            { "status", rbac::RBAC_PERM_COMMAND_SERVER_INFO, false, &HandleStatus, "", },
            { "on",     rbac::RBAC_PERM_COMMAND_SERVER_SET,  true,  &HandleOn,     "", },
            { "off",    rbac::RBAC_PERM_COMMAND_SERVER_SET,  true,  &HandleOff,    "", },
        };
        static std::vector<ChatCommand> tbl =
        {
            { "solocraft", rbac::RBAC_PERM_COMMAND_SERVER_INFO, false, NULL, "", sub },
        };
        return tbl;
    }

    static bool HandleStatus(ChatHandler* handler, char const* /*args*/)
    {
        handler->PSendSysMessage("Solocraft: %s | dungeons:%s raids:%s | balance:%.2f cap:x%.1f healthx:%.2f | corpses:%s | active buffs:%u",
            g_cfg.Enable ? "ENABLED" : "disabled",
            g_cfg.Dungeons ? "on" : "off", g_cfg.Raids ? "on" : "off",
            g_cfg.Balance, g_cfg.MaxMult, g_cfg.HealthMult,
            g_cfg.InstanceCorpsePersist ? "persist" : "decay", uint32(g_applied.size()));
        if (Player* p = handler->GetSession() ? handler->GetSession()->GetPlayer() : NULL)
        {
            std::unordered_map<uint32, AppliedMods>::iterator it = g_applied.find(p->GetGUIDLow());
            if (it != g_applied.end())
                handler->PSendSysMessage("  You are scaled: +%.0f%% stats, +%.0f%% health.", it->second.statPct, it->second.healthPct);
        }
        return true;
    }

    static bool HandleOn(ChatHandler* handler, char const* /*args*/)
    {
        g_cfg.Enable = true;
        handler->SendSysMessage("Solocraft enabled (runtime; re-evaluates on next instance change).");
        return true;
    }

    static bool HandleOff(ChatHandler* handler, char const* /*args*/)
    {
        g_cfg.Enable = false;
        handler->SendSysMessage("Solocraft disabled (buffs clear as players leave/relog).");
        return true;
    }
};

void AddSC_mod_solocraft()
{
    new mod_solocraft_playerscript();
    new mod_solocraft_worldscript();
    new mod_solocraft_commandscript();
}
