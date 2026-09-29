/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/

#ifndef SF_GUILDMGR_H
#define SF_GUILDMGR_H

#include "Guild.h"
#include "Platform/Singleton.h"

class GuildMgr
{
    friend class Skyfire::Singleton<GuildMgr, Skyfire::NullMutex>;

private:
    GuildMgr();
    ~GuildMgr();

public:
    Guild* GetGuildByLeader(uint64 guid) const;
    Guild* GetGuildById(uint32 guildId) const;
    Guild* GetGuildByGuid(uint64 guid) const;
    Guild* GetGuildByName(std::string const& guildName) const;
    std::string GetGuildNameById(uint32 guildId) const;

    void LoadGuildXpForLevel();
    void LoadGuildRewards();

    void LoadGuilds();
    void AddGuild(Guild* guild);
    void RemoveGuild(uint32 guildId);

    void SaveGuilds();
    std::vector<uint32> GetGuildIds() const;

    void ResetReputationCaps();

    uint32 GenerateGuildId();
    void SetNextGuildId(uint32 Id) { NextGuildId = Id; }

    uint32 GetXPForGuildLevel(uint8 level) const;
    std::vector<GuildReward> const& GetGuildRewards() const { return GuildRewards; }

    void ResetTimes(bool week);
protected:
    typedef UNORDERED_MAP<uint32, Guild*> GuildContainer;
    uint32 NextGuildId;
    GuildContainer GuildStore;
    std::vector<uint64> GuildXPperLevel;
    std::vector<GuildReward> GuildRewards;
};

#define sGuildMgr Skyfire::Singleton<GuildMgr, Skyfire::NullMutex>::instance()

#endif
