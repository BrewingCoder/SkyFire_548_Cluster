/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "ChannelState.h"
#include <stdexcept>
#include <utility>

int main()
{
    using State = Skyfire::Chat::ChannelState;
    using Action = State::Action;
    using Error = State::Error;
    auto check = [](bool passed) { if (!passed) throw std::runtime_error("Channel policy invariant failed"); };
    auto command = [](Action action, std::uint64_t actor = 1, std::uint64_t target = 0)
    {
        State::Command result;
        result.Type = action; result.Actor = actor; result.Incarnation = actor + 100;
        result.Target = target; result.Team = 469; result.TargetTeam = 469;
        return result;
    };
    State channel;
    check(channel.Apply(command(Action::Join)).Status == Error::None);
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
    check(channel.Apply(command(Action::Leave)).Status == Error::None && channel.Owner == 2);
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
