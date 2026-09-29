/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_CHAT_GROUP_RELAY_H
#define SKYFIRE_CHAT_GROUP_RELAY_H
#include "ChatRemoteWhisper.h"
#include "ChatRouting.h"
namespace Skyfire::Chat
{
    struct GroupRelay
    {
        std::uint64_t Group = 0, Sender = 0, Incarnation = 0, Ignore = 0;
        AudienceKind Kind = AudienceKind::Party;
        std::uint32_t Language = 0;
        std::uint8_t Subgroup = 255, ChatTag = 0;
        bool IgnoreOtherGroups = false, GM = false;
        std::string Name, Text, Prefix;
        std::vector<std::uint64_t> Members;
    };
    inline Cluster::Writer EncodeGroupRelay(GroupRelay const& value)
    {
        Cluster::Writer out; Write64(out, value.Group); Write64(out, value.Sender); Write64(out, value.Incarnation); Write64(out, value.Ignore);
        out.U8(std::uint8_t(value.Kind)); out.U32(value.Language); out.U8(value.Subgroup); out.U8(value.ChatTag); out.U8((value.IgnoreOtherGroups ? 1 : 0) | (value.GM ? 2 : 0));
        WriteRemoteString(out, value.Name); WriteRemoteString(out, value.Text); WriteRemoteString(out, value.Prefix);
        out.U16(std::uint16_t(value.Members.size())); for (auto guid : value.Members) Write64(out, guid); return out;
    }
    inline bool DecodeGroupRelay(std::vector<std::uint8_t> const& bytes, GroupRelay& value)
    {
        if (bytes.size() > 2048) return false;
        Cluster::Reader in(bytes); std::uint8_t kind, ignore; std::uint16_t count;
        if (!Read64(in, value.Group) || !value.Group || !Read64(in, value.Sender) || !value.Sender ||
            !Read64(in, value.Incarnation) || !value.Incarnation || !Read64(in, value.Ignore) ||
            !in.U8(kind) || kind < std::uint8_t(AudienceKind::Party) || kind > std::uint8_t(AudienceKind::Instance) ||
            !in.U32(value.Language) || !in.U8(value.Subgroup) || (value.Subgroup > 7 && value.Subgroup != 255) ||
            !in.U8(value.ChatTag) || !in.U8(ignore) || ignore > 3 || !ReadRemoteString(in, value.Name, 48) ||
            !ReadRemoteString(in, value.Text, 511, false, value.Language == 0xffffffffu) ||
            !ReadRemoteString(in, value.Prefix, 16, true, true) ||
            ((value.Language == 0xffffffffu) != !value.Prefix.empty()) || !in.U16(count) || !count || count > 40) return false;
        value.Kind = AudienceKind(kind); value.IgnoreOtherGroups = (ignore & 1) != 0; value.GM = (ignore & 2) != 0; value.Members.clear();
        std::set<std::uint64_t> members;
        for (unsigned i = 0; i < count; ++i) { std::uint64_t guid; if (!Read64(in, guid) || !guid || !members.insert(guid).second) return false; value.Members.push_back(guid); }
        return members.count(value.Sender) != 0 && in.End();
    }
}
#endif
