/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* mod-ah-bot - Auction House Bot
* -----------------------------------------------------------------------------
* Fakes a living auction-house economy on a small/family server:
*   * SELLER  - keeps each auction house topped up with reasonably-priced
*               listings so players can buy gear/mats they never looted.
*   * BUYER   - sweeps player listings and buys anything priced at or below a
*               configurable fraction of its value, so players can always
*               offload loot for gold.
*
* All behaviour is driven by conf/ahbot.conf.dist and can be tuned live with
* `.reload config` + `.ahbot reload` - no rebuild needed for the knobs.
*
* Pricing basis: item vendor SellPrice x multiplier (per design decision).
* Buyer scope:   everything under threshold (per design decision).
* AH scope:      Alliance, Horde and Neutral houses (auto-discovered).
*/

#include "Config.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "ObjectMgr.h"
#include "AuctionHouseMgr.h"
#include "Item.h"
#include "ItemPrototype.h"
#include "Player.h"
#include "World.h"
#include "DatabaseEnv.h"
#include "Chat.h"
#include "RBAC.h"
#include "Util.h"
#include <vector>
#include <ctime>

// UNIT_NPC_FLAG_AUCTIONEER (Unit.h) - duplicated here to avoid pulling Unit.h.
#define AHBOT_NPCFLAG_AUCTIONEER 0x00200000

namespace
{
    // ---- Configuration ------------------------------------------------------
    struct AHBotConfig
    {
        bool   Enable          = false;
        bool   SellerEnable    = true;
        bool   BuyerEnable     = true;
        bool   HouseAlliance   = true;
        bool   HouseHorde      = true;
        bool   HouseNeutral    = true;

        uint32 BotCharGuid     = 0;                 // resolved character low GUID
        std::string BotCharName = "Auctioneer";     // used if BotCharGuid == 0

        // seller
        uint32 SellerTarget    = 150;               // desired live listings per house
        uint32 SellerPerCycle  = 10;                // max new listings per house per tick
        float  SellPriceMult   = 1.5f;              // buyout = SellPrice * count * this
        uint32 SellMinQuality  = 1;                 // ITEM_QUALITY_NORMAL
        uint32 SellMaxQuality  = 4;                 // ITEM_QUALITY_EPIC
        uint32 SellMaxItemLevel = 0;                // 0 = no cap
        uint32 SellMaxStack    = 5;                 // cap on random stack size
        uint32 SellDurationHrs = 24;

        // buyer
        float  BuyPriceMult    = 1.0f;              // buy if buyout <= value * this
        uint32 BuyPerCycle     = 10;                // max buyouts per house per tick
        uint32 BuyMinQuality   = 0;                 // consider all by default
        uint64 BuyMaxPrice     = 5000000;           // never buy a listing dearer than this (copper); 0 = no cap

        uint32 TickSeconds     = 120;
    };

    AHBotConfig g_cfg;

    // ---- Per-house state ----------------------------------------------------
    struct AHBotHouse
    {
        AuctionHouseObject*       Object = nullptr;
        AuctionHouseEntry const*  Entry  = nullptr;
        uint32                    FactionTemplateId = 0;   // auctioneer faction_A
        uint32                    AuctioneerGuid    = 0;   // spawned creature low GUID
        char const*               Label  = "?";
    };

    std::vector<AHBotHouse> g_houses;
    std::vector<uint32>     g_pool;       // eligible item entries for the seller
    bool                    g_initialized = false;
    uint32                  g_accumMs = 0;

    uint32 RandBetween(uint32 lo, uint32 hi)
    {
        if (hi <= lo)
            return lo;
        return lo + uint32(rand_norm() * double(hi - lo + 1));
    }

    void LoadConfig()
    {
        g_cfg.Enable         = sConfigMgr->GetBoolDefault("AuctionHouseBot.Enable", false);
        g_cfg.SellerEnable   = sConfigMgr->GetBoolDefault("AuctionHouseBot.Seller.Enable", true);
        g_cfg.BuyerEnable    = sConfigMgr->GetBoolDefault("AuctionHouseBot.Buyer.Enable", true);
        g_cfg.HouseAlliance  = sConfigMgr->GetBoolDefault("AuctionHouseBot.House.Alliance", true);
        g_cfg.HouseHorde     = sConfigMgr->GetBoolDefault("AuctionHouseBot.House.Horde", true);
        g_cfg.HouseNeutral   = sConfigMgr->GetBoolDefault("AuctionHouseBot.House.Neutral", true);

        g_cfg.BotCharGuid    = uint32(sConfigMgr->GetIntDefault("AuctionHouseBot.Character.GUID", 0));
        g_cfg.BotCharName    = sConfigMgr->GetStringDefault("AuctionHouseBot.Character.Name", "Auctioneer");

        g_cfg.SellerTarget   = uint32(sConfigMgr->GetIntDefault("AuctionHouseBot.Seller.Target", 150));
        g_cfg.SellerPerCycle = uint32(sConfigMgr->GetIntDefault("AuctionHouseBot.Seller.PerCycle", 10));
        g_cfg.SellPriceMult  = sConfigMgr->GetFloatDefault("AuctionHouseBot.Seller.PriceMultiplier", 1.5f);
        g_cfg.SellMinQuality = uint32(sConfigMgr->GetIntDefault("AuctionHouseBot.Seller.MinQuality", 1));
        g_cfg.SellMaxQuality = uint32(sConfigMgr->GetIntDefault("AuctionHouseBot.Seller.MaxQuality", 4));
        g_cfg.SellMaxItemLevel = uint32(sConfigMgr->GetIntDefault("AuctionHouseBot.Seller.MaxItemLevel", 0));
        g_cfg.SellMaxStack   = uint32(sConfigMgr->GetIntDefault("AuctionHouseBot.Seller.MaxStack", 5));
        g_cfg.SellDurationHrs = uint32(sConfigMgr->GetIntDefault("AuctionHouseBot.Seller.DurationHours", 24));

        g_cfg.BuyPriceMult   = sConfigMgr->GetFloatDefault("AuctionHouseBot.Buyer.PriceMultiplier", 1.0f);
        g_cfg.BuyPerCycle    = uint32(sConfigMgr->GetIntDefault("AuctionHouseBot.Buyer.PerCycle", 10));
        g_cfg.BuyMinQuality  = uint32(sConfigMgr->GetIntDefault("AuctionHouseBot.Buyer.MinQuality", 0));
        g_cfg.BuyMaxPrice    = uint64(sConfigMgr->GetIntDefault("AuctionHouseBot.Buyer.MaxPrice", 5000000));

        g_cfg.TickSeconds    = uint32(sConfigMgr->GetIntDefault("AuctionHouseBot.TickSeconds", 120));
        if (g_cfg.TickSeconds < 10)
            g_cfg.TickSeconds = 10;
        if (g_cfg.SellMaxStack < 1)
            g_cfg.SellMaxStack = 1;
    }

    bool ItemEligibleForSeller(ItemTemplate const& proto)
    {
        if (proto.Name1.empty())
            return false;
        if (proto.SellPrice == 0)
            return false;
        if (proto.Quality < g_cfg.SellMinQuality || proto.Quality > g_cfg.SellMaxQuality)
            return false;
        if (g_cfg.SellMaxItemLevel && proto.ItemLevel > g_cfg.SellMaxItemLevel)
            return false;
        if (proto.Flags & ITEM_PROTO_FLAG_CONJURED)
            return false;

        // Only items a player could legitimately post: unbound, BoE, bind-on-use.
        switch (proto.Bonding)
        {
            case NO_BIND:
            case BIND_WHEN_EQUIPED:
            case BIND_WHEN_USE:
                break;
            default:
                return false;   // BoP / quest-bound
        }

        switch (proto.Class)
        {
            case ITEM_CLASS_CONSUMABLE:
            case ITEM_CLASS_CONTAINER:
            case ITEM_CLASS_WEAPON:
            case ITEM_CLASS_GEM:
            case ITEM_CLASS_ARMOR:
            case ITEM_CLASS_REAGENT:
            case ITEM_CLASS_PROJECTILE:
            case ITEM_CLASS_TRADE_GOODS:
            case ITEM_CLASS_RECIPE:
            case ITEM_CLASS_QUIVER:
            case ITEM_CLASS_MISCELLANEOUS:
            case ITEM_CLASS_GLYPH:
                return true;
            default:
                return false;   // quest, key, money, etc.
        }
    }

    void BuildItemPool()
    {
        g_pool.clear();
        ItemTemplateContainer const* store = sObjectMgr->GetItemTemplateStore();
        if (!store)
            return;

        for (ItemTemplateContainer::const_iterator itr = store->begin(); itr != store->end(); ++itr)
            if (ItemEligibleForSeller(itr->second))
                g_pool.push_back(itr->first);

        SF_LOG_INFO("modules", "[mod-ah-bot] seller item pool built: %u eligible item templates.", uint32(g_pool.size()));
    }

    // Resolve the bot's character low GUID from config (GUID first, then name).
    uint32 ResolveBotGuid()
    {
        if (g_cfg.BotCharGuid)
        {
            if (sObjectMgr->GetPlayerAccountIdByGUID(MAKE_NEW_GUID(g_cfg.BotCharGuid, 0, HIGHGUID_PLAYER)))
                return g_cfg.BotCharGuid;
            SF_LOG_ERROR("modules", "[mod-ah-bot] configured Character.GUID %u does not exist.", g_cfg.BotCharGuid);
            return 0;
        }

        uint64 full = sObjectMgr->GetPlayerGUIDByName(g_cfg.BotCharName);
        if (full)
            return GUID_LOPART(full);

        SF_LOG_ERROR("modules", "[mod-ah-bot] bot character '%s' not found. Create it (or set Character.GUID) then `.ahbot reload`.",
            g_cfg.BotCharName.c_str());
        return 0;
    }

    // Discover one spawned auctioneer NPC for each enabled house.
    void DiscoverHouses()
    {
        g_houses.clear();

        // Target houses keyed by a representative faction template id.
        struct Target { uint32 ftid; bool enabled; char const* label; };
        Target targets[3] =
        {
            { 55,  g_cfg.HouseAlliance, "Alliance" },  // dwarf / generic alliance
            { 29,  g_cfg.HouseHorde,    "Horde"    },  // orc / generic horde
            { 120, g_cfg.HouseNeutral,  "Neutral"  },  // booty bay
        };

        // Pull every spawned auctioneer and remember one guid per distinct house object.
        QueryResult result = WorldDatabase.Query(
            "SELECT c.guid, ct.faction_A FROM creature c "
            "JOIN creature_template ct ON c.id = ct.entry "
            "WHERE (ct.npcflag & 2097152) <> 0");

        if (!result)
        {
            SF_LOG_ERROR("modules", "[mod-ah-bot] no spawned auctioneer NPCs found - cannot create persistent auctions. Disabled.");
            return;
        }

        // Buffer all auctioneers once (QueryResult is forward-only).
        struct Auctioneer { uint32 guid; uint32 faction; };
        std::vector<Auctioneer> auctioneers;
        do
        {
            Field* f = result->Fetch();
            auctioneers.push_back({ f[0].GetUInt32(), uint32(f[1].GetUInt16()) });
        } while (result->NextRow());

        for (uint8 i = 0; i < 3; ++i)
        {
            if (!targets[i].enabled)
                continue;

            AuctionHouseObject* wantObj = sAuctionMgr->GetAuctionsMap(targets[i].ftid);

            // Skip a house we already registered (two-side-interaction collapses all to neutral).
            bool dup = false;
            for (AHBotHouse const& h : g_houses)
                if (h.Object == wantObj) { dup = true; break; }
            if (dup)
                continue;

            // Find an auctioneer whose faction maps to this house object.
            uint32 chosenGuid = 0;
            uint32 chosenFaction = 0;
            for (Auctioneer const& au : auctioneers)
            {
                if (sAuctionMgr->GetAuctionsMap(au.faction) == wantObj)
                {
                    chosenGuid = au.guid;
                    chosenFaction = au.faction;
                    break;
                }
            }

            if (!chosenGuid)
            {
                SF_LOG_INFO("modules", "[mod-ah-bot] %s house enabled but no matching auctioneer spawn found - skipping.", targets[i].label);
                continue;
            }

            AHBotHouse house;
            house.Object = wantObj;
            house.Entry = AuctionHouseMgr::GetAuctionHouseEntry(chosenFaction);
            house.FactionTemplateId = chosenFaction;
            house.AuctioneerGuid = chosenGuid;
            house.Label = targets[i].label;
            if (!house.Entry)
            {
                SF_LOG_ERROR("modules", "[mod-ah-bot] %s house has no AuctionHouse.dbc entry for faction %u - skipping.", targets[i].label, chosenFaction);
                continue;
            }

            g_houses.push_back(house);
            SF_LOG_INFO("modules", "[mod-ah-bot] %s house ready (auctioneer guid %u, faction %u).", house.Label, house.AuctioneerGuid, house.FactionTemplateId);
        }
    }

    bool EnsureInitialized()
    {
        if (g_initialized)
            return true;

        BuildItemPool();
        DiscoverHouses();
        g_cfg.BotCharGuid = ResolveBotGuid();

        if (!g_cfg.BotCharGuid || g_houses.empty() || g_pool.empty())
        {
            SF_LOG_ERROR("modules", "[mod-ah-bot] not ready (bot guid=%u, houses=%u, pool=%u) - will retry next tick.",
                g_cfg.BotCharGuid, uint32(g_houses.size()), uint32(g_pool.size()));
            return false;
        }

        g_initialized = true;
        SF_LOG_INFO("modules", "[mod-ah-bot] initialized. bot guid=%u, houses=%u, pool=%u.",
            g_cfg.BotCharGuid, uint32(g_houses.size()), uint32(g_pool.size()));
        return true;
    }

    // ---- Seller -------------------------------------------------------------
    void RunSeller(AHBotHouse const& house)
    {
        uint32 live = house.Object->Getcount();
        if (live >= g_cfg.SellerTarget)
            return;

        uint32 toAdd = g_cfg.SellerTarget - live;
        if (toAdd > g_cfg.SellerPerCycle)
            toAdd = g_cfg.SellerPerCycle;

        uint64 botGuidFull = MAKE_NEW_GUID(g_cfg.BotCharGuid, 0, HIGHGUID_PLAYER);
        uint32 added = 0;

        for (uint32 n = 0; n < toAdd; ++n)
        {
            uint32 entry = g_pool[RandBetween(0, uint32(g_pool.size() - 1))];
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (!proto)
                continue;

            uint32 maxStack = proto->GetMaxStackSize();
            uint32 cap = g_cfg.SellMaxStack;
            if (maxStack < cap)
                cap = maxStack;
            if (cap < 1)
                cap = 1;
            uint32 count = RandBetween(1, cap);

            Item* item = Item::CreateItem(entry, count, nullptr);
            if (!item)
                continue;
            item->SetOwnerGUID(botGuidFull);

            uint64 buyout = uint64(double(proto->SellPrice) * double(count) * double(g_cfg.SellPriceMult));
            if (buyout < 1)
            {
                delete item;
                continue;
            }
            uint64 startbid = uint64(double(buyout) * frand(0.5f, 0.9f));
            if (startbid < 1)
                startbid = 1;

            AuctionEntry* ah = new AuctionEntry();
            ah->Id = sObjectMgr->GenerateAuctionID();
            ah->auctioneer = house.AuctioneerGuid;
            ah->itemGUIDLow = item->GetGUIDLow();
            ah->itemEntry = entry;
            ah->itemCount = count;
            ah->owner = g_cfg.BotCharGuid;
            ah->startbid = startbid;
            ah->bidder = 0;
            ah->bid = 0;
            ah->buyout = buyout;
            ah->expire_time = time(nullptr) + g_cfg.SellDurationHrs * HOUR;
            ah->deposit = 0;
            ah->auctionHouseEntry = house.Entry;
            ah->factionTemplateId = house.FactionTemplateId;

            SQLTransaction trans = CharacterDatabase.BeginTransaction();
            item->SaveToDB(trans);
            ah->SaveToDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            sAuctionMgr->AddAItem(item);
            house.Object->AddAuction(ah);
            ++added;
        }

        if (added)
            SF_LOG_INFO("modules", "[mod-ah-bot] seller: listed %u item(s) in %s house (now %u live).",
                added, house.Label, house.Object->Getcount());
    }

    // ---- Buyer --------------------------------------------------------------
    void RunBuyer(AHBotHouse const& house)
    {
        std::vector<AuctionEntry*> buys;
        for (AuctionHouseObject::AuctionEntryMap::iterator itr = house.Object->GetAuctionsBegin();
             itr != house.Object->GetAuctionsEnd(); ++itr)
        {
            AuctionEntry* a = itr->second;
            if (!a || a->owner == g_cfg.BotCharGuid)   // never buy our own listings
                continue;
            if (a->buyout == 0)                         // bid-only auctions: skip
                continue;
            if (g_cfg.BuyMaxPrice && a->buyout > g_cfg.BuyMaxPrice)
                continue;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(a->itemEntry);
            if (!proto)
                continue;
            if (proto->Quality < g_cfg.BuyMinQuality)
                continue;

            uint64 value = uint64(proto->SellPrice) * a->itemCount;
            if (value == 0)
                continue;
            if (a->buyout <= uint64(double(value) * double(g_cfg.BuyPriceMult)))
                buys.push_back(a);

            if (buys.size() >= g_cfg.BuyPerCycle)
                break;
        }

        if (buys.empty())
            return;

        for (AuctionEntry* a : buys)
        {
            uint32 itemGuidLow = a->itemGUIDLow;
            uint32 itemEntry = a->itemEntry;

            SQLTransaction trans = CharacterDatabase.BeginTransaction();
            a->bidder = g_cfg.BotCharGuid;
            a->bid = a->buyout;
            sAuctionMgr->SendAuctionSalePendingMail(a, trans);  // notify seller
            sAuctionMgr->SendAuctionSuccessfulMail(a, trans);   // pay seller
            sAuctionMgr->SendAuctionWonMail(a, trans);          // deliver item to bot
            a->DeleteFromDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            sAuctionMgr->RemoveAItem(itemGuidLow);
            house.Object->RemoveAuction(a, itemEntry);          // deletes 'a'
        }

        SF_LOG_INFO("modules", "[mod-ah-bot] buyer: bought %u listing(s) from %s house.",
            uint32(buys.size()), house.Label);
    }

    void RunCycle()
    {
        if (!g_cfg.Enable)
            return;
        if (!EnsureInitialized())
            return;

        for (AHBotHouse const& house : g_houses)
        {
            if (g_cfg.SellerEnable)
                RunSeller(house);
            if (g_cfg.BuyerEnable)
                RunBuyer(house);
        }
    }
}

// ---- Scripts ---------------------------------------------------------------
class mod_ahbot_worldscript : public WorldScript
{
public:
    mod_ahbot_worldscript() : WorldScript("mod_ahbot_worldscript") { }

    void OnConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
        g_initialized = false;   // force re-init with fresh config on next tick
    }

    void OnUpdate(uint32 diff) override
    {
        if (!g_cfg.Enable)
            return;

        g_accumMs += diff;
        if (g_accumMs < g_cfg.TickSeconds * IN_MILLISECONDS)
            return;
        g_accumMs = 0;

        RunCycle();
    }
};

class mod_ahbot_commandscript : public CommandScript
{
public:
    mod_ahbot_commandscript() : CommandScript("mod_ahbot_commandscript") { }

    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> ahbotCommandTable =
        {
            { "status", rbac::RBAC_PERM_COMMAND_SERVER_INFO, true, &HandleAHBotStatus, "", },
            { "on",     rbac::RBAC_PERM_COMMAND_SERVER_SET,  true, &HandleAHBotOn,     "", },
            { "off",    rbac::RBAC_PERM_COMMAND_SERVER_SET,  true, &HandleAHBotOff,    "", },
            { "run",    rbac::RBAC_PERM_COMMAND_SERVER_SET,  true, &HandleAHBotRun,    "", },
            { "reload", rbac::RBAC_PERM_COMMAND_SERVER_SET,  true, &HandleAHBotReload, "", },
        };
        static std::vector<ChatCommand> commandTable =
        {
            { "ahbot", rbac::RBAC_PERM_COMMAND_SERVER_INFO, true, NULL, "", ahbotCommandTable },
        };
        return commandTable;
    }

    static bool HandleAHBotStatus(ChatHandler* handler, char const* /*args*/)
    {
        handler->PSendSysMessage("AH-Bot: %s | seller:%s buyer:%s | houses:%u pool:%u botGuid:%u",
            g_cfg.Enable ? "ENABLED" : "disabled",
            g_cfg.SellerEnable ? "on" : "off",
            g_cfg.BuyerEnable ? "on" : "off",
            uint32(g_houses.size()), uint32(g_pool.size()), g_cfg.BotCharGuid);
        for (AHBotHouse const& h : g_houses)
            handler->PSendSysMessage("  %s: %u live auction(s)", h.Label, h.Object->Getcount());
        return true;
    }

    static bool HandleAHBotOn(ChatHandler* handler, char const* /*args*/)
    {
        g_cfg.Enable = true;
        handler->SendSysMessage("AH-Bot enabled (runtime only; set AuctionHouseBot.Enable=1 to persist).");
        return true;
    }

    static bool HandleAHBotOff(ChatHandler* handler, char const* /*args*/)
    {
        g_cfg.Enable = false;
        handler->SendSysMessage("AH-Bot disabled (runtime only).");
        return true;
    }

    static bool HandleAHBotRun(ChatHandler* handler, char const* /*args*/)
    {
        bool savedEnable = g_cfg.Enable;
        g_cfg.Enable = true;
        RunCycle();
        g_cfg.Enable = savedEnable;
        handler->SendSysMessage("AH-Bot: ran one cycle.");
        return true;
    }

    static bool HandleAHBotReload(ChatHandler* handler, char const* /*args*/)
    {
        LoadConfig();
        g_initialized = false;
        handler->SendSysMessage("AH-Bot: config reloaded, state will re-initialize on next cycle.");
        return true;
    }
};

void AddSC_mod_ahbot()
{
    new mod_ahbot_worldscript();
    new mod_ahbot_commandscript();
}
