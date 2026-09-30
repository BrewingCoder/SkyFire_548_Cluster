/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "BattlegroundQueue.h"
#include "BattlegroundMgr.h"
#include "DBCStores.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Timer.h"
#include "World.h"
#include "Group.h"
#include "Cluster/BattlegroundMatcher.h"
#include <algorithm>
#include <memory>
#include <set>

bool BattlegroundQueue::EligibleRemoteGroup(GroupQueueInfo const& group, BattlegroundTypeId type, BattlegroundBracketId bracket, uint8 arenaType, bool rated) const
{
    auto* model = sBattlegroundMgr->GetBattlegroundTemplate(type);
    if (!model || (model->isArena() != bool(arenaType)) || group.BgTypeId != type || group.IsRated != rated || group.ArenaType != arenaType ||
        group.IsInvitedToBGInstanceGUID || !group.RemoteId || group.Players.empty() ||
        group.Players.size() > (arenaType ? arenaType : model->GetMaxPlayersPerTeam())) return false;
    if (rated && (!group.ArenaGroup || group.Players.size() < (sBattlegroundMgr->isArenaTesting() ? 1u : arenaType))) return false;
    auto queue = BattlegroundMgr::BGQueueTypeId(type,arenaType);
    for (auto const& member : group.Players)
    {
        auto* player = ObjectAccessor::FindPlayer(member.first);
        auto local = m_QueuedPlayers.find(member.first);
        if (!player || !player->GetSession() || !player->IsInWorld() || player->InBattleground() || player->GetTeam() != group.Team ||
            !player->CanJoinToBattleground(model) || !player->InBattlegroundQueueForBattlegroundQueueType(queue) ||
            local == m_QueuedPlayers.end() || local->second.GroupInfo != &group ||
            (rated && player->GetGroup() != group.ArenaGroup)) return false;
        auto* levelBracket = GetBattlegroundBracketByLevel(model->GetMapId(),player->getLevel());
        if (!levelBracket || levelBracket->GetBracketId() != bracket) return false;
    }
    return true;
}

void BattlegroundQueue::UpdateRemoteQueues()
{
    if (!Skyfire::BattlegroundService::Enabled()) return;
    for (auto const& context : m_RemoteContexts)
        SubmitRemoteQueue(BattlegroundTypeId(std::get<0>(context.first)),BattlegroundBracketId(std::get<1>(context.first)),
            std::get<2>(context.first),std::get<3>(context.first));
}

void BattlegroundQueue::SubmitRemoteQueue(BattlegroundTypeId type, BattlegroundBracketId bracket, uint8 arenaType, bool rated)
{
    namespace Remote = Skyfire::BattlegroundService;
    if (uint8(bracket) >= MAX_BATTLEGROUND_BRACKETS) return;
    auto& context = m_RemoteContexts[{uint32(type),uint8(bracket),arenaType,rated}];
    auto now = std::chrono::steady_clock::now();
    if (context.Pending && now - context.Submitted < std::chrono::seconds(10)) return;
    context.Pending = 0;
    if (now < context.Next) return;
    context.Next = now + std::chrono::seconds(1);
    auto* model = sBattlegroundMgr->GetBattlegroundTemplate(type);
    if (!model || (model->isArena() != bool(arenaType))) return;
    Remote::Snapshot snapshot; snapshot.Realm = realmID; snapshot.Type = uint32(type); snapshot.Bracket = uint8(bracket);
    snapshot.ArenaType = arenaType; snapshot.Rated = rated;
    snapshot.Testing = arenaType ? sBattlegroundMgr->isArenaTesting() : sBattlegroundMgr->isTesting();
    snapshot.Min = snapshot.Testing ? 1 : (arenaType ? arenaType : model->GetMinPlayersPerTeam());
    snapshot.Max = arenaType ? arenaType : model->GetMaxPlayersPerTeam();
    if (rated) { snapshot.MaxRatingDifference = sBattlegroundMgr->GetMaxRatingDifference(); snapshot.RatingDiscardMs = sBattlegroundMgr->GetRatingDiscardTimer(); }
    snapshot.PremadeWaitMs = sWorld->getIntConfig(WorldIntConfigs::CONFIG_BATTLEGROUND_PREMADE_GROUP_WAIT_FOR_MATCH);
    snapshot.InvitationType = sWorld->getIntConfig(WorldIntConfigs::CONFIG_BATTLEGROUND_INVITATION_TYPE) ? 1 : 0;
    std::size_t members = 0;
    // Round-robin whole groups across both factions and premade/normal queues.
    // A bounded oldest-first prefix can progress even when the local queue is
    // larger than the wire limits; invited groups leave the next snapshot.
    std::array<std::size_t,BG_QUEUE_GROUP_TYPES_COUNT> offsets{};
    bool more = true;
    while (more && snapshot.Groups.size() < Remote::MaxGroups)
    {
        more = false;
        for (unsigned list = 0; list < BG_QUEUE_GROUP_TYPES_COUNT && snapshot.Groups.size() < Remote::MaxGroups; ++list)
        {
            auto const& groups = m_QueuedGroups[uint8(bracket)][list];
            if (offsets[list] >= groups.size()) continue;
            more = true;
            auto* group = groups[offsets[list]++];
            if (!EligibleRemoteGroup(*group,type,bracket,arenaType,rated) || members + group->Players.size() > Remote::MaxMembers) continue;
            Remote::Group entry; entry.Id = group->RemoteId; entry.Team = group->Team == ALLIANCE ? 0 : 1;
            entry.Premade = list < BG_QUEUE_NORMAL_ALLIANCE; entry.WaitMs = getMSTimeDiff(group->JoinTime,getMSTime());
            for (auto const& player : group->Players) entry.Members.push_back(player.first);
            if (rated) { entry.ArenaTeam = group->ArenaGroup->GetGUID(); entry.Rating = group->ArenaTeamRating; entry.MatchmakerRating = group->ArenaMatchmakerRating; }
            members += entry.Members.size(); snapshot.Groups.push_back(std::move(entry));
        }
    }
    for (auto* bg : sBattlegroundMgr->GetBGFreeSlotQueueStore(type))
        if (!arenaType && !bg->ToBeDeleted() && !bg->isRated() && bg->GetTypeID() == type && bg->GetBracketId() == bracket &&
            bg->GetStatus() > STATUS_WAIT_QUEUE && bg->GetStatus() < STATUS_WAIT_LEAVE)
        {
            if (snapshot.Running.size() >= Remote::MaxRunning) break;
            if (bg->GetFreeSlotsForTeam(ALLIANCE) > snapshot.Max || bg->GetFreeSlotsForTeam(HORDE) > snapshot.Max) continue;
            snapshot.Running.push_back({bg->GetInstanceID(),{bg->GetFreeSlotsForTeam(ALLIANCE),bg->GetFreeSlotsForTeam(HORDE)}});
        }
    context.Pending = Remote::Submit(std::move(snapshot)); context.Submitted = now;
}

void BattlegroundQueue::HandleRemoteResult(Skyfire::BattlegroundService::Result const& result)
{
    namespace Remote = Skyfire::BattlegroundService;
    auto const& snapshot = result.Request;
    auto found = m_RemoteContexts.find(Remote::KeyFor(snapshot));
    if (found == m_RemoteContexts.end() || !found->second.Pending || found->second.Pending != snapshot.Sequence) return;
    auto& context = found->second; context.Pending = 0;
    if (!Remote::Enabled() || !result.Success || result.Matches.Sequence != snapshot.Sequence ||
        std::chrono::steady_clock::now() - context.Submitted >= std::chrono::seconds(10) || snapshot.Bracket >= MAX_BATTLEGROUND_BRACKETS) return;
    if (snapshot.Groups.empty())
    {
        if (!result.Matches.Plans.empty()) return;
        for (unsigned list = 0; list < BG_QUEUE_GROUP_TYPES_COUNT; ++list)
            for (auto* group : m_QueuedGroups[snapshot.Bracket][list])
                if (uint32(group->BgTypeId) == snapshot.Type && group->ArenaType == snapshot.ArenaType && group->IsRated == snapshot.Rated && !group->IsInvitedToBGInstanceGUID) return;
        m_RemoteContexts.erase(found); return;
    }
    auto type = BattlegroundTypeId(snapshot.Type); auto bracket = BattlegroundBracketId(snapshot.Bracket);
    auto arenaType = snapshot.ArenaType; bool rated = snapshot.Rated;
    auto* model = sBattlegroundMgr->GetBattlegroundTemplate(type);
    if (!model || model->isArena() != bool(arenaType) || snapshot.Max != (arenaType ? arenaType : model->GetMaxPlayersPerTeam()) ||
        snapshot.InvitationType != (sWorld->getIntConfig(WorldIntConfigs::CONFIG_BATTLEGROUND_INVITATION_TYPE) ? 1 : 0) ||
        snapshot.Testing != (arenaType ? sBattlegroundMgr->isArenaTesting() : sBattlegroundMgr->isTesting()) ||
        snapshot.Min != (snapshot.Testing ? 1 : (arenaType ? arenaType : model->GetMinPlayersPerTeam()))) return;
    if (rated && (snapshot.MaxRatingDifference != sBattlegroundMgr->GetMaxRatingDifference() || snapshot.RatingDiscardMs != sBattlegroundMgr->GetRatingDiscardTimer())) return;
    auto* bracketEntry = GetBattlegroundBracketById(model->GetMapId(),bracket);
    if (!bracketEntry) return;
    std::map<uint64,GroupQueueInfo*> current;
    for (unsigned list = 0; list < BG_QUEUE_GROUP_TYPES_COUNT; ++list)
        for (auto* group : m_QueuedGroups[snapshot.Bracket][list]) current.emplace(group->RemoteId,group);
    struct Prepared { Battleground* Match = nullptr; std::array<std::vector<GroupQueueInfo*>,2> Teams; };
    std::vector<Prepared> prepared;
    std::set<uint64> used; std::set<uint32> instances;
    for (auto const& plan : result.Matches.Plans)
    {
        Prepared item; std::array<uint32,2> counts{};
        for (unsigned team = 0; team < 2; ++team)
            for (auto id : plan.Teams[team])
            {
                auto original = std::find_if(snapshot.Groups.begin(),snapshot.Groups.end(),[&](auto const& group) { return group.Id == id; });
                auto live = current.find(id);
                if (!used.insert(id).second || original == snapshot.Groups.end() || live == current.end() || (!arenaType && original->Team != team) ||
                    !EligibleRemoteGroup(*live->second,type,bracket,arenaType,rated) || original->Team != (live->second->Team == ALLIANCE ? 0 : 1)) return;
                std::vector<uint64> members;
                for (auto const& member : live->second->Players) members.push_back(member.first);
                if (members != original->Members) return;
                if (rated && (original->ArenaTeam != live->second->ArenaGroup->GetGUID() || original->Rating != live->second->ArenaTeamRating ||
                    original->MatchmakerRating != live->second->ArenaMatchmakerRating)) return;
                counts[team] += uint32(members.size()); item.Teams[team].push_back(live->second);
            }
        if (counts[0] > snapshot.Max || counts[1] > snapshot.Max || (!counts[0] && !counts[1])) return;
        if (plan.Instance)
        {
            if (arenaType) return;
            if (!instances.insert(plan.Instance).second) return;
            auto original = std::find_if(snapshot.Running.begin(),snapshot.Running.end(),[&](auto const& match) { return match.Instance == plan.Instance; });
            if (original == snapshot.Running.end() || counts[0] > original->Free[0] || counts[1] > original->Free[1]) return;
            for (auto* candidate : sBattlegroundMgr->GetBGFreeSlotQueueStore(type)) if (candidate->GetInstanceID() == plan.Instance) item.Match = candidate;
            if (!item.Match || item.Match->ToBeDeleted() || item.Match->isRated() || item.Match->GetTypeID() != type || item.Match->GetBracketId() != bracket ||
                item.Match->GetStatus() <= STATUS_WAIT_QUEUE || item.Match->GetStatus() >= STATUS_WAIT_LEAVE ||
                counts[0] > item.Match->GetFreeSlotsForTeam(ALLIANCE) || counts[1] > item.Match->GetFreeSlotsForTeam(HORDE)) return;
            if (!snapshot.Testing && snapshot.InvitationType)
            {
                uint32 freeAlliance = item.Match->GetFreeSlotsForTeam(ALLIANCE);
                uint32 freeHorde = item.Match->GetFreeSlotsForTeam(HORDE);
                uint32 before = std::max(freeAlliance,freeHorde) - std::min(freeAlliance,freeHorde);
                uint32 leftAlliance = freeAlliance - counts[0], leftHorde = freeHorde - counts[1];
                uint32 after = std::max(leftAlliance,leftHorde) - std::min(leftAlliance,leftHorde);
                if (after > 1 && after > before) return;
            }
        }
        else
        {
            if (arenaType ? (counts[0] < snapshot.Min || counts[1] < snapshot.Min) :
                (snapshot.Testing ? (!counts[0] && !counts[1]) : (counts[0] < snapshot.Min || counts[1] < snapshot.Min))) return;
            if (rated)
            {
                if (item.Teams[0].size() != 1 || item.Teams[1].size() != 1) return;
                auto first = std::find_if(snapshot.Groups.begin(),snapshot.Groups.end(),[&](auto const& g) { return g.Id == item.Teams[0][0]->RemoteId; });
                auto second = std::find_if(snapshot.Groups.begin(),snapshot.Groups.end(),[&](auto const& g) { return g.Id == item.Teams[1][0]->RemoteId; });
                if (first == snapshot.Groups.end() || second == snapshot.Groups.end() || !Remote::RatedArenaCompatible(snapshot,*first,*second)) return;
            }
            if (!snapshot.Testing && snapshot.InvitationType && std::max(counts[0],counts[1]) - std::min(counts[0],counts[1]) > 2) return;
        }
        prepared.push_back(std::move(item));
    }
    // Validate the entire response before allocating or inviting any player.
    // Failed allocation batches enter the manager's normal deletion path so
    // reserved client-visible instance identifiers are released as well.
    std::vector<std::unique_ptr<Battleground,void(*)(Battleground*)>> created;
    for (auto& item : prepared)
        if (!item.Match)
        {
            auto* match = sBattlegroundMgr->CreateNewBattleground(type,bracketEntry,arenaType,rated);
            if (!match) return;
            created.emplace_back(match,[](Battleground* abandoned) { abandoned->StartBattleground(); abandoned->SetDeleteThis(); });
            item.Match = match;
            for (unsigned team = 0; team < 2; ++team)
            {
                uint32 count = 0;
                for (auto* group : item.Teams[team]) count += uint32(group->Players.size());
                if (count > (arenaType ? arenaType : match->GetMaxPlayersPerTeam()) || (!snapshot.Testing && count < (arenaType ? arenaType : match->GetMinPlayersPerTeam()))) return;
            }
        }
    for (auto& item : prepared)
    {
        if (rated)
        {
            auto* first = item.Teams[0][0]; auto* second = item.Teams[1][0];
            first->OpponentsTeamRating = second->ArenaTeamRating; second->OpponentsTeamRating = first->ArenaTeamRating;
            first->OpponentsMatchmakerRating = second->ArenaMatchmakerRating; second->OpponentsMatchmakerRating = first->ArenaMatchmakerRating;
            item.Match->SetArenaMatchmakerRating(ALLIANCE, first->ArenaMatchmakerRating);
            item.Match->SetArenaMatchmakerRating(HORDE, second->ArenaMatchmakerRating);
        }
        for (unsigned team = 0; team < 2; ++team)
            for (auto* group : item.Teams[team])
            {
                auto side = team ? HORDE : ALLIANCE;
                if (arenaType && group->Team != side)
                {
                    auto base = rated ? BG_QUEUE_PREMADE_ALLIANCE : BG_QUEUE_NORMAL_ALLIANCE;
                    auto& previous = m_QueuedGroups[snapshot.Bracket][base + (group->Team == ALLIANCE ? 0 : 1)];
                    previous.erase(std::remove(previous.begin(),previous.end(),group),previous.end());
                    m_QueuedGroups[snapshot.Bracket][base + team].push_back(group);
                }
                InviteGroupToBG(group,item.Match,side);
            }
        if (!item.Match->HasFreeSlots()) item.Match->RemoveFromBGFreeSlotQueue();
    }
    for (auto& match : created) { match->StartBattleground(); match.release(); }
}
