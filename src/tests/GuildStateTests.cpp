/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "GuildState.h"
#include "Cluster/GuildWire.h"
#include "Cluster/GuildEvents.h"
#include <stdexcept>

int main()
{
    using State = Skyfire::Chat::GuildState;
    using Action = State::Action;
    using Error = State::Error;
    auto check = [](bool condition) { if (!condition) throw std::runtime_error("Guild authority invariant failed"); };
    State guild;
    guild.Id = 1; guild.Leader = 10; guild.Name = "Test guild";
    guild.Ranks = {{"Leader", 0xDDFFBF}, {"Officer", 0xFFFF}, {"Veteran", 0x43}, {"Member", 0x43}, {"Initiate", 0x43}};
    guild.Members = {{10, {0, {}, {}}}, {20, {1, {}, {}}}, {30, {4, {}, {}}}, {40, {2, {}, {}}}};
    check(guild.Valid());
    auto command = [](Action action, std::uint64_t actor, std::uint64_t target = 0)
    { State::Command value; value.Type = action; value.Actor = actor; value.Target = target; return value; };
    auto motd = command(Action::Motd, 30); motd.Text = "unauthorized";
    check(guild.Apply(motd).Status == Error::Permission && guild.Motd.empty());
    motd.Actor = 20; motd.Text = "Welcome";
    auto candidate = guild;
    check(candidate.Apply(motd).Durable && candidate.Motd == "Welcome" && guild.Motd.empty());
    guild = candidate;
    auto promote = command(Action::SetRank, 20, 30); promote.RankId = 0;
    check(guild.Apply(promote).Status == Error::RankTooHigh && guild.Members.at(30).RankId == 4);
    promote.RankId = 1; check(guild.Apply(promote).Status == Error::RankTooHigh);
    promote.RankId = 2; check(guild.Apply(promote).PermissionsChanged && guild.Members.at(30).RankId == 2);
    check(guild.Apply(command(Action::Remove, 20, 10)).Status == Error::RankTooHigh);
    check(guild.Apply(command(Action::Leave, 10)).Status == Error::LeaderCannotLeave);
    auto officer = guild.Apply(command(Action::OfficerSpeak, 20));
    check(officer.Status == Error::None && officer.Recipients == std::vector<std::uint64_t>({10, 20}));
    check(guild.Apply(command(Action::OfficerSpeak, 30)).Status == Error::Permission);
    check(guild.Apply(command(Action::Accept, 50)).Status == Error::InvitationRequired);
    auto accept = command(Action::Accept, 50); accept.HasInvitation = true;
    check(guild.Apply(accept).Durable && guild.Members.at(50).RankId == 4);
    check(guild.Apply(accept).Status == Error::AlreadyMember);
    auto add = command(Action::AddRank, 10); add.Text = "Recruit";
    check(guild.Apply(add).Durable && guild.Ranks.size() == 6);
    guild.Members.at(50).RankId = 5;
    auto remove = command(Action::RemoveRank, 10); remove.RankId = 3;
    check(guild.Apply(remove).PermissionsChanged && guild.Ranks.size() == 5 && guild.Members.at(50).RankId == 4 && guild.Valid());
    check(guild.Apply(command(Action::Leader, 10, 20)).Durable && guild.Leader == 20 && guild.Valid());
    check(guild.Apply(command(Action::Disband, 10)).Status == Error::Permission);
    check(guild.Apply(command(Action::Disband, 20)).Deleted);
    Skyfire::Chat::GuildResponse projected; projected.Revision = 12; projected.State = guild;
    Skyfire::Chat::GuildResponse decoded;
    auto payload = Skyfire::Chat::EncodeGuildResponse(projected);
    check(Skyfire::Chat::DecodeGuildResponse(payload, decoded) && decoded.Revision == 12 && decoded.State.Valid());
    payload.push_back(0); check(!Skyfire::Chat::DecodeGuildResponse(payload, decoded));
    Skyfire::Chat::GuildRequest create; create.Guild = 3; create.Command.Type = Action::Create; create.Command.Text = "New guild";
    create.Creation.Members = {10, 20}; create.Creation.RankNames = {"Leader", "Officer", "Veteran", "Member", "Initiate"};
    create.Creation.Petition = 123; create.Creation.MinimumSignatures = 1;
    Skyfire::Chat::GuildRequest request;
    check(Skyfire::Chat::DecodeGuildRequest(Skyfire::Chat::EncodeGuildRequest(create), request) && request.Creation.Members.size() == 2);
    create.Creation.Members.push_back(10);
    check(!Skyfire::Chat::DecodeGuildRequest(Skyfire::Chat::EncodeGuildRequest(create), request));
    Skyfire::Chat::GuildRequest speech; speech.Guild = 1; speech.Command.Type = Action::OfficerSpeak;
    speech.Command.Text = std::string("\x01\xff", 2); speech.Language = 0xFFFFFFFFu; speech.Prefix = "Test";
    check(Skyfire::Chat::DecodeGuildRequest(Skyfire::Chat::EncodeGuildRequest(speech), request) && request.Command.Text == speech.Command.Text);
    speech.Language = 0; check(!Skyfire::Chat::DecodeGuildRequest(Skyfire::Chat::EncodeGuildRequest(speech), request));
    Skyfire::Chat::GuildMessageEvent event; event.Guild = 1; event.Sender = 10; event.SenderIncarnation = 99;
    event.SenderName = "Leader"; event.Text = "Hello";
    Skyfire::Chat::GuildMessageEvent incoming;
    check(Skyfire::Chat::DecodeGuildMessageEvent(Skyfire::Chat::EncodeGuildMessageEvent(event), incoming) && incoming.Text == "Hello");
    auto invalid = Skyfire::Chat::EncodeGuildMessageEvent(event); invalid.push_back(0);
    check(!Skyfire::Chat::DecodeGuildMessageEvent(invalid, incoming));
    Skyfire::Chat::GuildRequest edit; edit.Guild = 1; edit.Command.Type = Action::EditRank;
    edit.Command.RankId = 2; edit.Command.Text = "Member"; edit.Command.Rights = 0x43;
    edit.Bank.Money = 500; edit.Bank.Tabs[7] = {7, 100};
    check(Skyfire::Chat::DecodeGuildRequest(Skyfire::Chat::EncodeGuildRequest(edit), request) && request.Bank.Money == 500 && request.Bank.Tabs[7].Slots == 100);
    Skyfire::Chat::GuildInvitationEvent invite{1, 469, 10, 99, "Leader"}, decodedInvite;
    check(Skyfire::Chat::DecodeGuildInvitationEvent(Skyfire::Chat::EncodeGuildInvitationEvent(invite), decodedInvite) && decodedInvite.Incarnation == 99);
    auto truncatedInvite = Skyfire::Chat::EncodeGuildInvitationEvent(invite); truncatedInvite.pop_back();
    check(!Skyfire::Chat::DecodeGuildInvitationEvent(truncatedInvite, decodedInvite));
    auto tooLong = command(Action::PublicNote, 20, 30); tooLong.Text = std::string(32, 'x');
    check(guild.Apply(tooLong).Status == Error::Invalid);
    guild.Members.at(30).RankId = 0; check(!guild.Valid());
    return 0;
}
