/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "Cluster/ChatClient.h"
#include "Cluster/GuildWire.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include <chrono>
#include <iostream>
#include <thread>

// Shared logging links its database appender; this fixture never opens a pool.
DatabaseWorkerPool<LoginDatabaseConnection> LoginDatabase;

// Driven by chat_failover_fixture.py against disposable, real TLS daemons.
int main(int argc, char** argv)
{
    if (argc != 2 || !sConfigMgr->LoadInitial(argv[1])) return 1;
    Skyfire::Cluster::AgentOptions options;
    options.Enabled = true;
    options.CA = sConfigMgr->GetStringDefault("Cluster.CA", "");
    options.Certificate = sConfigMgr->GetStringDefault("Cluster.Certificate", "");
    options.PrivateKey = sConfigMgr->GetStringDefault("Cluster.PrivateKey", "");
    std::string error;
    if (!Skyfire::Chat::StartClient(options, error)) { std::cerr << error << std::endl; return 2; }
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    auto next = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < until)
    {
        if (std::chrono::steady_clock::now() >= next)
        {
            Skyfire::Chat::PublishPresence({{40,400,1,"ClientProbe"}});
            Skyfire::Chat::GuildRequest guild;
            guild.Guild = 77; guild.Command.Type = Skyfire::Chat::GuildState::Action::Roster;
            Skyfire::Chat::ServiceRequest request;
            request.Domain = Skyfire::Chat::ServiceDomain::Guild;
            request.Account = 40; request.Actor = 400; request.Incarnation = 1;
            request.Payload = Skyfire::Chat::EncodeGuildRequest(guild);
            Skyfire::Chat::QueueServiceRequest(std::move(request));
            next = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        }
        for (auto const& result : Skyfire::Chat::TakeServiceResults())
            if (result.Success)
                std::cout << "CHAT_ENDPOINT " << Skyfire::Chat::ClientNodeKey() << std::endl;
        Skyfire::Chat::TakeServiceEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Skyfire::Chat::StopClient();
    return 0;
}
