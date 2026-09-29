/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_CHAT_GUILD_AUTHORITY_H
#define SKYFIRE_CHAT_GUILD_AUTHORITY_H
#include "GuildState.h"
#include "SocialPersistence.h"
#include "Cluster/ChatService.h"
#include <functional>
#include <map>
#include <optional>
#include <tuple>

namespace Skyfire::Chat
{
    class GuildAuthority
    {
    public:
        using PresenceCheck = std::function<bool(std::uint32_t, std::uint64_t, std::uint64_t)>;
        explicit GuildAuthority(SocialPersistence& persistence, PresenceCheck presence = {}) : Persistence(persistence), Presence(std::move(presence)) { }
        void Handle(std::uint32_t realm, std::string const& node, ServiceRequest const& request, ServiceCompletion complete);
        void Complete(SocialPersistence::Result const& result);
        using Lookup = std::function<std::optional<PlayerPresence>(std::uint32_t, std::uint64_t)>;
        using Emit = std::function<bool(std::uint32_t, ServiceEvent)>;
        void SetMessaging(Lookup lookup, Emit emit) { Find = std::move(lookup); Send = std::move(emit); }
        using NameLookup = std::function<std::optional<PlayerPresence>(std::uint32_t, std::string const&)>;
        void SetNames(NameLookup lookup) { FindName = std::move(lookup); }
        static GuildState Decode(boost::json::value const& document);
        static boost::json::value Encode(GuildState const& state);
    private:
        struct Pending
        {
            std::uint32_t Realm = 0;
            std::uint64_t Sequence = 0, Actor = 0;
            std::string Key;
            boost::json::value Document;
            ServiceCompletion Completion;
        };
        using OutcomeKey = std::tuple<std::uint32_t, std::string, std::string, std::uint64_t>;
        struct OutcomeRecord
        {
            std::uint32_t Guild, Account;
            std::uint64_t Actor, Incarnation;
            std::optional<ServiceResponse> Response;
            std::chrono::steady_clock::time_point Updated = std::chrono::steady_clock::now();
        };
        std::map<OutcomeKey, OutcomeRecord> Outcomes;
        SocialPersistence& Persistence;
        PresenceCheck Presence;
        Lookup Find;
        NameLookup FindName;
        Emit Send;
        struct Invitation
        {
            std::uint32_t Guild;
            std::uint64_t TargetIncarnation, Inviter, InviterIncarnation;
            std::chrono::steady_clock::time_point Expires;
        };
        std::map<std::pair<std::uint32_t, std::uint64_t>, Invitation> Invitations;
        std::map<std::string, Pending> PendingRequests;
        std::map<std::pair<std::uint32_t, std::string>, SocialRecord> Committed;
    };
}
#endif
