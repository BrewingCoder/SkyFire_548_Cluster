/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "GuildService.h"
#include "Config.h"
#include "Language.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "WorldSession.h"
#include <map>
#include <memory>
#include <set>

namespace Skyfire::Chat::GuildService
{
    namespace
    {
        struct Pending
        {
            std::uint32_t Guild, Account;
            std::uint64_t Actor, Incarnation;
            GuildState::Command Command;
            bool Fetch;
            std::function<void(::Guild*, WorldSession*)> Applied;
            GuildRequest::CreateProof Creation;
            GuildAdmin Admin = GuildAdmin::None;
            bool Console = false;
            std::uint32_t Language = 0;
            std::uint8_t ChatTag = 0;
            std::string Prefix;
            bool Poll = false;
            GuildRequest::BankPermissions Bank;
            std::uint64_t Recover = 0;
        };
        std::map<std::uint64_t, Pending> Requests;
        std::map<std::uint64_t, Pending> Uncertain;
        std::map<std::uint32_t, std::uint64_t> Revisions;
        std::set<std::uint32_t> Fenced;
        thread_local bool ApplyingResult = false;
        std::uint64_t Queue(Pending const& pending, std::uint64_t revision)
        {
            GuildRequest wire; wire.Guild = pending.Guild; wire.Revision = revision; wire.Command = pending.Command; wire.Creation = pending.Creation;
            wire.Bank = pending.Bank;
            wire.Language = pending.Language; wire.ChatTag = pending.ChatTag; wire.Prefix = pending.Prefix;
            wire.Admin = pending.Admin; wire.Permission = GuildAdminPermission(pending.Admin); wire.Console = pending.Console;
            if (pending.Fetch) wire.Command.Type = GuildState::Action::Roster;
            if (pending.Recover) { wire.Command.Type = GuildState::Action::Status; wire.Command.Target = pending.Recover; }
            ServiceRequest request; request.Domain = ServiceDomain::Guild; request.Account = pending.Account;
            request.Actor = pending.Actor; request.Incarnation = pending.Incarnation; request.Payload = EncodeGuildRequest(wire);
            return QueueServiceRequest(std::move(request));
        }
    }
    bool Enabled()
    {
        // Ownership changes require a restart; config reload cannot bypass the
        // startup import/projection and legacy-writer checks.
        static bool const enabled = sConfigMgr->GetBoolDefault("Chat.GuildAuthority.Enable", false);
        return enabled;
    }
    bool Applying() { return ApplyingResult; }
    bool Blocked(std::uint32_t guild)
    {
        if (!Enabled()) return false;
        if (Fenced.count(guild)) return true;
        for (auto const& [sequence, pending] : Requests) if (pending.Guild == guild && pending.Command.Type != GuildState::Action::Speak && pending.Command.Type != GuildState::Action::OfficerSpeak) return true;
        return false;
    }
    bool Submit(WorldSession* session, std::uint32_t guild, GuildState::Command command,
        std::function<void(::Guild*, WorldSession*)> applied, GuildRequest::BankPermissions bank)
    {
        if (!Enabled()) return false;
        if (!session || !session->GetPlayer()) return true;
        if (Blocked(guild) || Requests.size() >= 128 || !ClientEnabled())
        { session->SendNotification("Guild service unavailable or a guild change is pending."); return true; }
        auto player = session->GetPlayer();
        Pending pending{guild, session->GetAccountId(), player->GetGUID(), session->GetChatIncarnation(),
            std::move(command), !Revisions.count(guild), std::move(applied)};
        pending.Bank = bank;
        auto sequence = Queue(pending, Revisions[guild]);
        if (!sequence) { Revisions.erase(guild); session->SendNotification("Guild service unavailable. Change was not sent."); return true; }
        Requests.emplace(sequence, std::move(pending)); return true;
    }
    void Update()
    {
        static auto next = std::chrono::steady_clock::now();
        static std::uint32_t previous = 0;
        static std::map<std::uint32_t, std::chrono::steady_clock::time_point> refreshed;
        auto now = std::chrono::steady_clock::now();
        if (!Enabled() || !ClientEnabled() || now < next || Requests.size() >= 96) return;
        next = now + std::chrono::seconds(1);
        static std::uint64_t previousRecovery = 0;
        static bool recoveryTurn = false;
        recoveryTurn = !recoveryTurn;
        if (recoveryTurn && !Uncertain.empty())
        {
            auto selectedRecovery = Uncertain.upper_bound(previousRecovery);
            if (selectedRecovery == Uncertain.end()) selectedRecovery = Uncertain.begin();
            previousRecovery = selectedRecovery->first;
            bool waiting = false;
            for (auto const& [sequence, request] : Requests) if (request.Recover == previousRecovery) waiting = true;
            if (!waiting)
            {
                Pending recovery = selectedRecovery->second; recovery.Recover = previousRecovery;
                auto sequence = Queue(recovery, 0);
                if (sequence) Requests.emplace(sequence, std::move(recovery));
            }
            return;
        }
        auto ids = sGuildMgr->GetGuildIds();
        if (ids.empty()) return;
        for (auto it = refreshed.begin(); it != refreshed.end();)
            if (!std::binary_search(ids.begin(), ids.end(), it->first)) it = refreshed.erase(it); else ++it;
        auto selected = std::upper_bound(ids.begin(), ids.end(), previous);
        if (selected == ids.end()) selected = ids.begin();
        auto guild = *selected; previous = guild;
        for (auto const& [sequence,pending] : Uncertain) if (pending.Guild == guild) return;
        for (auto const& entry : Requests)
            if (entry.second.Guild == guild && entry.second.Command.Type != GuildState::Action::Speak &&
                entry.second.Command.Type != GuildState::Action::OfficerSpeak) return;
        if (!Fenced.count(guild) && refreshed.count(guild) && now - refreshed[guild] < std::chrono::seconds(5)) return;
        Pending pending{}; pending.Guild = guild; pending.Command.Type = GuildState::Action::Roster;
        pending.Admin = GuildAdmin::Projection; pending.Console = true; pending.Poll = true;
        auto sequence = Queue(pending, 0);
        if (sequence) { Requests.emplace(sequence, std::move(pending)); refreshed[guild] = now; }
    }
    bool Message(WorldSession* session, std::uint32_t guild, bool officer, std::string const& text,
        std::uint32_t language, std::string const& prefix)
    {
        if (!Enabled()) return false;
        if (!session || !session->GetPlayer()) return true;
        auto* player = session->GetPlayer();
        if (language != uint32(Language::LANG_ADDON) && (!player->CanSpeak() || player->HasAura(1852))) return true;
        if (Requests.size() >= 128 || !ClientEnabled() || player->GetGuildId() != guild)
        { session->SendNotification("Guild chat unavailable. Message was not sent."); return true; }
        Pending pending{}; pending.Guild = guild; pending.Account = session->GetAccountId();
        pending.Actor = player->GetGUID(); pending.Incarnation = session->GetChatIncarnation();
        pending.Command.Type = officer ? GuildState::Action::OfficerSpeak : GuildState::Action::Speak;
        pending.Command.Text = text; pending.Language = language; pending.ChatTag = player->GetChatTag(); pending.Prefix = prefix;
        auto sequence = Queue(pending, 0);
        if (!sequence) { session->SendNotification("Guild chat unavailable. Message was not sent."); return true; }
        Requests.emplace(sequence, std::move(pending)); return true;
    }
    bool Create(WorldSession* session, std::uint32_t guild, std::string const& name, std::uint32_t petition,
        std::uint32_t minimumSignatures, bool gameMaster, std::vector<std::uint64_t> members,
        std::function<void(::Guild*, WorldSession*)> applied)
    {
        if (!Enabled()) return false;
        if (!session || !session->GetPlayer()) return true;
        if (Blocked(guild) || Requests.size() >= 128 || !ClientEnabled())
        { session->SendNotification("Guild service unavailable. Guild was not created."); return true; }
        Pending pending{guild, session->GetAccountId(), session->GetPlayer()->GetGUID(), session->GetChatIncarnation(), {}, false, std::move(applied), {}};
        pending.Command.Type = GuildState::Action::Create; pending.Command.Text = name;
        pending.Creation.Petition = petition; pending.Creation.MinimumSignatures = minimumSignatures;
        pending.Creation.GameMaster = gameMaster; pending.Creation.Members = std::move(members);
        for (auto id : {LANG_GUILD_MASTER, LANG_GUILD_OFFICER, LANG_GUILD_VETERAN, LANG_GUILD_MEMBER, LANG_GUILD_INITIATE})
            pending.Creation.RankNames.push_back(sObjectMgr->GetSkyFireString(id, session->GetSessionDbLocaleIndex()));
        auto sequence = Queue(pending, 0);
        if (!sequence) { session->SendNotification("Guild service unavailable. Guild was not created."); return true; }
        Requests.emplace(sequence, std::move(pending)); return true;
    }
    bool Admin(WorldSession* session, GuildAdmin operation, std::uint32_t guild, std::uint64_t target,
        std::uint32_t rank, std::string const& text)
    {
        if (!Enabled()) return false;
        if (operation == GuildAdmin::None || (session && (!session->GetPlayer() || !session->HasPermission(rbac::RBACPermissions(GuildAdminPermission(operation)))))) return true;
        if (Blocked(guild) || Requests.size() >= 128 || !ClientEnabled())
        {
            if (session) session->SendNotification("Guild service unavailable or a guild change is pending.");
            else SF_LOG_ERROR("server.chat", "Guild administration could not be queued: service unavailable or guild busy.");
            return true;
        }
        Pending pending{}; pending.Guild = guild; pending.Admin = operation; pending.Console = session == nullptr;
        if (session) { pending.Account = session->GetAccountId(); pending.Actor = session->GetPlayer()->GetGUID(); pending.Incarnation = session->GetChatIncarnation(); }
        pending.Command.Target = target; pending.Command.RankId = rank; pending.Command.Text = text;
        switch (operation)
        {
            case GuildAdmin::Create: pending.Command.Type = GuildState::Action::Create; break;
            case GuildAdmin::Delete: pending.Command.Type = GuildState::Action::Disband; break;
            case GuildAdmin::Add: pending.Command.Type = GuildState::Action::Accept; break;
            case GuildAdmin::Remove: pending.Command.Type = GuildState::Action::Remove; break;
            case GuildAdmin::Rank: pending.Command.Type = GuildState::Action::SetRank; break;
            case GuildAdmin::Rename: pending.Command.Type = GuildState::Action::Info; break;
            default: return true;
        }
        pending.Fetch = operation != GuildAdmin::Create && !Revisions.count(guild);
        if (operation == GuildAdmin::Create)
        {
            pending.Creation.GameMaster = true; pending.Creation.Members = {target};
            for (auto id : {LANG_GUILD_MASTER, LANG_GUILD_OFFICER, LANG_GUILD_VETERAN, LANG_GUILD_MEMBER, LANG_GUILD_INITIATE})
                pending.Creation.RankNames.push_back(sObjectMgr->GetSkyFireString(id, session ? session->GetSessionDbLocaleIndex() : DEFAULT_LOCALE));
        }
        auto sequence = Queue(pending, operation == GuildAdmin::Create ? 0 : Revisions[guild]);
        if (!sequence)
        {
            if (session) session->SendNotification("Guild service unavailable. Administration was not sent.");
            else SF_LOG_ERROR("server.chat", "Guild administration was not sent.");
            return true;
        }
        Requests.emplace(sequence, std::move(pending)); return true;
    }
    void HandleResult(ServiceResult const& result)
    {
        if (result.Request.Domain != ServiceDomain::Guild) return;
        auto found = Requests.find(result.Request.Sequence);
        if (found == Requests.end()) return;
        auto pending = std::move(found->second); Requests.erase(found);
        if (pending.Recover)
        {
            if (!result.Success || result.Response.Status == ServiceStatus::Unknown || result.Response.Status == ServiceStatus::Unavailable) return;
            if (result.Response.Status == ServiceStatus::Ok)
            {
                GuildResponse verified;
                if (!DecodeGuildResponse(result.Response.Payload, verified) || verified.Error != GuildState::Error::None) return;
            }
            Uncertain.erase(pending.Recover);
            if (result.Response.Status == ServiceStatus::Rejected) Fenced.erase(pending.Guild);
            pending.Recover = 0;
        }
        auto* player = ObjectAccessor::FindPlayer(pending.Actor);
        auto* session = player ? player->GetSession() : nullptr;
        bool current = pending.Console || (session && session->GetAccountId() == pending.Account && session->GetChatIncarnation() == pending.Incarnation);
        if (current && !pending.Console && pending.Admin != GuildAdmin::None)
            current = session->HasPermission(rbac::RBACPermissions(GuildAdminPermission(pending.Admin)));
        if (pending.Command.Type == GuildState::Action::Speak || pending.Command.Type == GuildState::Action::OfficerSpeak)
        {
            if (current && session && (!result.Success || result.Response.Status != ServiceStatus::Ok))
                session->SendNotification("Guild chat service could not confirm delivery.");
            return;
        }
        GuildResponse response;
        if (!result.Success || result.Response.Status != ServiceStatus::Ok ||
            !DecodeGuildResponse(result.Response.Payload, response) || response.Error != GuildState::Error::None)
        {
            Revisions.erase(pending.Guild);
            // A timeout after submission cannot prove that the durable write did
            // not happen. Keep bank use fenced until a fresh process reload.
            if (!pending.Fetch && !pending.Poll && (!result.Success || result.Response.Status == ServiceStatus::Unknown))
            { Fenced.insert(pending.Guild); Uncertain.emplace(result.Request.Sequence, pending); }
            if (current && session) session->SendNotification("Guild service could not confirm this change. Retry after guild state is refreshed.");
            if (pending.Console && !pending.Poll) SF_LOG_ERROR("server.chat", "Guild administration could not confirm guild %u request %llu.", pending.Guild, static_cast<unsigned long long>(result.Request.Sequence));
            return;
        }
        auto* guild = sGuildMgr->GetGuildById(pending.Guild);
        if (response.Deleted)
        {
            if (guild) { guild->ApplyDisbandProjection(); delete guild; }
            Revisions.erase(pending.Guild); Fenced.erase(pending.Guild);
            if (pending.Admin != GuildAdmin::None && !pending.Poll) SF_LOG_INFO("server.chat", "Guild administration committed deletion for guild %u.", pending.Guild);
            return;
        }
        if (!guild && pending.Command.Type == GuildState::Action::Create)
        {
            auto created = std::make_unique<::Guild>();
            if (created->ApplyCreateProjection(response.State))
            { guild = created.release(); sGuildMgr->AddGuild(guild); }
        }
        if (!guild || response.State.Id != pending.Guild || !guild->ApplyChatProjection(response.State, !pending.Fetch && pending.Command.Type == GuildState::Action::RemoveRank ? uint8(pending.Command.RankId) : 255))
        { Fenced.insert(pending.Guild); if (current && session) session->SendNotification("Guild projection requires a world restart."); return; }
        Revisions[pending.Guild] = response.Revision;
        Fenced.erase(pending.Guild);
        if (pending.Poll) return;
        if (pending.Fetch)
        {
            if (!current || (pending.Admin == GuildAdmin::None && (player->GetGuildId() != pending.Guild &&
                !(pending.Command.Type == GuildState::Action::Accept && player->GetGuildIdInvited() == pending.Guild)))) return;
            pending.Fetch = false;
            auto sequence = Queue(pending, response.Revision);
            if (!sequence) { if (session) session->SendNotification("Guild service unavailable. Change was not sent."); else SF_LOG_ERROR("server.chat", "Guild administration could not be submitted after state refresh."); return; }
            Requests.emplace(sequence, std::move(pending));
        }
        else if (pending.Admin != GuildAdmin::None)
        {
            if (session && current) session->SendNotification("Guild administration committed.");
            SF_LOG_INFO("server.chat", "Guild administration action %u committed for guild %u (account %u).", unsigned(pending.Admin), pending.Guild, pending.Account);
        }
        else if ((current || pending.Command.Type == GuildState::Action::EditRank) && pending.Applied)
        {
            struct Scope { Scope() { ApplyingResult = true; } ~Scope() { ApplyingResult = false; } } scope;
            pending.Applied(guild, session);
        }
    }
}
