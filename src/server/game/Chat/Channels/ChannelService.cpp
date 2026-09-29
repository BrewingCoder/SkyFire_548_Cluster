/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "Channel.h"
#include "ChannelMgr.h"
#include "Chat.h"
#include "Config.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SocialMgr.h"
#include "World.h"
#include "WorldSession.h"

bool Channel::ServiceEnabled()
{
    // Never downgrade an enabled authority to local SQL when its connection fails.
    static bool const enabled = sConfigMgr->GetBoolDefault("Chat.ChannelAuthority.Enable", false);
    return enabled;
}

bool Channel::ServiceCommand(Player const* player, Skyfire::Chat::ChannelAction action, std::string const& target,
    std::string const& password, std::string const& text, uint32 language, bool value, bool silent)
{
    using namespace Skyfire::Chat;
    if (!ServiceEnabled()) return false;
    if (!player || !player->GetSession()) return true;
    ChannelCommand command; command.Action=action; command.Name=_name; command.ChannelId=_channelId; command.Team=sWorld->GetBoolConfig(WorldBoolConfigs::CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHANNEL) ? ALLIANCE : _Team;
    command.ActorTeam=player->GetTeam(); command.Password=password; command.TargetName=target; command.Text=text;
    command.Language=sWorld->GetBoolConfig(WorldBoolConfigs::CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHANNEL) ? 0 : language;
    command.ChatTag=player->GetChatTag(); command.Value=value; command.Silent=silent || player->GetSession()->HasPermission(rbac::RBAC_PERM_SILENTLY_JOIN_CHANNEL);
    command.Override=player->GetSession()->HasPermission(action==ChannelAction::Ownership ? rbac::RBAC_PERM_COMMAND_CHANNEL_SET_OWNERSHIP : rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR);
    command.CrossFaction=player->GetSession()->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHANNEL);
    if (!target.empty())
    {
        if (Player* selected=sObjectAccessor->FindPlayerByName(target))
        {
            if (action==ChannelAction::Invite && !selected->isGMVisible())
            { WorldPacket data; MakePlayerNotFound(&data,target); SendToOne(&data,player->GetGUID()); return true; }
            command.Target=selected->GetGUID(); command.TargetTeam=selected->GetTeam();
            command.TargetCrossFaction=selected->GetSession()->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHANNEL);
        }
        else if (action==ChannelAction::Unban) command.Target=sObjectMgr->GetPlayerGUIDByName(target);
    }
    ServiceRequest request; request.Domain=ServiceDomain::Channel; request.Actor=player->GetGUID();
    request.Account=player->GetSession()->GetAccountId(); request.Incarnation=player->GetSession()->GetChatIncarnation();
    request.Payload=EncodeChannelCommand(command).Bytes;
    if (!QueueServiceRequest(std::move(request)) && !silent)
        player->GetSession()->SendNotification("Chat service unavailable. Channel operation was not executed.");
    return true;
}

void Channel::HandleServiceResult(Skyfire::Chat::ServiceResult const& result)
{
    using namespace Skyfire::Chat;
    if (result.Request.Domain!=ServiceDomain::Channel) return;
    Player* player=ObjectAccessor::FindPlayer(result.Request.Actor);
    if (!player || !player->GetSession() || player->GetSession()->GetChatIncarnation()!=result.Request.Incarnation ||
        player->GetSession()->GetAccountId()!=result.Request.Account) return;
    if (!result.Success || result.Response.Status!=ServiceStatus::Ok)
    {
        player->GetSession()->SendNotification("Chat service unavailable or request expired. Channel operation was not confirmed."); return;
    }
    ChannelUpdate update;
    if (!DecodeChannelUpdate(result.Response.Payload,update)) return;
    ChannelMgr* manager=ChannelMgr::forTeam(player->GetTeam());
    if (!manager) return;
    manager->setTeam(update.Team);
    if (Channel* channel=manager->GetJoinChannel(update.Name,update.ChannelId)) channel->ApplyServiceUpdate(player,update);
}

void Channel::HandleServiceEvent(Skyfire::Chat::ServiceEvent const& event)
{
    using namespace Skyfire::Chat;
    if (event.Domain!=ServiceDomain::Channel) return;
    Player* player=ObjectAccessor::FindPlayer(event.Recipient);
    if (!player || !player->GetSession() || player->GetSession()->GetChatIncarnation()!=event.Incarnation) return;
    ChannelUpdate update;
    if (!DecodeChannelUpdate(event.Payload,update)) return;
    if(update.Action==ChannelAction::InviteCheck)
    {
        ChannelCommand reply; reply.Action=ChannelAction::InviteDecision; reply.Name=update.Name;
        reply.ChannelId=update.ChannelId; reply.Team=update.Team; reply.Text=update.Text;
        reply.ActorTeam=player->GetTeam(); reply.CrossFaction=player->GetSession()->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHANNEL);
        reply.Value=player->isGMVisible();
        ServiceRequest request; request.Domain=ServiceDomain::Channel; request.Actor=player->GetGUID();
        request.Account=player->GetSession()->GetAccountId(); request.Incarnation=player->GetSession()->GetChatIncarnation();
        request.Payload=EncodeChannelCommand(reply).Bytes; QueueServiceRequest(std::move(request)); return;
    }
    ChannelMgr* manager=ChannelMgr::forTeam(player->GetTeam());
    if (!manager) return;
    manager->setTeam(update.Team);
    if (Channel* channel=manager->GetJoinChannel(update.Name,update.ChannelId)) channel->ApplyServiceUpdate(player,update);
}

void Channel::ApplyServiceUpdate(Player* recipient,Skyfire::Chat::ChannelUpdate const& update)
{
    using namespace Skyfire::Chat;
    _Team=update.Team;
    if (update.Revision < _serviceRevision && !update.Error)
    {
        // A later member's event may arrive before our join response. Preserve
        // the successful client acknowledgement if we are still a member, but
        // never roll the projection back or acknowledge an already-ended join.
        if (update.Action==ChannelAction::Join && update.Actor==recipient->GetGUID() && IsOn(recipient->GetGUID()))
        {
            for(auto const& notice:update.Notices) if(notice.Type==CHAT_YOU_JOINED_NOTICE)
            { WorldPacket joined; MakeYouJoined(&joined); SendToOne(&joined,recipient->GetGUID()); break; }
        }
        return;
    }
    bool changed=update.Revision>_serviceRevision;
    if (changed || update.Snapshot)
    {
        // The daemon's snapshot is authoritative; these are packet/visibility projections only.
        if (update.Snapshot && update.Revision>=_serviceRevision) playersStore.clear();
        if (update.Revision>=_serviceRevision)
        {
            for (auto& item:playersStore) item.second.SetOwner(false);
            for (auto const& item:update.Members)
            { PlayerInfo info; info.player=item.Guid; info.flags=item.Flags; info.security=item.Security; info.profileKnown=item.ProfileKnown; info.visible=item.Visible; playersStore[item.Guid]=info; }
            if (update.Removed) playersStore.erase(update.Removed);
            _ownerGUID=update.Owner; _announce=update.Announce; _ownership=update.Ownership; _serviceRevision=update.Revision;
        }
    }
    // Another local recipient's delta can populate this GUID before its own
    // reply arrives. Derive the Player linkage from the authoritative projection,
    // not from whether this shared Channel already happened to contain the GUID.
    recipient->LeftChannel(this);
    if (IsOn(recipient->GetGUID())) recipient->JoinedChannel(this);
    if (changed && !update.Error)
    {
        if (update.Action==ChannelAction::Join) JoinNotify(update.Actor,GetChannelId(),GetFlags(),GetPlayerFlags(update.Actor),GetName());
        if (update.Removed) LeaveNotify(update.Removed,GetChannelId(),GetFlags(),GetName());
    }
    WorldPacket data;
    if (update.Error)
    {
        switch(update.Error)
        {
            case 1: MakeNotMember(&data); break;
            case 2: if (IsConstant()) return; MakePlayerAlreadyMember(&data,update.Actor); break;
            case 3: MakeBanned(&data); break;
            case 4: MakeWrongPassword(&data); break;
            case 5: MakeNotModerator(&data); break;
            case 6: MakeNotOwner(&data); break;
            case 7: if(update.Action==ChannelAction::Unban) MakePlayerNotBanned(&data,update.TargetName); else MakePlayerNotFound(&data,update.TargetName); break;
            case 8: MakeInviteWrongFaction(&data); break;
            case 9: MakeMuted(&data); break;
            default: MakeThrottled(&data); break;
        }
        SendToOne(&data,recipient->GetGUID()); return;
    }
    for(auto const& notice:update.Notices)
    {
        data.clear();
        switch(notice.Type)
        {
            case CHAT_JOINED_NOTICE: MakeJoined(&data,notice.Actor); break;
            case CHAT_LEFT_NOTICE: MakeLeft(&data,notice.Actor); break;
            case CHAT_YOU_JOINED_NOTICE: MakeYouJoined(&data); break;
            case CHAT_YOU_LEFT_NOTICE: MakeYouLeft(&data); break;
            case CHAT_PASSWORD_CHANGED_NOTICE: MakePasswordChanged(&data,notice.Actor); break;
            case CHAT_OWNER_CHANGED_NOTICE: MakeOwnerChanged(&data,notice.Target); break;
            case CHAT_CHANNEL_OWNER_NOTICE:
                MakeNotifyPacket(&data,CHAT_CHANNEL_OWNER_NOTICE);
                data << ((IsConstant() || !update.Owner)?std::string("Nobody"):update.TargetName); break;
            case CHAT_MODE_CHANGE_NOTICE: MakeNotifyPacket(&data,CHAT_MODE_CHANGE_NOTICE); data << uint64(notice.Target) << uint8(notice.OldFlags) << uint8(notice.NewFlags); break;
            case CHAT_ANNOUNCEMENTS_ON_NOTICE: MakeAnnouncementsOn(&data,notice.Actor); break;
            case CHAT_ANNOUNCEMENTS_OFF_NOTICE: MakeAnnouncementsOff(&data,notice.Actor); break;
            case CHAT_PLAYER_KICKED_NOTICE: MakePlayerKicked(&data,notice.Target,notice.Actor); break;
            case CHAT_PLAYER_BANNED_NOTICE: MakePlayerBanned(&data,notice.Target,notice.Actor); break;
            case CHAT_PLAYER_UNBANNED_NOTICE: MakePlayerUnbanned(&data,notice.Target,notice.Actor); break;
            case CHAT_INVITE_NOTICE:
                if (!recipient->isGMVisible() || recipient->GetSocial()->HasIgnore(GUID_LOPART(notice.Actor)) ||
                    (recipient->GetTeam()!=update.ActorTeam && !(update.ActorCrossFaction && recipient->GetSession()->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHANNEL)))) continue;
                MakeInvite(&data,notice.Actor); break;
            case CHAT_PLAYER_INVITED_NOTICE: MakePlayerInvited(&data,update.TargetName); break;
            default: continue;
        }
        SendToOne(&data,recipient->GetGUID());
    }
    if (update.Action==ChannelAction::Ownership && update.Snapshot)
        recipient->GetSession()->SendNotification("Channel ownership setting updated.");
    if (update.Action==ChannelAction::List && update.Snapshot)
    {
        data.Initialize(SMSG_CHANNEL_LIST); data << uint8(1) << GetName() << uint8(GetFlags());
        auto position=data.wpos(); data << uint32(0); uint32 count=0;
        for(auto const& item:playersStore)
        {
            Player* member=ObjectAccessor::FindPlayer(item.first);
            uint8 security=member?uint8(member->GetSession()->GetSecurity()):item.second.security;
            bool visible=member?member->IsVisibleGloballyFor(recipient):(item.second.profileKnown &&
                (item.second.visible || (recipient->GetSession()->GetSecurity()>AccountTypes::SEC_PLAYER && security<=uint8(recipient->GetSession()->GetSecurity()))));
            if (visible && (recipient->GetSession()->HasPermission(rbac::RBAC_PERM_WHO_SEE_ALL_SEC_LEVELS) ||
                security<=sWorld->getIntConfig(WorldIntConfigs::CONFIG_GM_LEVEL_IN_WHO_LIST)))
            { data << uint64(item.first) << uint8(item.second.flags); ++count; }
        }
        data.put<uint32>(position,count); SendToOne(&data,recipient->GetGUID());
    }
    if (update.Action==ChannelAction::Speak && !update.Text.empty() && IsOn(recipient->GetGUID()))
    {
        auto sender=playersStore.find(update.Actor);
        if(sender==playersStore.end() || sender->second.IsMuted() || (!sender->second.IsModerator() && recipient->GetSocial()->HasIgnore(GUID_LOPART(update.Actor)))) return;
        Player* player=ObjectAccessor::FindPlayer(update.Actor);
        if (player && !player->CanSpeak()) return;
        ChatHandler::BuildChatPacket(data,ChatMsg::CHAT_MSG_CHANNEL,Language(update.Language),update.Actor,update.Actor,update.Text,
            player?player->GetChatTag():update.ChatTag,update.TargetName,"",0,false,_name);
        SendToOne(&data,recipient->GetGUID());
    }
}
