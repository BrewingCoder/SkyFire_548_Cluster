/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_SOCIAL_PERSISTENCE_H
#define SKYFIRE_SOCIAL_PERSISTENCE_H
#include "Cluster/ClusterAgent.h"
#include <boost/json/value.hpp>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace Skyfire::Chat
{
    struct SocialRecord
    {
        std::uint64_t Revision = 0;
        boost::json::value Document;
    };
    // Main-loop callers never perform network or SQL work. Mutations carry a
    // caller-stable receipt identity; unknown outcomes must be reconciled first.
    class SocialPersistence
    {
    public:
        enum class Outcome { Committed, Rejected, Unknown, NotSent };
        struct Result
        {
            std::uint32_t Realm;
            std::string Domain, Request;
            Outcome Status;
            std::uint64_t Revision = 0;
            boost::json::value Document;
            bool Lookup = false;
        };
        SocialPersistence();
        ~SocialPersistence();
        bool Start(std::set<std::uint32_t> const& realms, Cluster::AgentOptions const& credentials, std::string& error);
        bool Ready() const;
        bool Ready(std::uint32_t realm) const;
        bool Enabled() const;
        bool Standby() const;
        std::uint64_t OwnershipEpoch(std::uint32_t realm) const;
        bool Read(std::uint32_t realm, std::string const& domain, std::string const& key, SocialRecord& record) const;
        bool Submit(std::uint32_t realm, std::string const& domain, std::string const& request,
            std::string const& key, std::uint64_t expected, std::uint64_t actor, boost::json::value document,
            boost::json::value context = nullptr, boost::json::value command = nullptr);
        bool LookupCommand(std::uint32_t realm, std::string const& request, boost::json::value command);
        std::vector<Result> TakeResults();
        void Stop();
    private:
        struct State;
        std::unique_ptr<State> _state;
    };
}
#endif
