/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "GuildAuthority.h"
#include "Cluster/GuildWire.h"
#include "Cluster/GuildEvents.h"
#include "Cluster/HandoffClient.h"
#include <boost/json.hpp>
#include <stdexcept>

namespace Skyfire::Chat
{
    namespace
    {
        std::uint64_t Number(boost::json::value const& value, std::uint64_t maximum = UINT64_MAX)
        {
            auto result = value.is_uint64() ? value.as_uint64() :
                value.is_int64() && value.as_int64() >= 0 ? std::uint64_t(value.as_int64()) : throw std::runtime_error("Invalid guild integer");
            if (result > maximum) throw std::runtime_error("Guild integer overflow");
            return result;
        }
        std::string Text(boost::json::value const& value)
        { auto const& text = value.as_string(); return {text.data(), text.size()}; }
        ServiceResponse Reply(std::uint64_t sequence, ServiceStatus status, std::uint64_t revision, boost::json::value const& document)
        {
            GuildResponse reply; reply.Revision = revision; reply.Deleted = document.is_null();
            if (!reply.Deleted) reply.State = GuildAuthority::Decode(document);
            return {sequence, status, EncodeGuildResponse(reply)};
        }
    }

    GuildState GuildAuthority::Decode(boost::json::value const& document)
    {
        auto const& object = document.as_object();
        if (object.size() != 7) throw std::runtime_error("Invalid guild document fields");
        GuildState state;
        state.Id = std::uint32_t(Number(object.at("id"), UINT32_MAX));
        state.Leader = Number(object.at("leader"));
        state.Name = Text(object.at("name")); state.Motd = Text(object.at("motd")); state.Info = Text(object.at("info"));
        for (auto const& value : object.at("ranks").as_array())
        {
            auto const& rank = value.as_object();
            if (rank.size() != 2) throw std::runtime_error("Invalid guild rank");
            state.Ranks.push_back({Text(rank.at("name")), std::uint32_t(Number(rank.at("rights"), UINT32_MAX))});
        }
        for (auto const& value : object.at("members").as_array())
        {
            auto const& member = value.as_object();
            if (member.size() != 4 || !state.Members.emplace(Number(member.at("guid")), GuildState::Member{
                std::uint8_t(Number(member.at("rank"), 255)), Text(member.at("public_note")), Text(member.at("officer_note"))}).second)
                throw std::runtime_error("Invalid guild member");
        }
        if (!state.Valid()) throw std::runtime_error("Invalid guild state");
        return state;
    }
    boost::json::value GuildAuthority::Encode(GuildState const& state)
    {
        if (!state.Valid()) throw std::runtime_error("Invalid guild candidate");
        boost::json::array ranks, members;
        for (auto const& rank : state.Ranks) ranks.push_back(boost::json::object{{"name", rank.Name}, {"rights", rank.Rights}});
        for (auto const& [guid, member] : state.Members) members.push_back(boost::json::object{
            {"guid", guid}, {"rank", member.RankId}, {"public_note", member.PublicNote}, {"officer_note", member.OfficerNote}});
        return boost::json::object{{"id", state.Id}, {"name", state.Name}, {"leader", state.Leader},
            {"motd", state.Motd}, {"info", state.Info}, {"ranks", std::move(ranks)}, {"members", std::move(members)}};
    }
    void GuildAuthority::Handle(std::uint32_t realm, std::string const& node, ServiceRequest const& request, ServiceCompletion complete)
    {
        if (!Persistence.Enabled() || !Persistence.Ready(realm))
        {
            Committed.clear();
            complete({request.Sequence, ServiceStatus::Unavailable, {}}); return;
        }
        try
        {
            GuildRequest wire;
            if (!DecodeGuildRequest(request.Payload, wire)) throw std::runtime_error("Invalid guild command");
            if (wire.Command.Type == GuildState::Action::Status)
            {
                auto prior = Outcomes.find({realm, node, request.Generation, wire.Command.Target});
                if (prior == Outcomes.end()) { complete({request.Sequence, ServiceStatus::Unknown, {}}); return; }
                auto const& record = prior->second;
                if (record.Guild != wire.Guild || record.Account != request.Account || record.Actor != request.Actor || record.Incarnation != request.Incarnation)
                    throw std::runtime_error("Guild receipt identity mismatch");
                if (!record.Response) { complete({request.Sequence, ServiceStatus::Unknown, {}}); return; }
                auto response = *record.Response; response.Sequence = request.Sequence; complete(std::move(response)); return;
            }
            bool track = wire.Command.Type != GuildState::Action::Roster && wire.Command.Type != GuildState::Action::Speak && wire.Command.Type != GuildState::Action::OfficerSpeak;
            if (track)
            {
                // Bound completed receipt retention. Missing receipts remain
                // unknown, never evidence that an operation did not commit.
                auto cutoff = std::chrono::steady_clock::now() - std::chrono::minutes(30);
                for (auto it = Outcomes.begin(); it != Outcomes.end();)
                    if (it->second.Response && it->second.Updated < cutoff) it = Outcomes.erase(it); else ++it;
                if (Outcomes.size() >= 4096) { complete({request.Sequence, ServiceStatus::Unavailable, {}}); return; }
                OutcomeKey identity{realm, node, request.Generation, request.Sequence};
                Outcomes.emplace(identity, OutcomeRecord{wire.Guild, request.Account, request.Actor, request.Incarnation, {}});
                complete = [this, identity, done = std::move(complete)](ServiceResponse response) mutable
                {
                    Outcomes.at(identity).Response = response;
                    Outcomes.at(identity).Updated = std::chrono::steady_clock::now();
                    done(std::move(response));
                };
            }
            bool admin = wire.Admin != GuildAdmin::None;
            bool console = !request.Actor && !request.Account && !request.Incarnation;
            if (wire.Console != console || (console && !admin) || (admin && wire.Permission != GuildAdminPermission(wire.Admin)))
                throw std::runtime_error("Invalid guild administration attestation");
            if (wire.Admin == GuildAdmin::Projection && (!console || wire.Command.Type != GuildState::Action::Roster))
                throw std::runtime_error("Projection requests are read-only");
            auto guild = wire.Guild;
            auto expected = wire.Revision;
            auto command = std::move(wire.Command);
            command.Actor = request.Actor;
            auto key = "guild-" + std::to_string(guild);
            if (PendingRequests.size() >= 128) { complete({request.Sequence, ServiceStatus::Unavailable, {}}); return; }
            for (auto const& [id, pending] : PendingRequests)
                if (pending.Realm == realm && pending.Key == key) { complete({request.Sequence, ServiceStatus::Unavailable, {}}); return; }
            if (command.Type == GuildState::Action::Create)
            {
                SocialRecord previous;
                auto founder = wire.Admin == GuildAdmin::Create ? command.Target : request.Actor;
                if ((admin && wire.Admin != GuildAdmin::Create) || !founder ||
                    expected || Committed.count({realm, key}) || Persistence.Read(realm, "guilds", key, previous) ||
                    std::find(wire.Creation.Members.begin(), wire.Creation.Members.end(), founder) == wire.Creation.Members.end())
                    throw std::runtime_error("Guild identity already exists");
                GuildState created; created.Id = guild; created.Name = command.Text; created.Leader = founder;
                created.Motd = "No message set.";
                for (std::size_t i = 0; i < wire.Creation.RankNames.size(); ++i)
                    created.Ranks.push_back({wire.Creation.RankNames[i], i < 2 ? 0x00DDFFBFu : 0x43u});
                for (auto guid : wire.Creation.Members) created.Members.emplace(guid, GuildState::Member{std::uint8_t(guid == founder ? 0 : 4), {}, {}});
                auto document = Encode(created);
                auto receipt = Cluster::Handoff::RandomToken().substr(0, 32);
                boost::json::object context{{"petition", wire.Creation.Petition}, {"minimum_signatures", wire.Creation.MinimumSignatures}, {"game_master", wire.Creation.GameMaster}};
                if (admin) { context["admin_permission"] = wire.Permission; context["console"] = wire.Console; context["founder"] = founder; }
                if (!Persistence.Submit(realm, "guilds", receipt, key, 0, request.Actor, document, std::move(context)))
                { complete({request.Sequence, ServiceStatus::Unavailable, {}}); return; }
                PendingRequests.emplace(receipt, Pending{realm, request.Sequence, request.Actor, std::move(key), std::move(document), std::move(complete)});
                return;
            }
            SocialRecord record;
            auto local = Committed.find({realm, key});
            bool loaded = Persistence.Read(realm, "guilds", key, record);
            if (local != Committed.end() && (!loaded || local->second.Revision > record.Revision)) record = local->second;
            else if (!loaded) throw std::runtime_error("Unknown guild");
            if (record.Document.is_null())
            {
                if (command.Type == GuildState::Action::Roster) { complete(Reply(request.Sequence, ServiceStatus::Ok, record.Revision, record.Document)); return; }
                throw std::runtime_error("Deleted guild");
            }
            if (command.Type != GuildState::Action::Roster && command.Type != GuildState::Action::Speak &&
                command.Type != GuildState::Action::OfficerSpeak && expected != record.Revision)
                throw std::runtime_error("Stale guild projection");
            auto candidate = Decode(record.Document);
            if (candidate.Id != guild) throw std::runtime_error("Guild key mismatch");
            auto now = std::chrono::steady_clock::now();
            for (auto i = Invitations.begin(); i != Invitations.end();)
                if (i->second.Expires <= now) i = Invitations.erase(i); else ++i;
            auto invitation = Invitations.find({realm, request.Actor});
            command.HasInvitation = invitation != Invitations.end() && invitation->second.Guild == guild &&
                invitation->second.TargetIncarnation == request.Incarnation && candidate.HasRight(invitation->second.Inviter, 0x10) &&
                Presence && Presence(realm, invitation->second.Inviter, invitation->second.InviterIncarnation);
            if (command.Type == GuildState::Action::Invite && !command.Text.empty())
            {
                auto target = FindName ? FindName(realm, command.Text) : std::nullopt;
                if (!target) throw std::runtime_error("Guild invite target unavailable");
                command.Target = target->Guid; command.TargetIncarnation = target->Incarnation;
            }
            if (command.Type == GuildState::Action::Invite && (!Presence || !command.TargetIncarnation ||
                !Presence(realm, command.Target, command.TargetIncarnation) || Invitations.size() >= 4096))
                throw std::runtime_error("Guild invitation target unavailable");
            auto previousLeader = candidate.Leader;
            GuildState::Result result;
            if (admin && command.Type != GuildState::Action::Roster)
            {
                auto target = candidate.Members.find(command.Target);
                switch (wire.Admin)
                {
                    case GuildAdmin::Delete:
                        if (command.Type != GuildState::Action::Disband) throw std::runtime_error("Invalid administrative action");
                        result.Deleted = result.Durable = true; break;
                    case GuildAdmin::Add:
                        if (command.Type != GuildState::Action::Accept || !command.Target || target != candidate.Members.end() || candidate.Members.size() >= 4096)
                            throw std::runtime_error("Invalid administrative addition");
                        candidate.Members.emplace(command.Target, GuildState::Member{std::uint8_t(candidate.Ranks.size()-1), {}, {}});
                        result.Durable = result.PermissionsChanged = true; break;
                    case GuildAdmin::Remove:
                        if (command.Type != GuildState::Action::Remove || target == candidate.Members.end()) throw std::runtime_error("Invalid administrative removal");
                        candidate.Members.erase(target);
                        if (candidate.Members.empty()) result.Deleted = true;
                        else if (candidate.Leader == command.Target)
                        {
                            auto successor = std::min_element(candidate.Members.begin(), candidate.Members.end(), [](auto const& left, auto const& right)
                            { return left.second.RankId < right.second.RankId; });
                            candidate.Leader = successor->first; successor->second.RankId = 0;
                        }
                        result.Durable = result.PermissionsChanged = true; break;
                    case GuildAdmin::Rank:
                        if (command.Type != GuildState::Action::SetRank || target == candidate.Members.end() || command.RankId >= candidate.Ranks.size() ||
                            (command.Target == candidate.Leader && command.RankId != 0)) throw std::runtime_error("Invalid administrative rank");
                        if (!command.RankId && command.Target != candidate.Leader)
                        { candidate.Members.at(candidate.Leader).RankId = std::uint8_t(candidate.Ranks.size()-1); candidate.Leader = command.Target; }
                        result.Durable = target->second.RankId != command.RankId; target->second.RankId = std::uint8_t(command.RankId);
                        result.PermissionsChanged = result.Durable; break;
                    case GuildAdmin::Rename:
                        if (command.Type != GuildState::Action::Info || command.Text.empty()) throw std::runtime_error("Invalid administrative name");
                        result.Durable = candidate.Name != command.Text; candidate.Name = command.Text; break;
                    default: throw std::runtime_error("Invalid administrative operation");
                }
            }
            else if (!(command.Type == GuildState::Action::Roster && (admin || command.HasInvitation))) result = candidate.Apply(command);
            if (result.Status == GuildState::Error::None && command.Type == GuildState::Action::Invite)
            {
                auto sender = Find ? Find(realm,request.Actor) : std::nullopt;
                if (!sender || !sender->ProfileKnown || sender->Incarnation != request.Incarnation || !Send)
                    throw std::runtime_error("Guild inviter unavailable");
                auto payload = EncodeGuildInvitationEvent({guild, sender->Team, request.Actor, request.Incarnation, sender->Name});
                if (!Send(realm, ServiceEvent{ServiceDomain::Guild,command.Target,command.TargetIncarnation,std::move(payload)}))
                    throw std::runtime_error("Guild invitation delivery unavailable");
                Invitations[{realm, command.Target}] = Invitation{std::uint32_t(guild), command.TargetIncarnation,
                    request.Actor, request.Incarnation, now + std::chrono::seconds(60)};
            }
            if (result.Status != GuildState::Error::None)
            {
                GuildResponse denied; denied.Error = result.Status;
                complete({request.Sequence, ServiceStatus::Rejected, EncodeGuildResponse(denied)}); return;
            }
            if (command.Type == GuildState::Action::Speak || command.Type == GuildState::Action::OfficerSpeak)
            {
                auto sender = Find ? Find(realm, request.Actor) : std::nullopt;
                if (!sender || sender->Incarnation != request.Incarnation || !Send)
                { complete({request.Sequence, ServiceStatus::Unavailable, {}}); return; }
                GuildMessageEvent message; message.Guild = guild; message.Language = wire.Language;
                message.Sender = request.Actor; message.SenderIncarnation = request.Incarnation; message.SenderName = sender->Name;
                message.ChatTag = wire.ChatTag; message.Officer = command.Type == GuildState::Action::OfficerSpeak;
                message.Text = command.Text; message.Prefix = wire.Prefix;
                auto payload = EncodeGuildMessageEvent(message);
                bool sent = true;
                for (auto guid : result.Recipients)
                    if (auto recipient = Find(realm, guid))
                        if (!Send(realm, ServiceEvent{ServiceDomain::Guild, guid, recipient->Incarnation, payload})) sent = false;
                complete({request.Sequence, sent ? ServiceStatus::Ok : ServiceStatus::Unknown, {}}); return;
            }
            auto document = result.Deleted ? boost::json::value(nullptr) : Encode(candidate);
            if (!result.Durable)
            {
                complete(Reply(request.Sequence, ServiceStatus::Ok, record.Revision, document)); return;
            }
            auto receipt = Cluster::Handoff::RandomToken().substr(0, 32);
            boost::json::value context;
            if (admin) context = boost::json::object{{"admin_permission", wire.Permission}, {"console", wire.Console}};
            else if (command.Type == GuildState::Action::EditRank)
            {
                boost::json::array tabs;
                for (auto const& tab : wire.Bank.Tabs)
                    tabs.push_back(boost::json::object{{"rights", command.RankId ? tab.Rights : 255u}, {"slots", command.RankId ? tab.Slots : UINT32_MAX}});
                context = boost::json::object{{"rank_bank", boost::json::object{{"rank", command.RankId},
                    {"money", command.RankId ? wire.Bank.Money : UINT32_MAX}, {"tabs", std::move(tabs)}}}};
            }
            else if (command.Type == GuildState::Action::RemoveRank) context = boost::json::object{{"rank_removed", command.RankId}};
            else if (command.Type == GuildState::Action::ClaimLeader) context = boost::json::object{{"claim_leader", previousLeader}};
            if (!Persistence.Submit(realm, "guilds", receipt, key, record.Revision, request.Actor, document, std::move(context)))
            { complete({request.Sequence, ServiceStatus::Unavailable, {}}); return; }
            if (command.Type == GuildState::Action::Accept) Invitations.erase({realm, request.Actor});
            PendingRequests.emplace(receipt, Pending{realm, request.Sequence, request.Actor, std::move(key), std::move(document), std::move(complete)});
        }
        catch (std::exception const&)
        { complete({request.Sequence, ServiceStatus::Rejected, {}}); }
    }
    void GuildAuthority::Complete(SocialPersistence::Result const& result)
    {
        if (result.Domain != "guilds") return;
        auto found = PendingRequests.find(result.Request);
        if (found == PendingRequests.end()) return;
        auto pending = std::move(found->second); PendingRequests.erase(found);
        if (result.Status == SocialPersistence::Outcome::Committed)
        {
            Committed[{pending.Realm, pending.Key}] = {result.Revision, pending.Document};
            pending.Completion(Reply(pending.Sequence, ServiceStatus::Ok, result.Revision, pending.Document));
        }
        else
        {
            Committed.erase({pending.Realm, pending.Key});
            pending.Completion({pending.Sequence, result.Status == SocialPersistence::Outcome::Unknown ? ServiceStatus::Unknown :
                ServiceStatus::Rejected, {}});
        }
    }
}
