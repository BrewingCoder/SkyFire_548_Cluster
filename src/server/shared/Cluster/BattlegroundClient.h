/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_BATTLEGROUND_CLIENT_H
#define SKYFIRE_BATTLEGROUND_CLIENT_H
#include "BattlegroundProtocol.h"
#include "ClusterAgent.h"
namespace Skyfire::BattlegroundService
{
    struct Result { Snapshot Request; Response Matches; bool Success = false; };
    bool StartClient(Cluster::AgentOptions options, std::string& error);
    void StopClient();
    bool Enabled();
    std::uint64_t Submit(Snapshot snapshot);
    std::vector<Result> TakeResults();
}
#endif
