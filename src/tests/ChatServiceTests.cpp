/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "Cluster/ChatService.h"
#include "Cluster/ChatRemoteWhisper.h"
#include "Cluster/ChatGroupRelay.h"
#include "Cluster/GuildWire.h"
#include <stdexcept>
int main()
{
    using namespace Skyfire::Chat;
    auto check = [](bool value) { if (!value) throw std::runtime_error("Chat service transport invariant failed"); };
    ServiceRequest request; request.Generation = std::string(64, 'a'); request.Sequence = 1;
    request.Account = 10; request.Actor = 20; request.Incarnation = 30; request.Payload = {0, 255, 1};
    auto encoded = EncodeServiceRequest(request); ServiceRequest decoded;
    check(DecodeServiceRequest(encoded.Bytes, decoded) && decoded.Payload == request.Payload);
    for (std::size_t i = 0; i < encoded.Bytes.size(); ++i)
        check(!DecodeServiceRequest({encoded.Bytes.begin(), encoded.Bytes.begin() + i}, decoded));
    auto invalid = encoded.Bytes; invalid.push_back(0); check(!DecodeServiceRequest(invalid, decoded));
    invalid = encoded.Bytes; invalid[0] = 0; check(!DecodeServiceRequest(invalid, decoded));
    request.Generation[0] = 'X'; check(!DecodeServiceRequest(EncodeServiceRequest(request).Bytes, decoded));
    request.Generation[0] = 'a'; request.Payload.resize(MaxServicePayload + 1);
    check(!DecodeServiceRequest(EncodeServiceRequest(request).Bytes, decoded));
    ServiceResponse response{1, ServiceStatus::Unknown, {0, 1}}, result;
    check(DecodeServiceResponse(EncodeServiceResponse(response).Bytes, result) && result.Status == ServiceStatus::Unknown);
    response.Status = ServiceStatus(4); check(!DecodeServiceResponse(EncodeServiceResponse(response).Bytes, result));
    RemoteWhisper whisper; whisper.Sender = 20; whisper.Incarnation = 30; whisper.Level = 5;
    whisper.SenderName = "First"; whisper.ReceiverName = "Second"; whisper.Text = "hello";
    RemoteWhisper parsed;
    check(DecodeRemoteWhisper(EncodeRemoteWhisper(whisper).Bytes, parsed));
    whisper.Flags = 16; check(!DecodeRemoteWhisper(EncodeRemoteWhisper(whisper).Bytes, parsed));
    whisper.Prefix = "addon"; check(DecodeRemoteWhisper(EncodeRemoteWhisper(whisper).Bytes, parsed));
    whisper.Flags = 128; check(!DecodeRemoteWhisper(EncodeRemoteWhisper(whisper).Bytes, parsed));
    RemoteWhisperReply acknowledgment; acknowledgment.Token = 1; acknowledgment.Receiver = 21; acknowledgment.Name = "Second";
    acknowledgment.Flags = 15; acknowledgment.ChatTag = 3;
    RemoteWhisperReply acknowledgmentDecoded;
    check(DecodeRemoteWhisperReply(EncodeRemoteWhisperReply(acknowledgment).Bytes, acknowledgmentDecoded) && acknowledgmentDecoded.ChatTag == 3);
    acknowledgment.Flags = 16; check(!DecodeRemoteWhisperReply(EncodeRemoteWhisperReply(acknowledgment).Bytes, acknowledgmentDecoded));
    GroupRelay group; group.Group = 100; group.Sender = 20; group.Incarnation = 30; group.Name = "First";
    group.Text = "hello"; group.Members = {20, 21}; GroupRelay projected;
    check(DecodeGroupRelay(EncodeGroupRelay(group).Bytes, projected));
    group.Members = {21}; check(!DecodeGroupRelay(EncodeGroupRelay(group).Bytes, projected));
    group.Members = {20, 20}; check(!DecodeGroupRelay(EncodeGroupRelay(group).Bytes, projected));
    group.Members = {20}; group.Kind = AudienceKind::Guild; check(!DecodeGroupRelay(EncodeGroupRelay(group).Bytes, projected));
    group.Kind = AudienceKind::Party; group.Language = 0xffffffffu;
    check(!DecodeGroupRelay(EncodeGroupRelay(group).Bytes, projected));
    group.Prefix = "test"; check(DecodeGroupRelay(EncodeGroupRelay(group).Bytes, projected));
    group.Language = 0; check(!DecodeGroupRelay(EncodeGroupRelay(group).Bytes, projected));
    GuildRequest guildRequest; guildRequest.Guild = 77;
    GuildRequest guildDecoded;
    check(DecodeGuildRequest(EncodeGuildRequest(guildRequest), guildDecoded) && guildDecoded.Command.Text.empty());
    guildRequest.Admin = GuildAdmin::Rename; guildRequest.Permission = 407; guildRequest.Console = true;
    check(DecodeGuildRequest(EncodeGuildRequest(guildRequest), guildDecoded) && guildDecoded.Console);
    guildRequest.Permission = 406; check(!DecodeGuildRequest(EncodeGuildRequest(guildRequest), guildDecoded));
    ServiceRequest console; console.Domain = ServiceDomain::Guild; console.Generation = std::string(64, 'a'); console.Sequence = 1;
    check(DecodeServiceRequest(EncodeServiceRequest(console).Bytes, decoded));
    console.Domain = ServiceDomain::Channel; check(!DecodeServiceRequest(EncodeServiceRequest(console).Bytes, decoded));
    console.Domain = ServiceDomain::Guild; console.Account = 1; check(!DecodeServiceRequest(EncodeServiceRequest(console).Bytes, decoded));
    PresenceDirectory presence;
    PresenceSnapshot first{std::string(64, 'a'), 1, {{10, 20, 30, "First"}}};
    PresenceSnapshot second{std::string(64, 'b'), 1, {{11, 21, 31, "Second"}}};
    check(presence.Replace(1, "world-a", first, 0)); check(presence.Replace(1, "world-b", second, 0));
    check(presence.Replace(2, "world-b", first, 0));
    std::string node, generation;
    check(presence.Locate(1, 21, 31, 1, node, generation) && node == "world-b" && generation == second.Generation);
    check(!presence.Locate(2, 21, 31, 1, node, generation));
    check(!presence.Locate(1, 21, 32, 1, node, generation));
    check(!presence.Locate(1, 21, 31, PresenceLeaseMs, node, generation));
    check(presence.FindByName(1, "Second", 1) && !presence.FindByName(2, "Second", 1));
    check(presence.FindAny(1, 21, 31, 1) && !presence.FindAny(1, 21, 32, 1));
}
