/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_BATTLEGROUND_SERVER_H
#define SKYFIRE_BATTLEGROUND_SERVER_H
#include "Cluster/ClusterAgent.h"
#include "Cluster/BattlegroundProtocol.h"
#include <memory>
#include <map>
#include <set>
namespace Skyfire::BattlegroundService
{
    struct Options
    {
        std::string Address;
        std::uint16_t Port = 54950;
        unsigned MaxConnections = 32, TimeoutSeconds = 5;
        std::set<std::string> WorldKeys;
        std::set<std::uint32_t> Realms;
        std::map<std::string,std::set<std::uint32_t>> WorldRealms;
    };
    class Server
    {
    public:
        Server(); ~Server();
        bool Open(Options options,Cluster::AgentOptions const& tls,std::string& error);
        void SetReady(bool ready);
        bool Ready() const;
        void Update();
        void Stop();
        Cluster::BattlegroundMetrics Metrics() const;
    private:
        struct State;
        std::unique_ptr<State> _state;
    };
}
#endif
