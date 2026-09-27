/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_CHANNEL_STATE_H
#define SKYFIRE_CHANNEL_STATE_H
#include <cstddef>
#include <cstdint>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace Skyfire::Chat
{
    // Policy has no Player, WorldSession, SQL or client-opcode dependency. The
    // adapter authenticates the actor's realm/session and attests gameplay RBAC.
    // Evaluate on a copy; publish that copy and its notices only after durable
    // changes have an acknowledged commit. Never publish an uncertain mutation.
    struct ChannelState
    {
        enum class Action { Join, Leave, Kick, Ban, Unban, Password, Announce, Moderator, Mute, Owner, QueryOwner, List, Invite, Speak };
        enum class Error { None, NotMember, AlreadyMember, Banned, WrongPassword, NotModerator, NotOwner, NotFound, WrongFaction, Muted, Full, Invalid };
        enum Flag : std::uint8_t { OwnerFlag = 1, ModeratorFlag = 2, MutedFlag = 8 };
        struct Member
        {
            std::uint64_t Incarnation = 0;
            std::uint32_t Team = 0;
            std::uint8_t Flags = 0;
            bool CrossFaction = false;
        };
        struct Command
        {
            Action Type = Action::Join;
            std::uint64_t Actor = 0, Incarnation = 0, Target = 0;
            std::uint32_t Team = 0, TargetTeam = 0;
            bool Override = false, Silent = false, CrossFaction = false, TargetCrossFaction = false;
            bool Value = false, PasswordMatches = false;
            std::string PasswordVerifier;
        };
        struct Notice
        {
            // Values match the existing client channel-notification meanings.
            std::uint8_t Type;
            std::uint64_t Recipient, Actor, Target;
            std::uint8_t OldFlags = 0, NewFlags = 0;
        };
        struct Result
        {
            Error Status = Error::None;
            bool Durable = false;
            std::vector<Notice> Notices;
            std::vector<std::uint64_t> Recipients;
        };
        std::map<std::uint64_t, Member> Members;
        std::set<std::uint64_t> Bans;
        std::uint64_t Owner = 0;
        bool Constant = false, Announce = true, Ownership = true;
        std::string PasswordVerifier;
        static constexpr std::size_t MaximumMembers = 4096;

        Result Apply(Command const& command)
        {
            Result result;
            auto fail = [&](Error error) { result.Status = error; return result; };
            if (!command.Actor || !command.Incarnation) return fail(Error::Invalid);
            auto actor = Members.find(command.Actor);
            bool const member = actor != Members.end() && actor->second.Incarnation == command.Incarnation;
            auto notice = [&](std::uint8_t type, std::uint64_t recipient, std::uint64_t target = 0,
                              std::uint8_t oldFlags = 0, std::uint8_t newFlags = 0)
            { result.Notices.push_back({type, recipient, command.Actor, target, oldFlags, newFlags}); };
            auto broadcast = [&](std::uint8_t type, std::uint64_t target = 0)
            { for (auto const& item : Members) notice(type, item.first, target); };
            auto setOwner = [&](std::uint64_t guid, bool announce)
            {
                auto previous = Members.find(Owner);
                if (previous != Members.end()) previous->second.Flags &= ~OwnerFlag;
                Owner = guid;
                auto next = Members.find(guid);
                if (next != Members.end())
                {
                    auto old = next->second.Flags;
                    next->second.Flags |= OwnerFlag | ModeratorFlag;
                    for (auto const& item : Members) notice(0x0c, item.first, guid, old, next->second.Flags);
                    if (announce) broadcast(0x08, guid);
                }
            };
            auto remove = [&](std::uint64_t guid, std::uint64_t preferred)
            {
                bool const wasOwner = guid == Owner;
                Members.erase(guid);
                if (wasOwner)
                {
                    Owner = 0;
                    if (Ownership && !Members.empty())
                        setOwner(Members.count(preferred) ? preferred : Members.begin()->first, true);
                }
            };
            if (command.Type == Action::Join)
            {
                if (actor != Members.end()) return fail(member ? Error::AlreadyMember : Error::Invalid);
                if (Bans.count(command.Actor)) return fail(Error::Banned);
                if (!PasswordVerifier.empty() && !command.PasswordMatches) return fail(Error::WrongPassword);
                if (Members.size() >= MaximumMembers) return fail(Error::Full);
                if (Announce && !command.Silent) broadcast(0x00, command.Actor);
                Members.emplace(command.Actor, Member{command.Incarnation, command.Team, 0, command.CrossFaction});
                notice(0x02, command.Actor);
                if (!Constant && Ownership && !Owner) setOwner(command.Actor, Members.size() > 1);
                return result;
            }
            if (!member) return fail(Error::NotMember);
            if (command.Type == Action::Leave)
            {
                notice(0x03, command.Actor);
                // Departure notice goes to remaining members only.
                bool announce = Announce && !command.Silent;
                remove(command.Actor, 0);
                if (announce) broadcast(0x01, command.Actor);
                return result;
            }
            if (command.Type == Action::QueryOwner) { notice(0x0b, command.Actor, Owner); return result; }
            if (command.Type == Action::List)
            {
                // The world adapter must still apply global visibility and GM-level filters.
                for (auto const& item : Members) result.Recipients.push_back(item.first);
                return result;
            }
            if (command.Type == Action::Speak)
            {
                if (actor->second.Flags & MutedFlag) return fail(Error::Muted);
                for (auto const& item : Members) result.Recipients.push_back(item.first);
                return result;
            }
            if (command.Type == Action::Invite)
            {
                if (!command.Target) return fail(Error::NotFound);
                if (Bans.count(command.Target)) return fail(Error::Banned);
                if (Members.count(command.Target)) return fail(Error::AlreadyMember);
                if (command.Team != command.TargetTeam && !(command.CrossFaction && command.TargetCrossFaction))
                    return fail(Error::WrongFaction);
                // The adapter also enforces target ignore/privacy before delivery.
                notice(0x18, command.Target); notice(0x1d, command.Actor, command.Target);
                return result;
            }
            if (command.Type == Action::Owner)
            {
                if (Owner != command.Actor && !command.Override) return fail(Error::NotOwner);
            }
            else if (!(actor->second.Flags & ModeratorFlag) && !command.Override) return fail(Error::NotModerator);
            if (command.Type == Action::Password)
            {
                // Never accept plaintext here. The service's bounded crypto worker
                // produces this verifier; the wire adapter must not trust a client value.
                static std::regex const verifier(R"(pbkdf2-sha256\$[1-9][0-9]{4,6}\$[0-9a-f]{32}\$[0-9a-f]{64})");
                if (!command.PasswordVerifier.empty() && !std::regex_match(command.PasswordVerifier, verifier))
                    return fail(Error::Invalid);
                PasswordVerifier = command.PasswordVerifier; result.Durable = true; broadcast(0x07); return result;
            }
            if (command.Type == Action::Announce)
            {
                Announce = !Announce; result.Durable = true; broadcast(Announce ? 0x0d : 0x0e); return result;
            }
            if (command.Type == Action::Unban)
            {
                if (!command.Target || !Bans.count(command.Target)) return fail(Error::NotFound);
                Bans.erase(command.Target); result.Durable = true; broadcast(0x15, command.Target); return result;
            }
            auto target = Members.find(command.Target);
            if (!command.Target || target == Members.end()) return fail(Error::NotFound);
            if (command.Type == Action::Kick || command.Type == Action::Ban)
            {
                if (command.Target == Owner && command.Actor != Owner && !command.Override) return fail(Error::NotOwner);
                bool const ban = command.Type == Action::Ban && !Bans.count(command.Target);
                if (ban && Bans.size() >= MaximumMembers) return fail(Error::Full);
                if (ban) { Bans.insert(command.Target); result.Durable = true; }
                if (!command.Silent) broadcast(ban ? 0x14 : 0x12, command.Target);
                remove(command.Target, command.Actor);
                return result;
            }
            if (command.Type != Action::Moderator && command.Type != Action::Mute && command.Type != Action::Owner)
                return fail(Error::Invalid);
            if (actor->second.Team != target->second.Team && !(command.CrossFaction && target->second.CrossFaction))
                return fail(Error::NotFound);
            if (command.Type == Action::Owner) { setOwner(command.Target, true); return result; }
            if (command.Target == Owner && command.Actor != Owner) return fail(Error::NotOwner);
            if (command.Type == Action::Moderator && command.Target == Owner) return result;
            auto old = target->second.Flags;
            auto flag = command.Type == Action::Moderator ? ModeratorFlag : MutedFlag;
            if (command.Value) target->second.Flags |= flag; else target->second.Flags &= ~flag;
            if (old != target->second.Flags)
                for (auto const& item : Members) notice(0x0c, item.first, command.Target, old, target->second.Flags);
            return result;
        }
    };
}
#endif
