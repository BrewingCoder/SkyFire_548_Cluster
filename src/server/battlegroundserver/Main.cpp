/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "BattlegroundServer.h"
#include "Cluster/BattlegroundProtocol.h"
#include "Configuration/Config.h"
#include "Configuration/ConfigVersion.h"
#include "Database/DatabaseEnv.h"
#include "Platform/HubProcessControl.h"
#include "Log.h"
#include <charconv>
#include <csignal>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <thread>

#ifndef _SKYFIRE_BATTLEGROUND_CONFIG
#define _SKYFIRE_BATTLEGROUND_CONFIG "battlegroundserver.conf"
#endif

// Required by the shared logging library. Battleground matchmaking opens no database connections.
LoginDatabaseWorkerPool LoginDatabase;
namespace
{
    volatile std::sig_atomic_t Stopping = 0;
    void Signal(int) { Stopping = 1; }
    bool Handle(std::string const& text, uint64& value)
    {
        auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc() && result.ptr == text.data() + text.size() && value != 0;
    }
}
int main(int argc, char** argv)
{
    std::string config = _SKYFIRE_BATTLEGROUND_CONFIG;
    std::string expectedKey;
    uint64 control = 0, status = 0;
    for (int i = 1; i < argc; ++i)
    {
        std::string argument = argv[i];
        if (argument == "--help")
        { std::puts("battlegroundserver -c battlegroundserver.conf\nNative realm-scoped unrated battleground matchmaking service."); return 0; }
        if (argument == "-c" && i + 1 < argc) config = argv[++i];
        else if (argument == "--hub-node-key" && i + 1 < argc) expectedKey = argv[++i];
        else if (argument == "--hub-control-read" && i + 1 < argc && Handle(argv[++i], control)) { }
        else if (argument == "--hub-status-write" && i + 1 < argc && Handle(argv[++i], status)) { }
        else { std::fputs("Invalid battlegroundserver argument. Use --help.\n", stderr); return 1; }
    }
    if (bool(control) != bool(status)) { std::fputs("Both inherited hub handles are required.\n", stderr); return 1; }
    Skyfire::HubControl::ChildChannel channel;
    std::string error;
    if (control && !channel.Initialize(control, status, error))
    { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    if (!sConfigMgr->LoadInitial(config.c_str()))
    { std::fputs("Cannot read battlegroundserver configuration.\n", stderr); return 1; }
    sLog->LoadFromConfig();
    uint32 const confVersion = sConfigMgr->GetIntDefault("ConfVersion", 0);
    if (confVersion < Skyfire::ConfigVersion::Battleground)
        SF_LOG_WARN("server.battleground", "Your battlegroundserver.conf is out of date (found %u, expected %u). Compare with battlegroundserver.conf.dist.",
            confVersion, Skyfire::ConfigVersion::Battleground);
    Skyfire::BattlegroundService::Options options;
    int const port = sConfigMgr->GetIntDefault("Battleground.Port", 54950);
    int const capacity = sConfigMgr->GetIntDefault("Battleground.MaxConnections", 32);
    int const timeout = sConfigMgr->GetIntDefault("Battleground.RequestTimeout", 5);
    if (port < 1 || port > 65535 || capacity < 1 || capacity > 128 || timeout < 1 || timeout > 30)
    { SF_LOG_ERROR("server.battleground", "Invalid battleground port, connection limit or deadline."); return 1; }
    options.Port = std::uint16_t(port); options.MaxConnections = unsigned(capacity); options.TimeoutSeconds = unsigned(timeout);
    options.Address = sConfigMgr->GetStringDefault("Battleground.BindIP", "127.0.0.1");
    std::istringstream keys(sConfigMgr->GetStringDefault("Battleground.AllowedWorlds", ""));
    std::string key;
    while (keys >> key) options.WorldKeys.insert(key);
    std::istringstream realms(sConfigMgr->GetStringDefault("Battleground.Realms", ""));
    while (realms >> key)
    {
        uint64 realm = 0;
        if (!Handle(key, realm) || realm > 0xffffffffULL || !options.Realms.insert(std::uint32_t(realm)).second)
        { SF_LOG_ERROR("server.battleground", "Battleground.Realms must contain unique positive 32-bit realm IDs."); return 1; }
    }
    std::istringstream scopes(sConfigMgr->GetStringDefault("Battleground.WorldRealms", ""));
    while (scopes >> key)
    {
        auto split = key.find('=');
        std::string identity = key.substr(0, split);
        if (split == std::string::npos || !Skyfire::Cluster::ValidKey(identity) || options.WorldRealms.count(identity))
        { SF_LOG_ERROR("server.battleground", "Battleground.WorldRealms requires unique node=realm,realm entries."); return 1; }
        std::istringstream ids(key.substr(split + 1)); std::string value;
        auto& allowed = options.WorldRealms[identity];
        while (std::getline(ids, value, ','))
        {
            uint64 realm = 0;
            if (!Handle(value, realm) || realm > 0xffffffffULL || !allowed.insert(std::uint32_t(realm)).second)
            { SF_LOG_ERROR("server.battleground", "Invalid realm in Battleground.WorldRealms."); return 1; }
        }
    }
    Skyfire::Cluster::Node node;
    node.Type = Skyfire::Cluster::Service::Battleground; node.Port = options.Port;
    node.Build = 18414; node.Capabilities = Skyfire::BattlegroundService::Capability;
    Skyfire::Cluster::AgentOptions agentOptions;
    if (!Skyfire::Cluster::LoadAgentOptions(node, agentOptions, error) || !agentOptions.Enabled ||
        agentOptions.HandoffEnabled || agentOptions.RealmDirectoryEnabled)
    { SF_LOG_ERROR("server.battleground", "Battleground service requires cluster TLS registration without auth handoff: %s", error.c_str()); return 1; }
    if (!expectedKey.empty() && expectedKey != agentOptions.Advertisement.Key)
    { SF_LOG_ERROR("server.battleground", "Managed cluster key does not match battleground configuration."); return 1; }
    agentOptions.Advertisement.Capacity = options.MaxConnections;
    Skyfire::BattlegroundService::Server server;
    if (!server.Open(std::move(options), agentOptions, error))
    { SF_LOG_ERROR("server.battleground", "%s", error.c_str()); return 1; }
    std::mutex sampleMutex;
    Skyfire::Cluster::AgentSample sample;
    sample.Ready=false; sample.Battleground=server.Metrics();
    Skyfire::Cluster::Agent agent;
    if(!agent.Start(agentOptions,[&](){std::lock_guard<std::mutex> lock(sampleMutex);return sample;},error))
    {SF_LOG_ERROR("server.battleground","%s",error.c_str());return 1;}
    std::signal(SIGINT,Signal);std::signal(SIGTERM,Signal);
#ifdef _WIN32
    std::signal(SIGBREAK,Signal);
#else
    std::signal(SIGPIPE,SIG_IGN);
#endif
    bool registered=false;
    if(control) channel.SendStatus(Skyfire::HubControl::StartingMessage);
    auto heartbeat=std::chrono::steady_clock::now();
    int result=0;
    try
    {
        while(!Stopping && !(control&&channel.StopRequested()))
        {
            server.SetReady(agent.IsRegistered());server.Update();
            bool ready=server.Ready();
            {
                std::lock_guard<std::mutex> lock(sampleMutex);
                sample.Ready=ready;sample.Battleground=server.Metrics();sample.Load=sample.Battleground.QueuedPlayers;
            }
            if(control && ready!=registered)
            {channel.SendStatus(ready?Skyfire::HubControl::ReadyMessage:"NOT_READY");registered=ready;}
            auto now=std::chrono::steady_clock::now();
            if(control && now-heartbeat>=std::chrono::seconds(1))
            {if(!channel.SendStatus(Skyfire::HubControl::HeartbeatMessage)) break;heartbeat=now;}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    catch(std::exception const&) {SF_LOG_ERROR("server.battleground","Battleground event loop failed; stopping.");result=1;}
    if(control) channel.SendStatus(Skyfire::HubControl::StoppingMessage);
    {std::lock_guard<std::mutex> lock(sampleMutex);sample.Ready=false;}
    server.Stop();agent.Stop();return result;
}
