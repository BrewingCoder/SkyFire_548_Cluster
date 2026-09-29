/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_BATTLEGROUND_MATCHER_H
#define SKYFIRE_BATTLEGROUND_MATCHER_H
#include "BattlegroundProtocol.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <vector>

namespace Skyfire::BattlegroundService
{
    namespace MatchDetail
    {
        using Selection = std::vector<Group const*>;
        struct Sum { bool Possible = false; Selection Groups; };
        inline std::vector<Sum> Sums(Selection const& groups, std::uint32_t maximum)
        {
            std::vector<Sum> sums(maximum + 1); sums[0].Possible = true;
            // Descending updates keep a whole group from being selected twice.
            // First reach wins, preserving oldest-first preference on ties.
            for (auto group : groups)
            {
                auto size = std::uint32_t(group->Members.size());
                for (auto total = maximum; total >= size; --total)
                    if (!sums[total].Possible && sums[total-size].Possible)
                    {
                        sums[total].Possible = true; sums[total].Groups = sums[total-size].Groups;
                        sums[total].Groups.push_back(group);
                    }
            }
            return sums;
        }
        inline std::uint32_t Count(Selection const& groups)
        { std::uint32_t count = 0; for (auto group : groups) count += std::uint32_t(group->Members.size()); return count; }
        inline Selection Greedy(Selection const& groups, std::uint32_t maximum, std::uint32_t goal)
        {
            Selection selected; std::uint32_t count = 0;
            for (auto group : groups)
            {
                if (count >= goal) break;
                if (count + group->Members.size() <= maximum) { selected.push_back(group); count += std::uint32_t(group->Members.size()); }
            }
            return selected;
        }
    }

    inline bool PlanMatches(Snapshot const& snapshot, Response& response)
    {
        response = {}; response.Sequence = snapshot.Sequence;
        if (!ValidSnapshot(snapshot)) return false;
        using namespace MatchDetail;
        std::array<Selection,2> normal, premade;
        for (auto const& group : snapshot.Groups)
        {
            bool reserved = !snapshot.Testing && group.Premade && group.WaitMs < snapshot.PremadeWaitMs && group.Members.size() >= snapshot.Min;
            (reserved ? premade : normal)[group.Team].push_back(&group);
        }
        auto order = [](Group const* left, Group const* right)
        { return left->WaitMs != right->WaitMs ? left->WaitMs > right->WaitMs : left->Id < right->Id; };
        for (auto& groups : normal) std::sort(groups.begin(),groups.end(),order);
        for (auto& groups : premade) std::sort(groups.begin(),groups.end(),order);
        std::set<std::uint64_t> used;
        auto publish = [&](std::uint32_t instance, std::array<Selection,2> const& selected)
        {
            Plan plan; plan.Instance = instance;
            for (unsigned team=0; team<2; ++team)
                for (auto group : selected[team]) { plan.Teams[team].push_back(group->Id); used.insert(group->Id); }
            if (plan.Teams[0].empty() && plan.Teams[1].empty()) return;
            response.Plans.push_back(std::move(plan));
            for (auto& groups : normal)
                groups.erase(std::remove_if(groups.begin(),groups.end(),[&](auto group) { return used.count(group->Id); }),groups.end());
            for (auto& groups : premade)
                groups.erase(std::remove_if(groups.begin(),groups.end(),[&](auto group) { return used.count(group->Id); }),groups.end());
        };
        // The world's advertised free slots already include its live team-
        // balance rules and invitations. Reserved premades never backfill.
        auto running = snapshot.Running;
        std::sort(running.begin(),running.end(),[](auto const& a,auto const& b){return a.Instance<b.Instance;});
        for (auto const& match : running)
        {
            if (response.Plans.size() >= 32) break;
            if (!snapshot.InvitationType || snapshot.Testing)
                publish(match.Instance,{Greedy(normal[0],match.Free[0],match.Free[0]),Greedy(normal[1],match.Free[1],match.Free[1])});
            else
            {
                auto a=Sums(normal[0],match.Free[0]), b=Sums(normal[1],match.Free[1]);
                std::uint32_t left=0,right=0,difference=std::max(match.Free[0],match.Free[1]);
                for(std::uint32_t i=0;i<=match.Free[0];++i)
                    for(std::uint32_t j=0;j<=match.Free[1];++j)
                    {
                        auto freeA=match.Free[0]-i,freeB=match.Free[1]-j;
                        auto delta=freeA>freeB?freeA-freeB:freeB-freeA;
                        if(a[i].Possible && b[j].Possible && (delta<difference || (delta==difference && i+j>left+right)))
                        { left=i;right=j;difference=delta; }
                    }
                publish(match.Instance,{std::move(a[left].Groups),std::move(b[right].Groups)});
            }
        }
        while (response.Plans.size() < 32 && !premade[0].empty() && !premade[1].empty())
        {
            std::array<Selection,2> selected{{{premade[0].front()},{premade[1].front()}}};
            auto target = std::max(Count(selected[0]),Count(selected[1]));
            for (unsigned team=0; team<2; ++team)
            {
                auto additions = Greedy(normal[team],target-Count(selected[team]),target-Count(selected[team]));
                selected[team].insert(selected[team].end(),additions.begin(),additions.end());
            }
            // Preserve complete premades; balance is enforced when requested.
            auto a=Count(selected[0]), b=Count(selected[1]);
            if (snapshot.InvitationType && (a>b?a-b:b-a)>2) break;
            publish(0,selected);
        }
        while (response.Plans.size() < 32 && (!normal[0].empty() || !normal[1].empty()))
        {
            std::array<Selection,2> selected;
            if (!snapshot.InvitationType || snapshot.Testing)
            {
                auto goal = snapshot.Testing ? 1u : snapshot.Min;
                for (unsigned team=0; team<2; ++team) selected[team]=Greedy(normal[team],snapshot.Max,goal);
                if (!snapshot.Testing && (Count(selected[0])<snapshot.Min || Count(selected[1])<snapshot.Min)) break;
            }
            else
            {
                auto a=Sums(normal[0],snapshot.Max), b=Sums(normal[1],snapshot.Max);
                std::uint32_t left=0,right=0;
                // Smallest viable match preserves the legacy minimum-start
                // policy; whole-group DP avoids stranding a feasible balance.
                for (auto i=snapshot.Min; i<=snapshot.Max; ++i)
                    for (auto j=snapshot.Min; j<=snapshot.Max; ++j)
                        if (a[i].Possible && b[j].Possible && (i>j?i-j:j-i)<=2 &&
                            (!left || i+j<left+right || (i+j==left+right && (i>j?i-j:j-i)<(left>right?left-right:right-left))))
                        { left=i;right=j; }
                if (!left) break;
                selected={std::move(a[left].Groups),std::move(b[right].Groups)};
            }
            if (selected[0].empty() && selected[1].empty()) break;
            publish(0,selected);
        }
        return true;
    }
}
#endif
