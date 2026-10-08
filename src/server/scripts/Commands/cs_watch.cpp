/*
* [lab] watch_commandscript - control the character event watcher (WatchMgr).
* Usable in-game (.watch ...) and over RA/RCON (watch ...), no target needed.
*
*   watch name <player> [radius] [cats]   start watching an online player
*   watch guid <lowguid> [radius] [cats]  start watching by creature-style low guid of an online player
*   watch off [player]                    stop one (by name) or all
*   watch status                          list active watches
*
* radius: yards; 0 (default) = whole map/instance.
* cats:   csv of go,spawn,death,flags,move,cast  (default: all but move/cast; 'all' includes move)
*/
#include "ScriptMgr.h"
#include "Chat.h"
#include "WatchMgr.h"
#include <sstream>
#include <vector>
#include <cstdlib>

class watch_commandscript : public CommandScript
{
public:
    watch_commandscript() : CommandScript("watch_commandscript") { }

    std::vector<ChatCommand> GetCommands() const OVERRIDE
    {
        static std::vector<ChatCommand> watchCommandTable =
        {
            { "name",   rbac::RBAC_PERM_COMMAND_DEBUG, true, &HandleWatchNameCommand,   "", },
            { "guid",   rbac::RBAC_PERM_COMMAND_DEBUG, true, &HandleWatchGuidCommand,   "", },
            { "off",    rbac::RBAC_PERM_COMMAND_DEBUG, true, &HandleWatchOffCommand,    "", },
            { "status", rbac::RBAC_PERM_COMMAND_DEBUG, true, &HandleWatchStatusCommand, "", },
        };
        static std::vector<ChatCommand> commandTable =
        {
            { "watch",  rbac::RBAC_PERM_COMMAND_DEBUG, true, NULL, "", watchCommandTable },
        };
        return commandTable;
    }

    // split "args" into tokens; pull out an optional numeric radius and a csv cats token
    static void ParseTail(std::vector<std::string> const& toks, size_t start, float& radius, std::string& cats)
    {
        radius = 0.0f;
        cats.clear();
        for (size_t i = start; i < toks.size(); ++i)
        {
            char* end = nullptr;
            float v = strtof(toks[i].c_str(), &end);
            if (end && *end == '\0' && !toks[i].empty())
                radius = v;             // purely numeric -> radius
            else if (cats.empty())
                cats = toks[i];         // first non-numeric -> cats csv
        }
    }

    static std::vector<std::string> Tokenize(char const* args)
    {
        std::vector<std::string> toks;
        if (args)
        {
            std::istringstream ss(args);
            std::string t;
            while (ss >> t)
                toks.push_back(t);
        }
        return toks;
    }

    static bool HandleWatchNameCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> toks = Tokenize(args);
        if (toks.empty())
        {
            handler->SendSysMessage("Usage: watch name <player> [radius] [cats]");
            handler->SetSentErrorMessage(true);
            return false;
        }
        float radius; std::string cats;
        ParseTail(toks, 1, radius, cats);
        std::string info;
        bool ok = sWatchMgr->StartByName(toks[0], radius, WatchMgr::ParseCategories(cats), info);
        handler->PSendSysMessage("%s", info.c_str());
        if (!ok)
            handler->SetSentErrorMessage(true);
        return ok;
    }

    static bool HandleWatchGuidCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> toks = Tokenize(args);
        if (toks.empty())
        {
            handler->SendSysMessage("Usage: watch guid <lowguid> [radius] [cats]");
            handler->SetSentErrorMessage(true);
            return false;
        }
        uint32 low = uint32(strtoul(toks[0].c_str(), nullptr, 10));
        float radius; std::string cats;
        ParseTail(toks, 1, radius, cats);
        std::string info;
        bool ok = sWatchMgr->StartByGuid(low, radius, WatchMgr::ParseCategories(cats), info);
        handler->PSendSysMessage("%s", info.c_str());
        if (!ok)
            handler->SetSentErrorMessage(true);
        return ok;
    }

    static bool HandleWatchOffCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> toks = Tokenize(args);
        if (toks.empty())
        {
            sWatchMgr->StopAll();
            handler->SendSysMessage("all watches stopped");
            return true;
        }
        std::string info;
        bool ok = sWatchMgr->StopByName(toks[0], info);
        handler->PSendSysMessage("%s", info.c_str());
        if (!ok)
            handler->SetSentErrorMessage(true);
        return ok;
    }

    static bool HandleWatchStatusCommand(ChatHandler* handler, char const* /*args*/)
    {
        handler->PSendSysMessage("%s", sWatchMgr->Status().c_str());
        return true;
    }
};

void AddSC_watch_commandscript()
{
    new watch_commandscript();
}
