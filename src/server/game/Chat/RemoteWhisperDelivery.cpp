/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "RemoteWhisperDelivery.h"
#include "Cluster/ChatRemoteWhisper.h"
#include "Chat.h"
#include "Language.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <map>
#include <mutex>
namespace Skyfire::Chat::RemoteDelivery
{
    namespace
    {
        struct Pending { RemoteWhisper Message; std::uint32_t Account; std::chrono::steady_clock::time_point Deadline; };
        std::map<std::uint64_t, Pending> PendingWhispers;
        std::mutex Lock;
        Player* Current(Pending const& pending)
        {
            Player* player = ObjectAccessor::FindPlayer(pending.Message.Sender);
            return player && player->IsInWorld() && player->GetSession() && player->GetSession()->GetAccountId() == pending.Account &&
                player->GetSession()->GetChatIncarnation() == pending.Message.Incarnation ? player : nullptr;
        }
    }
    bool Send(Player* sender, std::string const& name, std::string const& text, std::string const& prefix)
    {
        if (!WhispersEnabled()) return false;
        if (!sender || !sender->IsInWorld() || !sender->GetSession()) return true;
        auto* session = sender->GetSession();
        if (prefix.empty() && !sender->CanSpeak()) { session->SendNotification("You cannot send whispers while muted."); return true; }
        RemoteWhisper message; message.Sender = sender->GetGUID(); message.Incarnation = session->GetChatIncarnation();
        message.SenderName = sender->GetName(); message.ReceiverName = name; message.Text = text; message.Prefix = prefix;
        message.Team = sender->GetTeam(); message.Level = sender->getLevel(); message.ChatTag = sender->GetChatTag();
        if (sender->IsGameMaster()) message.Flags |= 1;
        if (sWorld->GetBoolConfig(WorldBoolConfigs::CONFIG_CHAT_GM_WHISPER_FILTER_BYPASS) && session->GetSecurity() > AccountTypes::SEC_PLAYER) message.Flags |= 2;
        if (session->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHAT)) message.Flags |= 4;
        if (sender->HasAura(1852)) message.Flags |= 8;
        if (!prefix.empty()) message.Flags |= 16;
        if (session->HasPermission(rbac::RBAC_PERM_COMMAND_GM_CHAT)) message.Flags |= 32;
        // A remote player has no local Player object. Use the sender-only script hook.
        sScriptMgr->OnPlayerChat(sender, ChatMsg::CHAT_MSG_WHISPER, prefix.empty() ? Language::LANG_UNIVERSAL : Language::LANG_ADDON, message.Text);
        ServiceRequest request; request.Domain = ServiceDomain::Whisper; request.Account = session->GetAccountId();
        request.Actor = message.Sender; request.Incarnation = message.Incarnation; request.Payload = EncodeRemoteWhisper(message).Bytes;
        RemoteWhisper decoded;
        std::lock_guard<std::mutex> lock(Lock);
        auto sequence = PendingWhispers.size() < 128 && DecodeRemoteWhisper(request.Payload, decoded) ? QueueServiceRequest(std::move(request)) : 0;
        if (!sequence) { session->SendNotification("Chat service unavailable or busy. Whisper was not sent."); return true; }
        PendingWhispers.emplace(sequence, Pending{std::move(message), session->GetAccountId(), std::chrono::steady_clock::now() + std::chrono::seconds(10)});
        return true;
    }
    void Complete(ServiceResult const& result)
    {
        if (result.Request.Payload.empty() || result.Request.Payload[0] != 1) return; // Receiver acknowledgments have no sender UI.
        std::lock_guard<std::mutex> lock(Lock);
        auto pending = PendingWhispers.find(result.Request.Sequence);
        if (pending == PendingWhispers.end() || (result.Success && result.Response.Status == ServiceStatus::Ok)) return;
        if (auto* sender = Current(pending->second))
        {
            if (result.Success && result.Response.Status == ServiceStatus::Rejected)
            { if (!(pending->second.Message.Flags & 16)) sender->GetSession()->SendPlayerNotFoundNotice(pending->second.Message.ReceiverName); }
            else sender->GetSession()->SendNotification("Chat service unavailable or request expired. Whisper delivery was not confirmed.");
        }
        PendingWhispers.erase(pending);
    }
    void Receive(ServiceEvent const& event)
    {
        Player* receiver = ObjectAccessor::FindPlayer(event.Recipient);
        if (!receiver || !receiver->IsInWorld() || !receiver->GetSession() || receiver->GetSession()->GetChatIncarnation() != event.Incarnation || event.Payload.empty()) return;
        auto* session = receiver->GetSession();
        if (event.Payload[0] == 2)
        {
            RemoteWhisperReply reply; if (!DecodeRemoteWhisperReply(event.Payload, reply)) return;
            std::lock_guard<std::mutex> lock(Lock);
            auto pending = PendingWhispers.find(reply.Token);
            if (pending == PendingWhispers.end() || Current(pending->second) != receiver) return;
            auto const message = pending->second.Message; PendingWhispers.erase(pending);
            if (message.Flags & 16) return; // Addon delivery has no player-facing inform/error packet.
            switch (reply.Status)
            {
                case RemoteWhisperStatus::NotFound: session->SendPlayerNotFoundNotice(message.ReceiverName); return;
                case RemoteWhisperStatus::LowLevel: session->SendNotification(session->GetSkyFireString(LANG_WHISPER_REQ), sWorld->getIntConfig(WorldIntConfigs::CONFIG_CHAT_WHISPER_LEVEL_REQ)); return;
                case RemoteWhisperStatus::WrongFaction: session->SendWrongFactionNotice(); return;
                case RemoteWhisperStatus::Silenced: session->SendNotification(session->GetSkyFireString(LANG_GM_SILENCE), receiver->GetName().c_str()); return;
                case RemoteWhisperStatus::Delivered: break;
            }
            WorldPacket packet;
            ChatHandler::BuildChatPacket(packet, ChatMsg::CHAT_MSG_WHISPER_INFORM, Language::LANG_UNIVERSAL,
                ObjectGuid(reply.Receiver), ObjectGuid(reply.Receiver), message.Text, reply.ChatTag, reply.Name, reply.Name, 0, (reply.Flags & 8) != 0);
            session->SendPacket(&packet);
            if (reply.Level < sWorld->getIntConfig(WorldIntConfigs::CONFIG_CHAT_WHISPER_LEVEL_REQ) ||
                (session->HasPermission(rbac::RBAC_PERM_CAN_FILTER_WHISPERS) && !receiver->isAcceptWhispers())) receiver->AddWhisperWhiteList(reply.Receiver);
            if (!receiver->isAcceptWhispers() && !receiver->IsGameMaster() && !(reply.Flags & 1))
            { receiver->SetAcceptWhispers(true); ChatHandler(session).SendSysMessage(LANG_COMMAND_WHISPERON); }
            if (reply.Flags & 2) ChatHandler(session).PSendSysMessage(LANG_PLAYER_AFK, reply.Name.c_str(), reply.AutoReply.c_str());
            else if (reply.Flags & 4) ChatHandler(session).PSendSysMessage(LANG_PLAYER_DND, reply.Name.c_str(), reply.AutoReply.c_str());
            return;
        }
        RemoteWhisper message; if (!DecodeRemoteWhisper(event.Payload, message) || !message.Token || message.ReceiverName != receiver->GetName()) return;
        RemoteWhisperReply reply; reply.Token = message.Token; reply.Receiver = receiver->GetGUID(); reply.Name = receiver->GetName(); reply.Level = receiver->getLevel();
        reply.Flags = (receiver->IsGameMaster() ? 1 : 0) | (receiver->isAFK() ? 2 : receiver->isDND() ? 4 : 0) |
            (session->HasPermission(rbac::RBAC_PERM_COMMAND_GM_CHAT) ? 8 : 0);
        reply.ChatTag = receiver->GetChatTag();
        if (reply.Flags & 6)
        {
            reply.AutoReply = receiver->autoReplyMsg.substr(0, 511);
            while (!reply.AutoReply.empty() && !Cluster::ValidUtf8(reply.AutoReply)) reply.AutoReply.pop_back();
        }
        bool const addon = (message.Flags & 16) != 0;
        bool const whitelist = receiver->IsInWhisperWhiteList(message.Sender);
        if ((!receiver->isAcceptWhispers() && !(message.Flags & 2) && !whitelist) || receiver->GetSocial()->HasIgnore(GUID_LOPART(message.Sender)) ||
            (addon && !session->IsAddonRegistered(message.Prefix))) reply.Status = RemoteWhisperStatus::NotFound;
        else if (!addon && !(message.Flags & 1) && message.Level < sWorld->getIntConfig(WorldIntConfigs::CONFIG_CHAT_WHISPER_LEVEL_REQ) && !whitelist) reply.Status = RemoteWhisperStatus::LowLevel;
        else if (message.Team != receiver->GetTeam() && !(message.Flags & 4) && !whitelist) reply.Status = RemoteWhisperStatus::WrongFaction;
        else if ((message.Flags & 8) && !receiver->IsGameMaster()) reply.Status = RemoteWhisperStatus::Silenced;
        else
        {
            WorldPacket packet;
            ChatHandler::BuildChatPacket(packet, ChatMsg::CHAT_MSG_WHISPER, addon ? Language::LANG_ADDON : Language::LANG_UNIVERSAL,
                ObjectGuid(message.Sender), ObjectGuid(message.Sender), message.Text, message.ChatTag, message.SenderName, message.SenderName,
                0, (message.Flags & 32) != 0, "", message.Prefix);
            session->SendPacket(&packet); reply.Status = RemoteWhisperStatus::Delivered;
        }
        ServiceRequest request; request.Domain = ServiceDomain::Whisper; request.Account = session->GetAccountId(); request.Actor = receiver->GetGUID();
        request.Incarnation = session->GetChatIncarnation(); request.Payload = EncodeRemoteWhisperReply(reply).Bytes;
        QueueServiceRequest(std::move(request)); // No retry: the client packet may already have been delivered.
    }
    void Update()
    {
        std::lock_guard<std::mutex> lock(Lock);
        auto now = std::chrono::steady_clock::now();
        for (auto it = PendingWhispers.begin(); it != PendingWhispers.end();)
            if (now >= it->second.Deadline)
            {
                if (auto* sender = Current(it->second)) sender->GetSession()->SendNotification("Chat service request expired. Whisper delivery was not confirmed.");
                it = PendingWhispers.erase(it);
            }
            else ++it;
    }
}
