/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_CHAT_GUILD_EVENTS_H
#define SKYFIRE_CHAT_GUILD_EVENTS_H
#include "ChatRouting.h"
namespace Skyfire::Chat
{
    struct GuildInvitationEvent
    {
        std::uint32_t Guild = 0, Team = 0;
        std::uint64_t Sender = 0, Incarnation = 0;
        std::string Name;
    };
    inline std::vector<std::uint8_t> EncodeGuildInvitationEvent(GuildInvitationEvent const& value)
    {
        Cluster::Writer out; out.U8(2); out.U32(value.Guild); out.U32(value.Team);
        Write64(out, value.Sender); Write64(out, value.Incarnation); out.String(value.Name); return std::move(out.Bytes);
    }
    inline bool DecodeGuildInvitationEvent(std::vector<std::uint8_t> const& bytes, GuildInvitationEvent& value)
    {
        Cluster::Reader in(bytes); std::uint8_t version;
        return bytes.size() <= 96 && in.U8(version) && version == 2 && in.U32(value.Guild) && value.Guild &&
            in.U32(value.Team) && Read64(in,value.Sender) && value.Sender && Read64(in,value.Incarnation) && value.Incarnation &&
            in.String(value.Name,48) && !value.Name.empty() && in.End();
    }
    struct GuildMessageEvent
    {
        std::uint32_t Guild = 0, Language = 0;
        std::uint64_t Sender = 0, SenderIncarnation = 0;
        std::uint8_t ChatTag = 0;
        bool Officer = false;
        std::string SenderName, Text, Prefix;
    };
    inline std::vector<std::uint8_t> EncodeGuildMessageEvent(GuildMessageEvent const& event)
    {
        Cluster::Writer out; out.U8(1); out.U32(event.Guild); out.U32(event.Language);
        Write64(out, event.Sender); Write64(out, event.SenderIncarnation); out.U8(event.ChatTag);
        out.U8(event.Officer ? 1 : 0); out.String(event.SenderName); out.String(event.Text); out.String(event.Prefix);
        return std::move(out.Bytes);
    }
    inline bool DecodeGuildMessageEvent(std::vector<std::uint8_t> const& bytes, GuildMessageEvent& event)
    {
        Cluster::Reader in(bytes); GuildMessageEvent value; std::uint8_t version, officer;
        if (bytes.size() > 1024 || !in.U8(version) || version != 1 || !in.U32(value.Guild) || !value.Guild ||
            !in.U32(value.Language) || !Read64(in, value.Sender) || !value.Sender || !Read64(in, value.SenderIncarnation) ||
            !value.SenderIncarnation || !in.U8(value.ChatTag) || !in.U8(officer) || officer > 1 ||
            !in.String(value.SenderName, 48) || value.SenderName.empty() || !ReadRoutingBytes(in, value.Text, 511) ||
            !ReadRoutingBytes(in, value.Prefix, 16) || !in.End()) return false;
        if ((value.Language == 0xFFFFFFFFu) != !value.Prefix.empty() ||
            (value.Language != 0xFFFFFFFFu && !Cluster::ValidUtf8(value.Text))) return false;
        value.Officer = officer != 0; event = std::move(value); return true;
    }
}
#endif
