/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "ChannelState.h"
#include "Cluster/ChatChannels.h"
#include <stdexcept>
#include <utility>

int main()
{
    auto check = [](bool passed) { if (!passed) throw std::runtime_error("Channel policy invariant failed"); };
    {
        Skyfire::Chat::ChannelCommand request;
        request.Name = "General"; request.Password = "test";
        request.Team = 469; request.ActorTeam = 469;
        auto bytes = Skyfire::Chat::EncodeChannelCommand(request).Bytes;
        Skyfire::Chat::ChannelCommand decoded;
        check(Skyfire::Chat::DecodeChannelCommand(bytes, decoded));
        check(decoded.Name == request.Name && decoded.Password == request.Password);
        bytes.push_back(0);
        check(!Skyfire::Chat::DecodeChannelCommand(bytes, decoded));
        request.Name.assign(128, 'x');
        check(!Skyfire::Chat::DecodeChannelCommand(Skyfire::Chat::EncodeChannelCommand(request).Bytes, decoded));
        Skyfire::Chat::ChannelUpdate update, result;
        update.Name = "General"; update.Revision = 1; update.Members.push_back({1, 7, 3});
        update.Notices.push_back({2, 0, 0, 1, 0});
        check(Skyfire::Chat::DecodeChannelUpdate(Skyfire::Chat::EncodeChannelUpdate(update).Bytes, result));
        check(result.Members.size() == 1 && result.Members[0].Incarnation == 7);
        update.Members.push_back({1, 8, 0});
        check(!Skyfire::Chat::DecodeChannelUpdate(Skyfire::Chat::EncodeChannelUpdate(update).Bytes, result));
    }

    using State = Skyfire::Chat::ChannelState;
    using Action = State::Action;
    using Error = State::Error;
    auto command = [](Action action, std::uint64_t actor = 1, std::uint64_t target = 0)
    {
        State::Command result;
        result.Type = action; result.Actor = actor; result.Incarnation = actor + 100;
        result.Target = target; result.Team = 469; result.TargetTeam = 469;
        return result;
    };
    State channel;
    auto firstJoin = channel.Apply(command(Action::Join));
    check(firstJoin.Status == Error::None && firstJoin.Notices.size() == 1);
    check(firstJoin.Notices.front().Type == 0x02); // No spurious first-owner mode notice.
    check(channel.Owner == 1 && (channel.Members.at(1).Flags & State::ModeratorFlag));
    check(channel.Apply(command(Action::Join, 2)).Status == Error::None);
    check(channel.Apply(command(Action::Ban, 2, 1)).Status == Error::NotModerator);
    auto moderator = command(Action::Moderator, 1, 2); moderator.Value = true;
    check(channel.Apply(moderator).Status == Error::None);
    check(channel.Apply(command(Action::Kick, 2, 1)).Status == Error::NotOwner);
    check(channel.Apply(command(Action::Owner, 2, 2)).Status == Error::NotOwner);
    auto mute = command(Action::Mute, 1, 2); mute.Value = true;
    check(channel.Apply(mute).Status == Error::None);
    check(channel.Apply(command(Action::Speak, 2)).Status == Error::Muted);
    check(channel.Apply(command(Action::Speak)).Recipients.size() == 2);
    auto stale = command(Action::Announce); ++stale.Incarnation;
    check(channel.Apply(stale).Status == Error::NotMember && channel.Announce);
    auto tentative = channel;
    check(tentative.Apply(command(Action::Ban, 1, 2)).Durable);
    // An unacknowledged candidate cannot change the published channel.
    check(channel.Members.count(2) == 1 && channel.Bans.empty());
    channel = std::move(tentative);
    check(!channel.Members.count(2) && channel.Bans.count(2));
    check(channel.Apply(command(Action::Join, 2)).Status == Error::Banned);
    check(channel.Apply(command(Action::Unban, 1, 2)).Durable);
    auto password = command(Action::Password);
    password.PasswordVerifier = "plaintext";
    check(channel.Apply(password).Status == Error::Invalid && channel.PasswordVerifier.empty());
    password.PasswordVerifier = "pbkdf2-sha256$210000$" + std::string(32, 'a') + "$" + std::string(64, 'b');
    check(channel.Apply(password).Durable);
    check(channel.Apply(command(Action::Join, 2)).Status == Error::WrongPassword);
    auto join = command(Action::Join, 2); join.PasswordMatches = true;
    check(channel.Apply(join).Status == Error::None);
    auto departed = channel.Apply(command(Action::Leave));
    check(departed.Status == Error::None && channel.Owner == 2);
    check(departed.Notices.size() >= 3 && departed.Notices[0].Type == 0x03 && departed.Notices[1].Type == 0x01);
    // Other members see the departure before the replacement-owner notices.
    check(channel.Apply(command(Action::Kick, 2, 2)).Status == Error::None);
    check(channel.Owner == 0 && channel.Members.empty());
    State otherRealm;
    check(otherRealm.Members.empty() && otherRealm.Bans.empty() && otherRealm.PasswordVerifier.empty());
    State constant; constant.Constant = true; constant.Ownership = false; constant.Announce = false;
    check(constant.Apply(command(Action::Join)).Status == Error::None && !constant.Owner);
    check(constant.Apply(command(Action::Announce)).Status == Error::NotModerator);
    auto invite = command(Action::Invite, 1, 3); invite.TargetTeam = 67;
    check(constant.Apply(invite).Status == Error::WrongFaction);
    invite.CrossFaction = invite.TargetCrossFaction = true;
    check(constant.Apply(invite).Status == Error::None);
    auto overrideCommand = command(Action::Announce); overrideCommand.Override = true;
    check(constant.Apply(overrideCommand).Durable);
    State limited;
    for (std::uint64_t id = 1; id <= State::MaximumMembers; ++id)
        limited.Members.emplace(id, State::Member{id + 100, 469, 0, false});
    check(limited.Apply(command(Action::Join, State::MaximumMembers + 1)).Status == Error::Full);
    return 0;
}
