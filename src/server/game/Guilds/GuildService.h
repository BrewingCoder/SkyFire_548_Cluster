/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_GAME_GUILD_SERVICE_H
#define SKYFIRE_GAME_GUILD_SERVICE_H
#include "Cluster/GuildWire.h"
#include "Cluster/ChatClient.h"
#include <functional>
class Guild;
class WorldSession;
namespace Skyfire::Chat::GuildService
{
    bool Enabled();
    bool Applying();
    bool Message(WorldSession* session, std::uint32_t guild, bool officer, std::string const& text,
        std::uint32_t language, std::string const& prefix = "");
    void Update();
    bool Blocked(std::uint32_t guild);
    bool Submit(WorldSession* session, std::uint32_t guild, GuildState::Command command,
        std::function<void(Guild*, WorldSession*)> applied = {}, GuildRequest::BankPermissions bank = {});
    bool Create(WorldSession* session, std::uint32_t guild, std::string const& name, std::uint32_t petition,
        std::uint32_t minimumSignatures, bool gameMaster, std::vector<std::uint64_t> members,
        std::function<void(Guild*, WorldSession*)> applied = {});
    bool Admin(WorldSession* session, GuildAdmin operation, std::uint32_t guild, std::uint64_t target,
        std::uint32_t rank, std::string const& text);
    void HandleResult(ServiceResult const& result);
}
#endif
