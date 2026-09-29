/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "Cluster/BattlegroundQueueBook.h"
#include <iostream>
#include <random>
#include <stdexcept>

using namespace Skyfire::BattlegroundService;
static void Check(bool value, char const* message)
{ if(!value) throw std::runtime_error(message); }
static Snapshot Base()
{
    Snapshot s; s.Realm=1;s.Generation=std::string(64,'a');s.Sequence=1;s.Type=2;
    s.Min=5;s.Max=10;s.InvitationType=1;s.PremadeWaitMs=60000;return s;
}
static Group Make(std::uint64_t id, unsigned team, unsigned size, bool premade=false, std::uint32_t wait=1000)
{
    Group g;g.Id=id;g.Team=std::uint8_t(team);g.Premade=premade;g.WaitMs=wait;
    for(unsigned i=0;i<size;++i)g.Members.push_back(id*100+i+1);return g;
}
static void Invariants(Snapshot const& s, Response const& response)
{
    Check(response.Sequence==s.Sequence && response.Plans.size()<=32,"Response bounds");
    std::set<std::uint64_t> used;
    for(auto const& plan:response.Plans)
    {
        std::array<unsigned,2> counts{};
        for(unsigned team=0;team<2;++team)
            for(auto id:plan.Teams[team])
            {
                auto group=std::find_if(s.Groups.begin(),s.Groups.end(),[&](auto const& g){return g.Id==id;});
                Check(group!=s.Groups.end() && group->Team==team && used.insert(id).second,"Group split, invented or reused");
                counts[team]+=unsigned(group->Members.size());
            }
        Check(counts[0]||counts[1],"Empty plan");
        Check(counts[0]<=s.Max && counts[1]<=s.Max,"Capacity exceeded");
        if(plan.Instance)
        {
            auto match=std::find_if(s.Running.begin(),s.Running.end(),[&](auto const& m){return m.Instance==plan.Instance;});
            Check(match!=s.Running.end() && counts[0]<=match->Free[0] && counts[1]<=match->Free[1],"Running capacity exceeded");
        }
        else if(!s.Testing)
        {
            Check(counts[0]>=s.Min && counts[1]>=s.Min,"New match below minimum");
            if(s.InvitationType)Check((counts[0]>counts[1]?counts[0]-counts[1]:counts[1]-counts[0])<=2,"Unbalanced match");
        }
    }
}
int main()
{
    try
    {
        Response result;auto s=Base();
        s.Groups={Make(1,0,5),Make(2,1,5)};
        Check(PlanMatches(s,result) && result.Plans.size()==1,"Basic match");Invariants(s,result);
        auto original=EncodeResponse(result).Bytes;
        std::reverse(s.Groups.begin(),s.Groups.end());
        Check(PlanMatches(s,result) && EncodeResponse(result).Bytes==original,"Order independence");
        s.Groups={Make(1,0,8),Make(2,1,5)};
        Check(PlanMatches(s,result) && result.Plans.empty(),"Balance enforced");
        s.InvitationType=0;Check(PlanMatches(s,result) && result.Plans.size()==1,"Legacy unbalanced policy");Invariants(s,result);
        s=Base();s.Groups={Make(1,0,8),Make(2,0,5),Make(3,1,5)};
        Check(PlanMatches(s,result) && result.Plans.size()==1 && result.Plans[0].Teams[0]==std::vector<std::uint64_t>{2},"Feasible whole-group balance");
        s=Base();s.Groups={Make(1,0,5,true),Make(2,1,5)};
        Check(PlanMatches(s,result) && result.Plans.empty(),"Premade protected");
        s.Groups[0].WaitMs=s.PremadeWaitMs;
        Check(PlanMatches(s,result) && result.Plans.size()==1,"Premade timeout fallback");
        s=Base();s.Groups={Make(1,0,5,true),Make(2,1,7,true),Make(3,0,2)};
        Check(PlanMatches(s,result) && result.Plans.size()==1 && result.Plans[0].Teams[0].size()==2,"Paired premades fill smaller faction");Invariants(s,result);
        s=Base();s.InvitationType=0;s.Groups={Make(1,0,4,false,2000),Make(2,0,2),Make(3,1,5,true)};s.Running={{42,{3,10}}};
        Check(PlanMatches(s,result) && result.Plans.size()==1 && result.Plans[0].Instance==42 && result.Plans[0].Teams[0]==std::vector<std::uint64_t>{2} && result.Plans[0].Teams[1].empty(),"Backfill respects whole groups and reserved premades");
        s=Base();s.Groups={Make(1,0,4),Make(2,1,2),Make(3,0,2)};s.Running={{42,{5,5}}};
        Check(PlanMatches(s,result) && result.Plans.size()==1 && result.Plans[0].Teams[0]==std::vector<std::uint64_t>{3},"Backfill balances residual slots without splitting groups");
        s=Base();s.Testing=true;s.Groups={Make(1,0,1)};
        Check(PlanMatches(s,result) && result.Plans.size()==1,"One-sided debug match");
        s.Groups.push_back(Make(2,1,1));s.Groups[1].Members=s.Groups[0].Members;
        Check(!PlanMatches(s,result),"Duplicate player rejected");
        s.Groups={Make(1,0,1),Make(1,1,1)};Check(!PlanMatches(s,result),"Duplicate group rejected");
        s=Base();s.Groups={Make(1,0,11)};Check(!PlanMatches(s,result),"Oversized group rejected");
        s=Base();s.Min=s.Max=1;
        for(unsigned i=1;i<=100;++i)s.Groups.push_back(Make(i,i%2,1));
        Check(PlanMatches(s,result) && result.Plans.size()==32,"Plan batch bounded");Invariants(s,result);

        QueueBook book;s=Base();s.Groups={Make(1,0,5),Make(2,1,5)};
        Check(book.Accept("world-a",s,100,result),"Initial owner");
        Check(book.QueuedGroups()==2 && book.QueuedPlayers()==10,"Queue metrics");
        Check(!book.Accept("world-a",s,101,result),"Replay rejected");
        auto successor=s;successor.Generation=std::string(64,'b');
        Check(!book.Accept("world-b",successor,15099,result),"Live owner cannot be replaced");
        Check(book.Accept("world-b",successor,15100,result),"Expired owner replaced");
        s.Sequence=2;Check(!book.Accept("world-a",s,40000,result),"Retired owner fenced");
        book.Expire(35100);Check(!book.QueuedPlayers(),"Idle snapshot expires");
        Check(!book.Accept("world-b",successor,35101,result),"Expiration preserves replay watermark");
        successor.Sequence=2;Check(book.Accept("world-b",successor,35102,result),"Current owner resumes");
        book.ClearQueues();Check(!book.QueuedGroups() && !book.Accept("world-b",successor,35103,result),"Hub loss preserves replay fence");
        successor.Sequence=3;Check(!book.Accept("world-c",successor,60000,result),"Generation bound to node");
        successor.Realm=2;Check(book.Accept("world-c",successor,60000,result),"Realm isolation");

        std::mt19937 random(548);
        for(unsigned iteration=0;iteration<500;++iteration)
        {
            s=Base();s.Sequence=iteration+1;s.InvitationType=std::uint8_t(random()%2);s.Testing=random()%10==0;
            for(unsigned i=1;i<=30;++i)s.Groups.push_back(Make(i,random()%2,1+random()%10,random()%3==0,random()%120000));
            s.Running={{100,{std::uint32_t(random()%11),std::uint32_t(random()%11)}},{200,{std::uint32_t(random()%11),std::uint32_t(random()%11)}}};
            Check(PlanMatches(s,result),"Random valid snapshot rejected");Invariants(s,result);
            Snapshot decoded;Check(DecodeSnapshot(EncodeSnapshot(s).Bytes,decoded),"Snapshot roundtrip");
            Response decodedResponse;Check(DecodeResponse(EncodeResponse(result).Bytes,decodedResponse),"Response roundtrip");
        }
        std::cout<<"Battleground matcher and ownership checks passed\n";return 0;
    }
    catch(std::exception const& error){std::cerr<<error.what()<<'\n';return 1;}
}
