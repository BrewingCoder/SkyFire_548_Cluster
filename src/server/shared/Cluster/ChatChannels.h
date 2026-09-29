/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_CHAT_CHANNELS_H
#define SKYFIRE_CHAT_CHANNELS_H
#include "ChatPresence.h"
namespace Skyfire::Chat
{
    enum class ChannelAction : std::uint8_t { Join, Leave, Kick, Ban, Unban, Password, Announce, Moderator, Mute, Owner, QueryOwner, List, Invite, Speak, Ownership, InviteDecision, InviteCheck };
    struct ChannelCommand
    {
        ChannelAction Action = ChannelAction::Join;
        std::string Name, Password, TargetName, Text;
        std::uint32_t ChannelId = 0, Team = 0, ActorTeam = 0, TargetTeam = 0, Language = 0;
        std::uint64_t Target = 0;
        std::uint8_t ChatTag = 0;
        bool Override = false, Silent = false, CrossFaction = false, TargetCrossFaction = false, Value = false;
    };
    struct ChannelMemberView { std::uint64_t Guid = 0, Incarnation = 0; std::uint8_t Flags = 0, Security = 0; bool ProfileKnown = false, Visible = false; };
    struct ChannelNotice { std::uint8_t Type = 0, OldFlags = 0, NewFlags = 0; std::uint64_t Actor = 0, Target = 0; };
    struct ChannelUpdate
    {
        std::string Name, TargetName, Text;
        std::uint32_t ChannelId = 0, Team = 0, Language = 0, ActorTeam = 0;
        std::uint64_t Revision = 0, Owner = 0, Actor = 0, Removed = 0;
        std::uint8_t Error = 0, ChatTag = 0;
        ChannelAction Action = ChannelAction::Join;
        bool Snapshot = false, Announce = true, Ownership = true, ActorCrossFaction = false;
        std::vector<ChannelMemberView> Members;
        std::vector<ChannelNotice> Notices;
    };
    inline void ChannelText(Cluster::Writer& out, std::string const& text)
    { out.U8(!text.empty()); if (!text.empty()) out.String(text); }
    inline bool ChannelText(Cluster::Reader& in, std::string& text, std::size_t maximum)
    { std::uint8_t present; if (!in.U8(present) || present > 1) return false; text.clear(); return !present || in.String(text, maximum); }
    inline Cluster::Writer EncodeChannelCommand(ChannelCommand const& value)
    {
        Cluster::Writer out; out.U8(std::uint8_t(value.Action)); out.String(value.Name);
        out.U32(value.ChannelId); out.U32(value.Team); out.U32(value.ActorTeam); out.U32(value.TargetTeam); out.U32(value.Language);
        Write64(out,value.Target); out.U8(std::uint8_t(value.Override) | value.Silent << 1 | value.CrossFaction << 2 | value.TargetCrossFaction << 3 | value.Value << 4);
        ChannelText(out,value.Password); ChannelText(out,value.TargetName); ChannelText(out,value.Text); out.U8(value.ChatTag); return out;
    }
    inline bool DecodeChannelCommand(std::vector<std::uint8_t> const& bytes, ChannelCommand& value)
    {
        if (bytes.size() > 2048) return false;
        Cluster::Reader in(bytes); ChannelCommand decoded; std::uint8_t action, flags;
        if (!in.U8(action) || action > std::uint8_t(ChannelAction::InviteCheck) || !in.String(decoded.Name,127) ||
            !in.U32(decoded.ChannelId) || !in.U32(decoded.Team) || !in.U32(decoded.ActorTeam) || !in.U32(decoded.TargetTeam) || !in.U32(decoded.Language) ||
            !Read64(in,decoded.Target) || !in.U8(flags) || flags > 31 || !ChannelText(in,decoded.Password,127) ||
            !ChannelText(in,decoded.TargetName,48) || !ChannelText(in,decoded.Text,511)) return false;
        if (!in.End() && (!in.U8(decoded.ChatTag) || !in.End())) return false;
        decoded.Action=ChannelAction(action); decoded.Override=flags&1; decoded.Silent=flags&2;
        decoded.CrossFaction=flags&4; decoded.TargetCrossFaction=flags&8; decoded.Value=flags&16;
        value=std::move(decoded); return true;
    }
    inline Cluster::Writer EncodeChannelUpdate(ChannelUpdate const& value)
    {
        Cluster::Writer out; out.String(value.Name); out.U32(value.ChannelId); out.U32(value.Team); out.U32(value.Language); out.U32(value.ActorTeam);
        Write64(out,value.Revision); Write64(out,value.Owner); Write64(out,value.Actor); Write64(out,value.Removed);
        out.U8(value.Error); out.U8(std::uint8_t(value.Action)); out.U8(std::uint8_t(value.Snapshot) | value.Announce << 1 | value.Ownership << 2 | value.ActorCrossFaction << 3);
        ChannelText(out,value.TargetName); ChannelText(out,value.Text); out.U16(std::uint16_t(value.Members.size()));
        for (auto const& item : value.Members) { Write64(out,item.Guid); Write64(out,item.Incarnation); out.U8(item.Flags); out.U8(item.Security); out.U8(std::uint8_t(item.ProfileKnown) | item.Visible << 1); }
        out.U16(std::uint16_t(value.Notices.size()));
        for (auto const& item : value.Notices) { out.U8(item.Type); Write64(out,item.Actor); Write64(out,item.Target); out.U8(item.OldFlags); out.U8(item.NewFlags); }
        out.U8(value.ChatTag); return out;
    }
    inline bool DecodeChannelUpdate(std::vector<std::uint8_t> const& bytes, ChannelUpdate& value)
    {
        if (bytes.size() > 128*1024) return false;
        Cluster::Reader in(bytes); ChannelUpdate decoded; std::uint8_t action,flags; std::uint16_t count;
        if (!in.String(decoded.Name,127) || !in.U32(decoded.ChannelId) || !in.U32(decoded.Team) || !in.U32(decoded.Language) || !in.U32(decoded.ActorTeam) ||
            !Read64(in,decoded.Revision) || !Read64(in,decoded.Owner) || !Read64(in,decoded.Actor) || !Read64(in,decoded.Removed) ||
            !in.U8(decoded.Error) || decoded.Error > 12 || !in.U8(action) || action > std::uint8_t(ChannelAction::InviteCheck) ||
            !in.U8(flags) || flags > 15 || !ChannelText(in,decoded.TargetName,48) || !ChannelText(in,decoded.Text,511) || !in.U16(count) || count >4096) return false;
        decoded.Action=ChannelAction(action); decoded.Snapshot=flags&1; decoded.Announce=flags&2; decoded.Ownership=flags&4; decoded.ActorCrossFaction=flags&8;
        std::set<std::uint64_t> guids;
        while (count--) { ChannelMemberView item; std::uint8_t profile; if (!Read64(in,item.Guid) || !item.Guid || !Read64(in,item.Incarnation) || !item.Incarnation || !in.U8(item.Flags) || (item.Flags & ~11) || !in.U8(item.Security) || item.Security>4 || !in.U8(profile) || profile>3 || !guids.insert(item.Guid).second) return false; item.ProfileKnown=profile&1; item.Visible=profile&2; decoded.Members.push_back(item); }
        if (!in.U16(count) || count >16) return false;
        while (count--) { ChannelNotice item; if (!in.U8(item.Type) || !Read64(in,item.Actor) || !Read64(in,item.Target) || !in.U8(item.OldFlags) || !in.U8(item.NewFlags)) return false; decoded.Notices.push_back(item); }
        if (!in.End() && (!in.U8(decoded.ChatTag) || !in.End())) return false; value=std::move(decoded); return true;
    }
}
#endif
