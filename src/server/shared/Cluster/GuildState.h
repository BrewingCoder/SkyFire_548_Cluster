/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_CHAT_GUILD_STATE_H
#define SKYFIRE_CHAT_GUILD_STATE_H
#include "ClusterProtocol.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Skyfire::Chat
{
    // No SQL or player pointers: this is the social authority's candidate state.
    // Publish a successful mutation only after the matching durable CAS receipt.
    struct GuildState
    {
        enum class Action { Motd, Info, PublicNote, OfficerNote, Remove, Leave, Promote, Demote,
            SetRank, Leader, AddRank, RemoveRank, EditRank, Disband, Invite, Accept, Speak, OfficerSpeak, Roster, Create, ClaimLeader, Status };
        enum class Error { None, Invalid, NotMember, Permission, NotFound, RankTooHigh,
            RankTooLow, LeaderCannotLeave, AlreadyMember, Full, InvitationRequired };
        struct Rank { std::string Name; std::uint32_t Rights = 0x40; };
        struct Member { std::uint8_t RankId = 0; std::string PublicNote, OfficerNote; };
        struct Command
        {
            Action Type = Action::Roster;
            std::uint64_t Actor = 0, Target = 0, TargetIncarnation = 0;
            std::uint32_t RankId = 0, Rights = 0;
            std::string Text;
            // Set by the authority from its own invitation registry, never from
            // an untrusted world request. Membership uniqueness is also checked
            // transactionally by the character service.
            bool HasInvitation = false;
        };
        struct Result
        {
            Error Status = Error::None;
            bool Durable = false, Deleted = false, PermissionsChanged = false;
            std::vector<std::uint64_t> Recipients;
        };
        std::uint32_t Id = 0;
        std::uint64_t Leader = 0;
        std::string Name, Motd, Info;
        std::vector<Rank> Ranks;
        std::map<std::uint64_t, Member> Members;

        bool Valid() const
        {
            if (!Id || !Leader || !Characters(Name, 24, false) || !Characters(Motd, 128) || !Text(Info, 2047) ||
                Ranks.size() < 2 || Ranks.size() > 10 || Members.empty() || Members.size() > 4096) return false;
            for (auto const& rank : Ranks) if (!Characters(rank.Name, 20, false) || rank.Rights > 0xFFFFFF) return false;
            for (auto const& [guid, member] : Members)
                if (!guid || member.RankId >= Ranks.size() || (member.RankId == 0) != (guid == Leader) ||
                    !Characters(member.PublicNote, 31) || !Characters(member.OfficerNote, 31)) return false;
            return Members.count(Leader) != 0;
        }
        bool HasRight(std::uint64_t guid, std::uint32_t bit) const
        {
            auto member = Members.find(guid);
            return member != Members.end() && member->second.RankId < Ranks.size() &&
                (guid == Leader || (Ranks[member->second.RankId].Rights & bit) != 0);
        }
        Result Apply(Command const& command)
        {
            Result result;
            auto fail = [&](Error error) { result.Status = error; return result; };
            if (!Valid() || !command.Actor) return fail(Error::Invalid);
            auto actor = Members.find(command.Actor), target = Members.find(command.Target);
            if (command.Type == Action::Accept)
            {
                if (actor != Members.end()) return fail(Error::AlreadyMember);
                if (!command.HasInvitation) return fail(Error::InvitationRequired);
                if (Members.size() >= 4096) return fail(Error::Full);
                Members.emplace(command.Actor, Member{std::uint8_t(Ranks.size() - 1), {}, {}});
                result.Durable = result.PermissionsChanged = true;
                return result;
            }
            if (actor == Members.end()) return fail(Error::NotMember);
            auto required = [&](std::uint32_t right) { return HasRight(command.Actor, right); };
            auto leader = command.Actor == Leader;
            switch (command.Type)
            {
                case Action::Motd:
                case Action::Info:
                {
                    bool motd = command.Type == Action::Motd;
                    if (!required(motd ? 0x1000 : 0x10000)) return fail(Error::Permission);
                    if (!(motd ? Characters(command.Text, 128) : Text(command.Text, 2047))) return fail(Error::Invalid);
                    auto& value = motd ? Motd : Info;
                    result.Durable = value != command.Text; value = command.Text;
                    break;
                }
                case Action::PublicNote:
                case Action::OfficerNote:
                {
                    bool publicNote = command.Type == Action::PublicNote;
                    if (!required(publicNote ? 0x2000 : 0x8000)) return fail(Error::Permission);
                    if (target == Members.end()) return fail(Error::NotFound);
                    if (!Characters(command.Text, 31)) return fail(Error::Invalid);
                    auto& value = publicNote ? target->second.PublicNote : target->second.OfficerNote;
                    result.Durable = value != command.Text; value = command.Text;
                    break;
                }
                case Action::Remove:
                    if (!required(0x20)) return fail(Error::Permission);
                    if (target == Members.end()) return fail(Error::NotFound);
                    if (target->second.RankId <= actor->second.RankId) return fail(Error::RankTooHigh);
                    Members.erase(target); result.Durable = result.PermissionsChanged = true;
                    break;
                case Action::Leave:
                    if (leader && Members.size() > 1) return fail(Error::LeaderCannotLeave);
                    if (leader) result.Deleted = true;
                    else Members.erase(actor);
                    result.Durable = result.PermissionsChanged = true;
                    break;
                case Action::Promote:
                case Action::Demote:
                case Action::SetRank:
                {
                    if (target == Members.end()) return fail(Error::NotFound);
                    if (target == actor || target->second.RankId <= actor->second.RankId) return fail(Error::RankTooHigh);
                    auto rank = command.Type == Action::Promote ? std::uint32_t(target->second.RankId - 1) :
                        command.Type == Action::Demote ? std::uint32_t(target->second.RankId + 1) : command.RankId;
                    if (rank <= actor->second.RankId) return fail(Error::RankTooHigh);
                    if (rank >= Ranks.size()) return fail(Error::RankTooLow);
                    if (!required(rank > target->second.RankId ? 0x100 : 0x80)) return fail(Error::Permission);
                    result.Durable = result.PermissionsChanged = rank != target->second.RankId;
                    target->second.RankId = std::uint8_t(rank);
                    break;
                }
                case Action::Leader:
                    if (!leader) return fail(Error::Permission);
                    if (target == Members.end()) return fail(Error::NotFound);
                    if (target != actor)
                    {
                        actor->second.RankId = std::uint8_t(std::min<std::size_t>(4, Ranks.size() - 1));
                        if (!actor->second.RankId) return fail(Error::Invalid);
                        target->second.RankId = 0; Leader = command.Target;
                        result.Durable = result.PermissionsChanged = true;
                    }
                    break;
                case Action::ClaimLeader:
                    if (actor->second.RankId > 3 || leader) return fail(Error::Permission);
                    Members.at(Leader).RankId = std::uint8_t(Ranks.size() - 1);
                    actor->second.RankId = 0; Leader = command.Actor;
                    result.Durable = result.PermissionsChanged = true;
                    break;
                case Action::AddRank:
                    if (!leader) return fail(Error::Permission);
                    if (Ranks.size() >= 10) return fail(Error::Full);
                    if (!Characters(command.Text, 20, false)) return fail(Error::Invalid);
                    Ranks.push_back({command.Text, 0x43}); result.Durable = result.PermissionsChanged = true;
                    break;
                case Action::RemoveRank:
                    if (!leader) return fail(Error::Permission);
                    if (!command.RankId || command.RankId >= Ranks.size() || Ranks.size() <= 2) return fail(Error::Invalid);
                    // Renumber every affected membership in the same document/CAS.
                    Ranks.erase(Ranks.begin() + command.RankId);
                    for (auto& [guid, member] : Members)
                        if (member.RankId >= command.RankId)
                            member.RankId = std::uint8_t(std::min<std::size_t>(member.RankId - (member.RankId > command.RankId), Ranks.size() - 1));
                    result.Durable = result.PermissionsChanged = true;
                    break;
                case Action::EditRank:
                    if (!leader) return fail(Error::Permission);
                    if (command.RankId >= Ranks.size() || !Characters(command.Text, 20, false) || command.Rights > 0xFFFFFF) return fail(Error::Invalid);
                    Ranks[command.RankId] = {command.Text, command.RankId ? command.Rights : 0x00DDFFBF};
                    result.Durable = result.PermissionsChanged = true;
                    break;
                case Action::Disband:
                    if (!leader) return fail(Error::Permission);
                    result.Durable = result.Deleted = result.PermissionsChanged = true;
                    break;
                case Action::Invite:
                    if (!required(0x10)) return fail(Error::Permission);
                    if (!command.Target) return fail(Error::Invalid);
                    if (target != Members.end()) return fail(Error::AlreadyMember);
                    if (Members.size() >= 4096) return fail(Error::Full);
                    break;
                case Action::Speak:
                case Action::OfficerSpeak:
                {
                    bool officer = command.Type == Action::OfficerSpeak;
                    if (!required(officer ? 0x8 : 0x2)) return fail(Error::Permission);
                    for (auto const& [guid, member] : Members)
                        if (HasRight(guid, officer ? 0x4 : 0x1)) result.Recipients.push_back(guid);
                    break;
                }
                case Action::Roster: break;
                default: return fail(Error::Invalid);
            }
            return result;
        }
    private:
        static bool Characters(std::string const& text, std::size_t maximum, bool empty = true)
        {
            return Text(text, maximum * 3, empty) && std::count_if(text.begin(), text.end(),
                [](unsigned char c) { return (c & 0xC0) != 0x80; }) <= maximum;
        }
        static bool Text(std::string const& text, std::size_t maximum, bool empty = true)
        {
            return text.size() <= maximum && (empty || !text.empty()) && Cluster::ValidUtf8(text) &&
                std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c >= 0xF0; });
        }
    };
}
#endif
