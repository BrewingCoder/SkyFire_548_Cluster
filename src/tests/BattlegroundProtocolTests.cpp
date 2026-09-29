/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "Cluster/BattlegroundProtocol.h"
#include <stdexcept>
int main()
{
    using namespace Skyfire::BattlegroundService;
    auto check=[](bool value){if(!value) throw std::runtime_error("Battleground protocol invariant failed");};
    Snapshot original; original.Realm=1;original.Generation=std::string(64,'a');original.Sequence=1;
    original.Type=2;original.Min=2;original.Max=10;original.Groups={{10,0,false,100,{101,102}},{20,1,false,200,{201,202}}};
    auto encoded=EncodeSnapshot(original);Snapshot parsed;
    check(DecodeSnapshot(encoded.Bytes,parsed)&&parsed.Groups.size()==2&&parsed.Groups[0].Members[1]==102);
    for(std::size_t i=0;i<encoded.Bytes.size();++i) check(!DecodeSnapshot({encoded.Bytes.begin(),encoded.Bytes.begin()+i},parsed));
    auto invalid=encoded.Bytes;invalid.push_back(0);check(!DecodeSnapshot(invalid,parsed));
    original.Groups[1].Members[0]=101;check(!ValidSnapshot(original));original.Groups[1].Members[0]=201;
    original.Generation[0]='A';check(!ValidSnapshot(original));original.Generation[0]='a';
    original.InvitationType=2;check(!ValidSnapshot(original));original.InvitationType=1;
    original.Running={{1,{5,5}},{1,{5,5}}};check(!ValidSnapshot(original));original.Running.clear();
    Response response;response.Sequence=1;response.Plans.push_back({0,{{{10},{20}}}});Response result;
    check(DecodeResponse(EncodeResponse(response).Bytes,result)&&result.Plans[0].Teams[1][0]==20);
    response.Plans[0].Teams[1].push_back(10);check(!DecodeResponse(EncodeResponse(response).Bytes,result));
    Skyfire::Cluster::BattlegroundMetrics metrics;metrics.Realms={1,2};metrics.QueuedPlayers=4;
    Skyfire::Cluster::BattlegroundMetrics decoded;
    check(DecodeMetrics(EncodeMetrics(metrics).Bytes,decoded)&&decoded.QueuedPlayers==4&&decoded.Realms.size()==2);
    metrics.Realms={1,1};check(!DecodeMetrics(EncodeMetrics(metrics).Bytes,decoded));
    return 0;
}
