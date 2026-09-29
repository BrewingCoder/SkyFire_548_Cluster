/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_CHAT_GUILD_WIRE_H
#define SKYFIRE_CHAT_GUILD_WIRE_H
#include "GuildState.h"
#include "ChatService.h"
#include "ChatRouting.h"
#include <array>
namespace Skyfire::Chat
{
    inline bool ReadGuildOptionalText(Cluster::Reader& in, std::string& value, std::size_t maximum)
    {
        std::uint16_t count; if (!in.U16(count) || count > maximum) return false;
        value.clear(); value.reserve(count);
        while (count--) { std::uint8_t byte; if (!in.U8(byte)) return false; value.push_back(char(byte)); }
        return value.empty() || Cluster::ValidUtf8(value);
    }
    enum class GuildAdmin : std::uint8_t { None, Create, Delete, Add, Remove, Rank, Rename, Projection };
    inline std::uint32_t GuildAdminPermission(GuildAdmin operation)
    { return (operation == GuildAdmin::None || operation == GuildAdmin::Projection) ? 0 : 401 + std::uint32_t(operation); }
    struct GuildRequest
    {
        std::uint32_t Guild = 0;
        std::uint64_t Revision = 0;
        GuildState::Command Command;
        GuildAdmin Admin = GuildAdmin::None;
        std::uint32_t Permission = 0;
        bool Console = false;
        std::uint32_t Language = 0;
        std::uint8_t ChatTag = 0;
        std::string Prefix;
        struct BankPermissions
        {
            struct Tab { std::uint8_t Rights = 0; std::uint32_t Slots = 0; };
            std::uint32_t Money = 0;
            std::array<Tab, 8> Tabs;
        } Bank;
        struct CreateProof
        {
            std::uint32_t Petition = 0, MinimumSignatures = 0;
            bool GameMaster = false;
            std::vector<std::uint64_t> Members;
            std::vector<std::string> RankNames;
        } Creation;
    };
    inline std::vector<std::uint8_t> EncodeGuildRequest(GuildRequest const& request)
    {
        Cluster::Writer out; out.U8(1); out.U32(request.Guild); Write64(out, request.Revision);
        out.U8(std::uint8_t(request.Command.Type)); Write64(out, request.Command.Target); Write64(out, request.Command.TargetIncarnation);
        out.U32(request.Command.RankId); out.U32(request.Command.Rights); out.String(request.Command.Text);
        if (request.Command.Type == GuildState::Action::EditRank)
        { out.U32(request.Bank.Money); for (auto const& tab : request.Bank.Tabs) { out.U8(tab.Rights); out.U32(tab.Slots); } }
        if (request.Command.Type == GuildState::Action::Create)
        {
            out.U32(request.Creation.Petition); out.U32(request.Creation.MinimumSignatures); out.U8(request.Creation.GameMaster);
            out.U16(std::uint16_t(request.Creation.Members.size()));
            for (auto guid : request.Creation.Members) Write64(out, guid);
            out.U8(std::uint8_t(request.Creation.RankNames.size()));
            for (auto const& name : request.Creation.RankNames) out.String(name);
        }
        if (request.Command.Type == GuildState::Action::Speak || request.Command.Type == GuildState::Action::OfficerSpeak)
        { out.U32(request.Language); out.U8(request.ChatTag); out.String(request.Prefix); }
        out.U8(std::uint8_t(request.Admin)); out.U32(request.Permission); out.U8(request.Console ? 1 : 0);
        return std::move(out.Bytes);
    }
    inline bool DecodeGuildRequest(std::vector<std::uint8_t> const& bytes, GuildRequest& request)
    {
        Cluster::Reader in(bytes); std::uint8_t version, action;
        if (!in.U8(version) || version != 1 || !in.U32(request.Guild) || !request.Guild ||
            !Read64(in, request.Revision) || !in.U8(action) || action > unsigned(GuildState::Action::Status) ||
            !Read64(in, request.Command.Target) || !Read64(in, request.Command.TargetIncarnation) || !in.U32(request.Command.RankId) ||
            !in.U32(request.Command.Rights) || !ReadRoutingBytes(in, request.Command.Text, 4096)) return false;
        request.Command.Type = GuildState::Action(action);
        if (request.Command.Type == GuildState::Action::EditRank)
        { if (!in.U32(request.Bank.Money)) return false; for (auto& tab : request.Bank.Tabs) if (!in.U8(tab.Rights) || !in.U32(tab.Slots)) return false; }
        if (request.Command.Type == GuildState::Action::Create)
        {
            std::uint8_t gm, ranks; std::uint16_t count;
            if (!in.U32(request.Creation.Petition) || !in.U32(request.Creation.MinimumSignatures) ||
                request.Creation.MinimumSignatures > 9 || !in.U8(gm) || gm > 1 || !in.U16(count) || !count || count > 10) return false;
            request.Creation.GameMaster = gm != 0; request.Creation.Members.clear();
            std::set<std::uint64_t> unique;
            while (count--) { std::uint64_t guid; if (!Read64(in,guid) || !guid || guid > UINT32_MAX || !unique.insert(guid).second) return false; request.Creation.Members.push_back(guid); }
            if (!in.U8(ranks) || ranks != 5) return false;
            request.Creation.RankNames.clear();
            while (ranks--) { std::string name; if (!in.String(name,80) || name.empty() || !Cluster::ValidUtf8(name)) return false; request.Creation.RankNames.push_back(std::move(name)); }
        }
        bool speaking = request.Command.Type == GuildState::Action::Speak || request.Command.Type == GuildState::Action::OfficerSpeak;
        if (speaking)
        {
            if (!in.U32(request.Language) || !in.U8(request.ChatTag) || !ReadRoutingBytes(in, request.Prefix, 16) ||
                request.Command.Text.size() > 511 || request.Command.Text.empty()) return false;
            if ((request.Language == 0xFFFFFFFFu) != !request.Prefix.empty() ||
                (request.Language != 0xFFFFFFFFu && !Cluster::ValidUtf8(request.Command.Text))) return false;
        }
        else if (!Cluster::ValidUtf8(request.Command.Text)) return false;
        if (!in.End())
        {
            std::uint8_t admin, console;
            if (!in.U8(admin) || admin > std::uint8_t(GuildAdmin::Projection) || !in.U32(request.Permission) ||
                !in.U8(console) || console > 1) return false;
            request.Admin = GuildAdmin(admin); request.Console = console != 0;
            if (request.Permission != GuildAdminPermission(request.Admin) || (request.Console && request.Admin == GuildAdmin::None)) return false;
        }
        return in.End();
    }
    struct GuildResponse
    {
        GuildState::Error Error = GuildState::Error::None;
        std::uint64_t Revision = 0;
        bool Deleted = false;
        GuildState State;
    };
    inline std::vector<std::uint8_t> EncodeGuildResponse(GuildResponse const& response)
    {
        Cluster::Writer out; out.U8(1); out.U8(std::uint8_t(response.Error)); Write64(out, response.Revision);
        out.U8(response.Deleted ? 1 : 0);
        if (!response.Deleted && response.Error == GuildState::Error::None)
        {
            auto const& state = response.State;
            out.U32(state.Id); Write64(out, state.Leader); out.String(state.Name); out.String(state.Motd); out.String(state.Info);
            out.U8(std::uint8_t(state.Ranks.size()));
            for (auto const& rank : state.Ranks) { out.String(rank.Name); out.U32(rank.Rights); }
            out.U32(std::uint32_t(state.Members.size()));
            for (auto const& [guid, member] : state.Members)
            { Write64(out, guid); out.U8(member.RankId); out.String(member.PublicNote); out.String(member.OfficerNote); }
        }
        return std::move(out.Bytes);
    }
    inline bool DecodeGuildResponse(std::vector<std::uint8_t> const& bytes, GuildResponse& response)
    {
        Cluster::Reader in(bytes); GuildResponse value; std::uint8_t version, error, deleted;
        if (bytes.size() > MaxServicePayload || !in.U8(version) || version != 1 || !in.U8(error) ||
            error > unsigned(GuildState::Error::InvitationRequired) || !Read64(in, value.Revision) ||
            !in.U8(deleted) || deleted > 1) return false;
        value.Error = GuildState::Error(error); value.Deleted = deleted != 0;
        if (!value.Deleted && value.Error == GuildState::Error::None)
        {
            auto& state = value.State; std::uint8_t ranks; std::uint32_t members;
            if (!in.U32(state.Id) || !Read64(in, state.Leader) || !in.String(state.Name, 128) ||
                !ReadGuildOptionalText(in, state.Motd, 2048) || !ReadGuildOptionalText(in, state.Info, 4096) || !in.U8(ranks) || !ranks || ranks > 10) return false;
            for (std::uint8_t i = 0; i < ranks; ++i)
            { GuildState::Rank rank; if (!in.String(rank.Name, 128) || !in.U32(rank.Rights)) return false; state.Ranks.push_back(std::move(rank)); }
            if (!in.U32(members) || !members || members > 4096) return false;
            for (std::uint32_t i = 0; i < members; ++i)
            {
                std::uint64_t guid; GuildState::Member member;
                if (!Read64(in, guid) || !in.U8(member.RankId) || !ReadGuildOptionalText(in, member.PublicNote, 512) ||
                    !ReadGuildOptionalText(in, member.OfficerNote, 512) || !state.Members.emplace(guid, std::move(member)).second) return false;
            }
            if (!state.Valid()) return false;
        }
        if (!in.End()) return false; response = std::move(value); return true;
    }
}
#endif
