/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_BATTLEGROUND_PROTOCOL_H
#define SKYFIRE_BATTLEGROUND_PROTOCOL_H
#include "ClusterProtocol.h"
#include <array>
#include <set>
#include <tuple>

namespace Skyfire::BattlegroundService
{
    constexpr std::uint32_t Capability = 4096;
    constexpr Cluster::Message MetricsType = Cluster::Message(15);
    constexpr std::size_t MaxFrame = 128 * 1024, MaxGroups = 1024, MaxMembers = 4096, MaxRunning = 128, MaxPlans = 32;
    struct Group
    {
        std::uint64_t Id = 0;
        std::uint8_t Team = 0;
        bool Premade = false;
        std::uint32_t WaitMs = 0;
        std::vector<std::uint64_t> Members;
        std::uint64_t ArenaTeam = 0;
        std::uint32_t Rating = 0, MatchmakerRating = 0;
    };
    struct RunningMatch { std::uint32_t Instance = 0; std::array<std::uint32_t, 2> Free{}; };
    struct Snapshot
    {
        std::uint32_t Realm = 0;
        std::string Generation;
        std::uint64_t Sequence = 0;
        std::uint32_t Type = 0;
        std::uint8_t Bracket = 0;
        std::uint32_t Min = 0, Max = 0;
        bool Testing = false;
        std::uint8_t InvitationType = 0;
        std::uint32_t PremadeWaitMs = 0;
        std::vector<Group> Groups;
        std::vector<RunningMatch> Running;
        std::uint8_t ArenaType = 0;
        bool Rated = false;
        std::uint32_t MaxRatingDifference = 0, RatingDiscardMs = 0;
    };
    using QueueKey = std::tuple<std::uint32_t, std::uint8_t, std::uint8_t, bool>;
    inline QueueKey KeyFor(Snapshot const& s) { return {s.Type, s.Bracket, s.ArenaType, s.Rated}; }
    struct Plan { std::uint32_t Instance = 0; std::array<std::vector<std::uint64_t>, 2> Teams; };
    struct Response { std::uint64_t Sequence = 0; std::vector<Plan> Plans; };
    inline void Write64(Cluster::Writer& out, std::uint64_t value)
    { out.U32(std::uint32_t(value >> 32)); out.U32(std::uint32_t(value)); }
    inline bool Read64(Cluster::Reader& in, std::uint64_t& value)
    { std::uint32_t hi,lo; if (!in.U32(hi) || !in.U32(lo)) return false; value = (std::uint64_t(hi)<<32)|lo; return true; }
    inline bool ValidSnapshot(Snapshot const& s)
    {
        if (!s.Realm || !s.Sequence || s.Generation.size()!=64 || s.Generation.find_first_not_of("0123456789abcdef")!=std::string::npos ||
            !s.Type || s.Type>255 || s.Bracket>31 || !s.Min || s.Min>s.Max || s.Max>40 || s.InvitationType>1 || s.Groups.size()>MaxGroups || s.Running.size()>MaxRunning) return false;
        if (s.ArenaType)
        {
            if ((s.ArenaType != 2 && s.ArenaType != 3 && s.ArenaType != 5) || s.Max != s.ArenaType ||
                s.Min != (s.Testing ? 1u : s.ArenaType) || !s.Running.empty()) return false;
        }
        else if (s.Rated || s.MaxRatingDifference || s.RatingDiscardMs) return false;
        std::set<std::uint64_t> groups,players; std::set<std::uint32_t> running;
        for (auto const& group:s.Groups)
        {
            if (!group.Id || group.Team>1 || group.Members.empty() || group.Members.size()>s.Max || !groups.insert(group.Id).second) return false;
            if (s.Rated && (!group.ArenaTeam || group.Members.size() < s.Min)) return false;
            if (!s.Rated && (group.ArenaTeam || group.Rating || group.MatchmakerRating)) return false;
            for (auto player:group.Members) if (!player || !players.insert(player).second || players.size()>MaxMembers) return false;
        }
        for (auto const& match:s.Running)
            if (!match.Instance || !running.insert(match.Instance).second || match.Free[0]>s.Max || match.Free[1]>s.Max) return false;
        return true;
    }
    inline Cluster::Writer EncodeSnapshot(Snapshot const& s)
    {
        Cluster::Writer out; out.U8(2); out.U32(s.Realm); out.String(s.Generation); Write64(out,s.Sequence);
        out.U32(s.Type); out.U8(s.Bracket); out.U32(s.Min); out.U32(s.Max); out.U8(s.Testing); out.U8(s.InvitationType); out.U32(s.PremadeWaitMs);
        out.U8(s.ArenaType); out.U8(s.Rated); out.U32(s.MaxRatingDifference); out.U32(s.RatingDiscardMs);
        out.U16(std::uint16_t(s.Groups.size()));
        for(auto const& group:s.Groups)
        {
            Write64(out,group.Id); out.U8(group.Team); out.U8(group.Premade); out.U32(group.WaitMs); out.U16(std::uint16_t(group.Members.size()));
            for(auto member:group.Members) Write64(out,member);
            Write64(out,group.ArenaTeam); out.U32(group.Rating); out.U32(group.MatchmakerRating);
        }
        out.U16(std::uint16_t(s.Running.size()));
        for(auto const& match:s.Running) { out.U32(match.Instance); out.U32(match.Free[0]); out.U32(match.Free[1]); }
        return out;
    }
    inline bool DecodeSnapshot(std::vector<std::uint8_t> const& bytes, Snapshot& output)
    {
        if(bytes.size()>MaxFrame) return false;
        Cluster::Reader in(bytes); Snapshot s; std::uint8_t version,testing,rated; std::uint16_t count;
        if(!in.U8(version)||version!=2||!in.U32(s.Realm)||!in.String(s.Generation,64)||!Read64(in,s.Sequence)||!in.U32(s.Type)||
            !in.U8(s.Bracket)||!in.U32(s.Min)||!in.U32(s.Max)||!in.U8(testing)||testing>1||!in.U8(s.InvitationType)||!in.U32(s.PremadeWaitMs)||
            !in.U8(s.ArenaType)||!in.U8(rated)||rated>1||!in.U32(s.MaxRatingDifference)||!in.U32(s.RatingDiscardMs)||!in.U16(count)||count>MaxGroups) return false;
        s.Testing=testing!=0; s.Rated=rated!=0;
        for(unsigned i=0;i<count;++i)
        {
            Group group; std::uint8_t premade; std::uint16_t members;
            if(!Read64(in,group.Id)||!in.U8(group.Team)||!in.U8(premade)||premade>1||!in.U32(group.WaitMs)||!in.U16(members)||members>40) return false;
            group.Premade=premade!=0;
            for(unsigned j=0;j<members;++j) { std::uint64_t member; if(!Read64(in,member)) return false; group.Members.push_back(member); }
            if(!Read64(in,group.ArenaTeam)||!in.U32(group.Rating)||!in.U32(group.MatchmakerRating)) return false;
            s.Groups.push_back(std::move(group));
        }
        if(!in.U16(count)||count>MaxRunning) return false;
        for(unsigned i=0;i<count;++i) { RunningMatch match; if(!in.U32(match.Instance)||!in.U32(match.Free[0])||!in.U32(match.Free[1])) return false; s.Running.push_back(match); }
        if(!in.End()||!ValidSnapshot(s)) return false; output=std::move(s); return true;
    }
    inline Cluster::Writer EncodeResponse(Response const& response)
    {
        Cluster::Writer out; out.U8(1); Write64(out,response.Sequence); out.U16(std::uint16_t(response.Plans.size()));
        for(auto const& plan:response.Plans)
        {
            out.U32(plan.Instance);
            for(auto const& team:plan.Teams) { out.U16(std::uint16_t(team.size())); for(auto group:team) Write64(out,group); }
        }
        return out;
    }
    inline bool DecodeResponse(std::vector<std::uint8_t> const& bytes, Response& output)
    {
        if(bytes.size()>MaxFrame) return false;
        Cluster::Reader in(bytes); Response response; std::uint8_t version; std::uint16_t count; std::set<std::uint64_t> used;
        if(!in.U8(version)||version!=1||!Read64(in,response.Sequence)||!response.Sequence||!in.U16(count)||count>MaxPlans) return false;
        for(unsigned i=0;i<count;++i)
        {
            Plan plan; if(!in.U32(plan.Instance)) return false;
            for(auto& team:plan.Teams)
            {
                std::uint16_t groups; if(!in.U16(groups)||groups>40) return false;
                for(unsigned j=0;j<groups;++j) { std::uint64_t group; if(!Read64(in,group)||!group||!used.insert(group).second||used.size()>MaxGroups) return false; team.push_back(group); }
            }
            if(plan.Teams[0].empty()&&plan.Teams[1].empty()) return false;
            response.Plans.push_back(std::move(plan));
        }
        if(!in.End()) return false; output=std::move(response); return true;
    }
    inline Cluster::Writer EncodeMetrics(Cluster::BattlegroundMetrics const& m)
    {
        Cluster::Writer out; out.U8(1); out.U32(m.Uptime); out.U32(m.Connections); out.U32(m.Requests); out.U32(m.Failures);
        out.U32(m.QueuedGroups); out.U32(m.QueuedPlayers); out.U32(m.Proposals); out.U16(std::uint16_t(m.Realms.size()));
        for(auto realm:m.Realms) out.U32(realm); return out;
    }
    inline bool DecodeMetrics(std::vector<std::uint8_t> const& bytes, Cluster::BattlegroundMetrics& m)
    {
        Cluster::Reader in(bytes); std::uint8_t version; std::uint16_t count; std::set<std::uint32_t> realms;
        if(!in.U8(version)||version!=1||!in.U32(m.Uptime)||!in.U32(m.Connections)||m.Connections>128||!in.U32(m.Requests)||!in.U32(m.Failures)||
            !in.U32(m.QueuedGroups)||!in.U32(m.QueuedPlayers)||!in.U32(m.Proposals)||!in.U16(count)||!count||count>64) return false;
        for(unsigned i=0;i<count;++i) { std::uint32_t realm; if(!in.U32(realm)||!realm||!realms.insert(realm).second) return false; }
        if(!in.End()) return false; m.Realms.assign(realms.begin(),realms.end()); return true;
    }
}
#endif
