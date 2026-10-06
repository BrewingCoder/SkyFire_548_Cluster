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
#include <mutex>

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
        float ArmorMult  = 1.0f;  // flat armor multiplier while scaled (physical mitigation); 1.0 = off
        float DamageMult = 1.0f;  // extra factor on weapon-damage scaling; weapon dmg scales at (mult * this). 1.0 = scale with group size
        uint32 MinLevel = 1;
        bool  ScaleHealth = true;
        bool  Announce = true;
        bool  InstanceCorpsePersist = true;  // keep mob corpses in instances until reset (loot after a death / clear-then-loot)
        bool  PreventDurabilityLoss = false; // disable all durability loss server-wide (solo/family QoL; enforced by a core gate)
        bool  UseAura = true;                // master toggle for the all-school incoming-damage reduction (name kept for config compat)
        float DamageTakenReduction = 0.0f;   // % all-school incoming damage reduction while scaled; 0 = off
    };

    SolocraftConfig g_cfg;

    // TOTAL_PCT amounts currently applied to a player (so we remove exactly what we added).
    // Membership also marks "this player is currently scaled" for the OnDamage reduction hook.
    struct AppliedMods { float statPct; float healthPct; float armorPct; float damagePct; };
    std::unordered_map<uint32, AppliedMods> g_applied;
    std::mutex g_applied_mutex;  // guards g_applied (read on the hot OnDamage path, written on map change)

    void LoadConfig()
    {
        g_cfg.Enable      = sConfigMgr->GetBoolDefault("Solocraft.Enable", false);
        g_cfg.Dungeons    = sConfigMgr->GetBoolDefault("Solocraft.Dungeons", true);
        g_cfg.Raids       = sConfigMgr->GetBoolDefault("Solocraft.Raids", true);
        g_cfg.Balance     = sConfigMgr->GetFloatDefault("Solocraft.Balance", 0.6f);
        g_cfg.MaxMult     = sConfigMgr->GetFloatDefault("Solocraft.MaxMultiplier", 10.0f);
        g_cfg.HealthMult  = sConfigMgr->GetFloatDefault("Solocraft.HealthMultiplier", 1.0f);
        g_cfg.ArmorMult   = sConfigMgr->GetFloatDefault("Solocraft.ArmorMultiplier", 1.0f);
        g_cfg.DamageMult  = sConfigMgr->GetFloatDefault("Solocraft.DamageMultiplier", 1.0f);
        g_cfg.MinLevel    = uint32(sConfigMgr->GetIntDefault("Solocraft.MinLevel", 1));
        g_cfg.ScaleHealth = sConfigMgr->GetBoolDefault("Solocraft.ScaleHealth", true);
        g_cfg.Announce    = sConfigMgr->GetBoolDefault("Solocraft.Announce", true);
        g_cfg.InstanceCorpsePersist = sConfigMgr->GetBoolDefault("Solocraft.InstanceCorpsePersist", true);
        g_cfg.PreventDurabilityLoss = sConfigMgr->GetBoolDefault("Solocraft.PreventDurabilityLoss", false);
        g_cfg.UseAura             = sConfigMgr->GetBoolDefault("Solocraft.UseAura", true);
        g_cfg.DamageTakenReduction = sConfigMgr->GetFloatDefault("Solocraft.DamageTakenReduction", 0.0f);
        if (g_cfg.DamageTakenReduction < 0.0f)  g_cfg.DamageTakenReduction = 0.0f;
        if (g_cfg.DamageTakenReduction > 95.0f) g_cfg.DamageTakenReduction = 95.0f;  // never full immunity

        if (g_cfg.Balance < 0.0f) g_cfg.Balance = 0.0f;
        if (g_cfg.MaxMult < 1.0f) g_cfg.MaxMult = 1.0f;
        if (g_cfg.HealthMult < 0.1f) g_cfg.HealthMult = 0.1f;   // keep health sane; this is an EXTRA multiplier on top of the core mult
        if (g_cfg.ArmorMult  < 0.1f) g_cfg.ArmorMult  = 0.1f;
        if (g_cfg.DamageMult < 0.1f) g_cfg.DamageMult = 0.1f;
    }

    // statPct scales the five primary stats; healthPct scales max health (an independent amount
    // so health can be boosted above and beyond the core multiplier via Solocraft.HealthMultiplier).
    void ApplyMods(Player* player, float statPct, float healthPct, float armorPct, float damagePct, bool apply)
    {
        for (uint8 s = STAT_STRENGTH; s < MAX_STATS; ++s)
            player->HandleStatModifier(UnitMods(UNIT_MOD_STAT_START + s), TOTAL_PCT, statPct, apply);
        if (g_cfg.ScaleHealth)
            player->HandleStatModifier(UNIT_MOD_HEALTH, TOTAL_PCT, healthPct, apply);
        if (armorPct != 0.0f)
            player->HandleStatModifier(UNIT_MOD_ARMOR, TOTAL_PCT, armorPct, apply);  // physical mitigation
        if (damagePct != 0.0f)
        {
            // Scale the weapon-damage term too: a weapon class's DPS comes mostly from weapon
            // damage (an item property stats never touch), so stat scaling alone barely moves it.
            player->HandleStatModifier(UNIT_MOD_DAMAGE_MAINHAND, TOTAL_PCT, damagePct, apply);
            player->HandleStatModifier(UNIT_MOD_DAMAGE_OFFHAND,  TOTAL_PCT, damagePct, apply);
            player->HandleStatModifier(UNIT_MOD_DAMAGE_RANGED,   TOTAL_PCT, damagePct, apply);
        }
    }

    void RemoveBuff(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> lock(g_applied_mutex);
        std::unordered_map<uint32, AppliedMods>::iterator it = g_applied.find(player->GetGUIDLow());
        if (it == g_applied.end())
            return;
        ApplyMods(player, it->second.statPct, it->second.healthPct, it->second.armorPct, it->second.damagePct, false);
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
        float armorPct  = (g_cfg.ArmorMult - 1.0f) * 100.0f;   // flat armor multiplier while scaled (0 = off)
        float damageMult = mult * g_cfg.DamageMult;            // weapon damage scales with group size (x DamageMult extra)
        float damagePct = (damageMult - 1.0f) * 100.0f;        // TOTAL_PCT on weapon damage
        ApplyMods(player, statPct, healthPct, armorPct, damagePct, true);
        {
            std::lock_guard<std::mutex> lock(g_applied_mutex);
            g_applied[player->GetGUIDLow()] = { statPct, healthPct, armorPct, damagePct };
        }

        // Incoming-damage reduction is applied in mod_solocraft_unitscript::OnDamage
        // (DealDamage is the universal sink, so it covers physical/spell/periodic alike).
        // The old approach rolled a carrier aura (32172) whose effects we tried to override
        // via world.spelleffect_dbc -- but this core builds SpellInfo purely from the
        // SpellEffect.dbc file and never reads that table, so the aura was a no-op (measured:
        // not present client-side, periodic damage flat). Driving it in code fixes that and
        // also covers physical melee, which MOD_DAMAGE_PERCENT_DONE excludes.

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
        if (!player)
            return;
        std::lock_guard<std::mutex> lock(g_applied_mutex);
        g_applied.erase(player->GetGUIDLow());   // runtime mods vanish on logout anyway
    }
};

// Incoming-damage reduction for scaled players. Unit::DealDamage is the single sink for
// ALL damage (melee, spell, periodic/DoT, environmental), so this one hook mitigates every
// type -- including physical, which an aura-based MOD_DAMAGE_PERCENT_DONE never could.
class mod_solocraft_unitscript : public UnitScript
{
public:
    mod_solocraft_unitscript() : UnitScript("mod_solocraft_unitscript") { }

    void OnDamage(Unit* /*attacker*/, Unit* victim, uint32& damage) override
    {
        if (!g_cfg.Enable || !g_cfg.UseAura || g_cfg.DamageTakenReduction <= 0.0f)
            return;
        if (!damage || !victim || victim->GetTypeId() != TypeID::TYPEID_PLAYER)
            return;

        {
            std::lock_guard<std::mutex> lock(g_applied_mutex);
            if (g_applied.find(victim->GetGUIDLow()) == g_applied.end())
                return;   // only players currently scaled by solocraft
        }

        float factor = 1.0f - (g_cfg.DamageTakenReduction / 100.0f);
        damage = uint32(float(damage) * factor);
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
        handler->PSendSysMessage("Solocraft: %s | dungeons:%s raids:%s | balance:%.2f cap:x%.1f healthx:%.2f armorx:%.2f dmgx:%.2f | corpses:%s | active buffs:%u",
            g_cfg.Enable ? "ENABLED" : "disabled",
            g_cfg.Dungeons ? "on" : "off", g_cfg.Raids ? "on" : "off",
            g_cfg.Balance, g_cfg.MaxMult, g_cfg.HealthMult, g_cfg.ArmorMult, g_cfg.DamageMult,
            g_cfg.InstanceCorpsePersist ? "persist" : "decay", uint32(g_applied.size()));
        handler->PSendSysMessage("  durability loss: %s | incoming-dmg reduction: %s -%.0f%% (code hook, all schools)",
            g_cfg.PreventDurabilityLoss ? "DISABLED" : "normal",
            (g_cfg.UseAura && g_cfg.DamageTakenReduction > 0.0f) ? "ON" : "off", g_cfg.DamageTakenReduction);
        if (Player* p = handler->GetSession() ? handler->GetSession()->GetPlayer() : NULL)
        {
            std::unordered_map<uint32, AppliedMods>::iterator it = g_applied.find(p->GetGUIDLow());
            if (it != g_applied.end())
                handler->PSendSysMessage("  You are scaled: +%.0f%% stats, +%.0f%% health, +%.0f%% armor, +%.0f%% weapon dmg.", it->second.statPct, it->second.healthPct, it->second.armorPct, it->second.damagePct);
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
    new mod_solocraft_unitscript();
    new mod_solocraft_worldscript();
    new mod_solocraft_commandscript();
}
