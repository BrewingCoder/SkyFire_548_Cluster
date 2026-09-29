/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_BATTLEGROUND_QUEUE_BOOK_H
#define SKYFIRE_BATTLEGROUND_QUEUE_BOOK_H
#include "BattlegroundMatcher.h"
#include <map>
#include <string>
#include <utility>

namespace Skyfire::BattlegroundService
{
    // All calls belong to the daemon event loop. The world remains authoritative
    // for admission: plans are proposals, never durable player invitations.
    class QueueBook
    {
        struct Queue
        {
            std::uint64_t Sequence=0, Updated=0;
            std::size_t Groups=0, Players=0;
        };
        struct Owner
        {
            std::string Node, Generation;
            std::uint64_t Updated=0;
            std::set<std::string> Retired;
            std::map<std::pair<std::uint32_t,std::uint8_t>,Queue> Queues;
        };
        std::map<std::uint32_t,Owner> Realms;
        std::string Error;
    public:
        static constexpr std::uint64_t OwnerLeaseMs=15000, SnapshotLeaseMs=20000;
        std::string const& LastError() const { return Error; }
        bool Accept(std::string const& node, Snapshot const& snapshot, std::uint64_t now, Response& response)
        {
            response={};Error.clear();
            auto reject=[&](char const* message){Error=message;return false;};
            if (node.empty() || node.size()>64 || node.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-")!=std::string::npos || !ValidSnapshot(snapshot)) return reject("Invalid queue snapshot");
            Expire(now);
            auto found=Realms.find(snapshot.Realm);
            if (found==Realms.end())
            {
                if(Realms.size()>=64) return reject("Queue realm capacity reached");
                found=Realms.emplace(snapshot.Realm,Owner{}).first;
            }
            auto& owner=found->second;
            if(owner.Generation==snapshot.Generation && owner.Node!=node) return reject("Queue generation belongs to another world");
            bool replacing=owner.Node!=node || owner.Generation!=snapshot.Generation;
            if (replacing && !owner.Generation.empty())
            {
                if (now<owner.Updated || now-owner.Updated<OwnerLeaseMs) return reject("Queue realm has a live world owner");
                if (owner.Retired.count(snapshot.Generation)) return reject("Retired queue generation");
                if (owner.Retired.size()>=4096) return reject("Queue generation history full; stop realm worlds before restarting the battleground daemon");
            }
            if (!replacing && now<owner.Updated) return reject("Queue clock moved backwards");
            auto key=std::make_pair(snapshot.Type,snapshot.Bracket);
            auto queue=owner.Queues.find(key);
            if (!replacing && queue!=owner.Queues.end() && snapshot.Sequence<=queue->second.Sequence) return reject("Queue snapshot replay");
            if (!replacing && queue==owner.Queues.end() && owner.Queues.size()>=256) return reject("Queue type/bracket capacity reached");
            if (!PlanMatches(snapshot,response)) return reject("Queue planning rejected snapshot");
            if (replacing)
            {
                if (!owner.Generation.empty()) owner.Retired.insert(owner.Generation);
                owner.Node=node; owner.Generation=snapshot.Generation; owner.Queues.clear();
            }
            owner.Updated=now;
            Queue state; state.Sequence=snapshot.Sequence; state.Updated=now; state.Groups=snapshot.Groups.size();
            for(auto const& group:snapshot.Groups) state.Players+=group.Members.size();
            owner.Queues[key]=state;
            return true;
        }
        void Expire(std::uint64_t now)
        {
            for(auto& realm:Realms)
                for(auto& item:realm.second.Queues)
                    if(now>=item.second.Updated && now-item.second.Updated>=SnapshotLeaseMs)
                    { item.second.Groups=0;item.second.Players=0; }
            // Retain sequence watermarks and retired generations: forgetting an
            // idle snapshot must never make an old packet valid again.
        }
        void ClearQueues()
        {
            for(auto& realm:Realms)
                for(auto& item:realm.second.Queues) { item.second.Groups=0;item.second.Players=0; }
        }
        std::size_t QueuedGroups() const
        { std::size_t count=0;for(auto const& realm:Realms)for(auto const& item:realm.second.Queues)count+=item.second.Groups;return count; }
        std::size_t QueuedPlayers() const
        { std::size_t count=0;for(auto const& realm:Realms)for(auto const& item:realm.second.Queues)count+=item.second.Players;return count; }
    };
}
#endif
