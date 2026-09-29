/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_CHAT_REMOTE_WHISPER_H
#define SKYFIRE_CHAT_REMOTE_WHISPER_H
#include "ChatService.h"
namespace Skyfire::Chat
{
    struct RemoteWhisper
    {
        std::uint64_t Token = 0, Sender = 0, Incarnation = 0;
        std::uint32_t Team = 0;
        std::uint8_t Level = 0, Flags = 0, ChatTag = 0;
        // Flags: GM, filter bypass, cross-faction permission, silence aura, addon, GM chat permission.
        std::string SenderName, ReceiverName, Text, Prefix;
    };
    enum class RemoteWhisperStatus : std::uint8_t { Delivered, NotFound, LowLevel, WrongFaction, Silenced };
    struct RemoteWhisperReply
    {
        std::uint64_t Token = 0, Receiver = 0;
        RemoteWhisperStatus Status = RemoteWhisperStatus::NotFound;
        std::uint8_t Level = 0, Flags = 0, ChatTag = 0; // GM, AFK, DND, GM-chat permission.
        std::string Name, AutoReply;
    };
    inline void WriteRemoteString(Cluster::Writer& out, std::string const& value)
    { WriteServiceBytes(out, {value.begin(), value.end()}); }
    inline bool ReadRemoteString(Cluster::Reader& in, std::string& value, std::size_t max, bool empty = false, bool binary = false)
    {
        std::vector<std::uint8_t> bytes;
        if (!ReadServiceBytes(in, bytes) || bytes.size() > max || (!empty && bytes.empty())) return false;
        value.assign(bytes.begin(), bytes.end()); return binary || value.empty() || Cluster::ValidUtf8(value);
    }
    inline Cluster::Writer EncodeRemoteWhisper(RemoteWhisper const& value)
    {
        Cluster::Writer out; out.U8(1); Write64(out, value.Token); Write64(out, value.Sender); Write64(out, value.Incarnation);
        out.U32(value.Team); out.U8(value.Level); out.U8(value.Flags); out.U8(value.ChatTag);
        WriteRemoteString(out, value.SenderName); WriteRemoteString(out, value.ReceiverName);
        WriteRemoteString(out, value.Text); WriteRemoteString(out, value.Prefix); return out;
    }
    inline bool DecodeRemoteWhisper(std::vector<std::uint8_t> const& bytes, RemoteWhisper& value)
    {
        if (bytes.size() > 1024) return false;
        Cluster::Reader in(bytes); std::uint8_t action;
        return in.U8(action) && action == 1 && Read64(in, value.Token) && Read64(in, value.Sender) && value.Sender &&
            Read64(in, value.Incarnation) && value.Incarnation && in.U32(value.Team) && in.U8(value.Level) && value.Level &&
            in.U8(value.Flags) && value.Flags <= 63 && in.U8(value.ChatTag) &&
            ReadRemoteString(in, value.SenderName, 48) && ReadRemoteString(in, value.ReceiverName, 48) &&
            ReadRemoteString(in, value.Text, 511, false, (value.Flags & 16) != 0) &&
            ReadRemoteString(in, value.Prefix, 16, true, true) && (((value.Flags & 16) != 0) == !value.Prefix.empty()) && in.End();
    }
    inline Cluster::Writer EncodeRemoteWhisperReply(RemoteWhisperReply const& value)
    {
        Cluster::Writer out; out.U8(2); Write64(out, value.Token); Write64(out, value.Receiver);
        out.U8(std::uint8_t(value.Status)); out.U8(value.Level); out.U8(value.Flags); out.U8(value.ChatTag);
        WriteRemoteString(out, value.Name); WriteRemoteString(out, value.AutoReply); return out;
    }
    inline bool DecodeRemoteWhisperReply(std::vector<std::uint8_t> const& bytes, RemoteWhisperReply& value)
    {
        if (bytes.size() > 1024) return false;
        Cluster::Reader in(bytes); std::uint8_t action, status;
        if (!in.U8(action) || action != 2 || !Read64(in, value.Token) || !value.Token || !Read64(in, value.Receiver) ||
            !value.Receiver || !in.U8(status) || status > 4 || !in.U8(value.Level) || !in.U8(value.Flags) || value.Flags > 15 || !in.U8(value.ChatTag) ||
            !ReadRemoteString(in, value.Name, 48) || !ReadRemoteString(in, value.AutoReply, 511, true) || !in.End()) return false;
        value.Status = RemoteWhisperStatus(status); return true;
    }
}
#endif
