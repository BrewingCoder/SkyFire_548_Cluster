/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "ChannelAuthority.h"
#include "ChannelState.h"
#include "Cluster/ChatChannels.h"
#include "Util.h"
#include <boost/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <chrono>
#include <future>
#include <deque>
#include <iomanip>
#include <sstream>

namespace Skyfire::Chat
{
    namespace
    {
        std::string Hex(unsigned char const* bytes, std::size_t count)
        { std::ostringstream result; result << std::hex << std::setfill('0'); for(std::size_t i=0;i<count;++i) result << std::setw(2) << unsigned(bytes[i]); return result.str(); }
        bool Unhex(std::string const& text, unsigned char* out, std::size_t count)
        {
            if(text.size()!=2*count || text.find_first_not_of("0123456789abcdef")!=std::string::npos) return false;
            auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
            for(std::size_t i=0;i<count;++i) out[i]=std::uint8_t(digit(text[2*i])*16+digit(text[2*i+1])); return true;
        }
        std::string Normalize(std::string const& name)
        { std::wstring wide; std::string result; if(!Utf8toWStr(name,wide)) return {}; wstrToLower(wide); WStrToUtf8(wide,result); return result; }
        std::string Key(std::uint32_t team,std::string const& name)
        { auto source=std::to_string(team)+":"+Normalize(name); unsigned char bytes[SHA256_DIGEST_LENGTH]; SHA256(reinterpret_cast<unsigned char const*>(source.data()),source.size(),bytes); return "channel-"+Hex(bytes,sizeof(bytes)); }
        struct CryptoResult { bool Success=false, Match=false; std::string Verifier; };
        CryptoResult Password(std::string const& password,std::string const& verifier,bool create)
        {
            CryptoResult result; unsigned char salt[16], digest[32]; int iterations=210000;
            if(create)
            {
                if(password.empty()) { result.Success=true; result.Match=true; return result; }
                if(RAND_bytes(salt,sizeof(salt))!=1) return result;
            }
            else
            {
                if(verifier.empty()) { result.Success=true; result.Match=true; return result; }
                std::vector<std::string> parts; std::istringstream input(verifier); std::string part;
                while(std::getline(input,part,'$')) parts.push_back(part);
                if(parts.size()!=4 || parts[0]!="pbkdf2-sha256" || !Unhex(parts[2],salt,sizeof(salt))) return result;
                try { iterations=std::stoi(parts[1]); } catch(...) { return result; }
                if(iterations<10000 || iterations>2000000) return result;
                unsigned char expected[32]; if(!Unhex(parts[3],expected,sizeof(expected))) return result;
                if(PKCS5_PBKDF2_HMAC(password.data(),int(password.size()),salt,sizeof(salt),iterations,EVP_sha256(),sizeof(digest),digest)!=1) return result;
                result.Success=true; result.Match=CRYPTO_memcmp(expected,digest,sizeof(digest))==0; return result;
            }
            if(PKCS5_PBKDF2_HMAC(password.data(),int(password.size()),salt,sizeof(salt),iterations,EVP_sha256(),sizeof(digest),digest)!=1) return result;
            result.Success=true; result.Match=true; result.Verifier="pbkdf2-sha256$210000$"+Hex(salt,sizeof(salt))+"$"+Hex(digest,sizeof(digest)); return result;
        }
    }
    struct ChannelAuthority::Impl
    {
        struct Identity { std::string Node,Generation,Name; std::uint32_t Account=0; };
        struct Pending
        {
            ServiceRequest Request; ChannelCommand Command; Completion Complete; ChannelState Candidate;
            ChannelState::Result Result; std::map<std::uint64_t,Identity> Identities;
            std::future<CryptoResult> Crypto; std::string Receipt; std::uint64_t Started=0; bool Saving=false, Inviting=false, Durable=false; std::uint64_t InviteIncarnation=0; std::string InviteToken;
        };
        struct Queued { std::string Node; ServiceRequest Request; Completion Complete; std::uint64_t Started=0; };
        struct Entry
        {
            ChannelState State; std::map<std::uint64_t,Identity> Identities;
            std::string Name,Key; std::uint32_t Realm=0,Team=0,Id=0; std::uint64_t Revision=0,StoredRevision=0;
            bool Fenced=false; std::uint64_t RestoreGrace=0; std::unique_ptr<Pending> Work; std::deque<Queued> Waiting;
        };
        PresenceDirectory& Presence; SocialPersistence& Persistence; Emit EmitEvent;
        std::map<std::pair<std::uint32_t,std::string>,Entry> Entries; std::uint64_t Now=0, LastExpire=0;
        std::map<std::uint32_t,std::uint64_t> Epochs;
        Impl(PresenceDirectory& presence,SocialPersistence& persistence,Emit emit):Presence(presence),Persistence(persistence),EmitEvent(std::move(emit)){}
        void ObserveOwnership(std::uint32_t realm)
        {
            auto epoch=Persistence.OwnershipEpoch(realm);
            auto previous=Epochs.find(realm);
            if(previous!=Epochs.end() && previous->second==epoch) return;
            Epochs[realm]=epoch;
            for(auto it=Entries.begin();it!=Entries.end();)
            {
                if(it->second.Realm!=realm) { ++it; continue; }
                auto& entry=it->second;
                // Never publish an old owner's candidate or invitation. New
                // ownership reloads the committed document with presence grace.
                if(entry.Work && !entry.Work->Inviting)
                    entry.Work->Complete({entry.Work->Request.Sequence,entry.Work->Saving?ServiceStatus::Unknown:ServiceStatus::Unavailable,{}});
                for(auto& queued:entry.Waiting) queued.Complete({queued.Request.Sequence,ServiceStatus::Unavailable,{}});
                it=Entries.erase(it);
            }
        }
        bool Load(Entry& entry)
        {
            SocialRecord record;
            if(!Persistence.Read(entry.Realm,"channels",entry.Key,record)) return true;
            try
            {
                auto const& doc=record.Document.as_object();
                if(std::uint32_t(doc.at("team").as_int64())!=entry.Team || std::uint32_t(doc.at("channel_id").as_int64())!=entry.Id) return false;
                entry.State.Announce=doc.at("announce").as_bool(); entry.State.Ownership=doc.at("ownership").as_bool();
                entry.State.PasswordVerifier=std::string(doc.at("password_verifier").as_string());
                for(auto const& ban:doc.at("bans").as_array()) entry.State.Bans.insert(boost::json::value_to<std::uint64_t>(ban));
                if(auto runtime=doc.if_contains("runtime"))
                {
                    auto const& state=runtime->as_object(); entry.State.Owner=boost::json::value_to<std::uint64_t>(state.at("owner"));
                    for(auto const& value:state.at("members").as_array())
                    {
                        auto const& member=value.as_object(); auto guid=boost::json::value_to<std::uint64_t>(member.at("guid"));
                        entry.State.Members[guid]={boost::json::value_to<std::uint64_t>(member.at("incarnation")),std::uint32_t(member.at("team").as_int64()),
                            std::uint8_t(member.at("flags").as_int64()),member.at("cross_faction").as_bool()};
                        entry.Identities[guid]={std::string(member.at("node").as_string()),std::string(member.at("generation").as_string()),
                            std::string(member.at("name").as_string()),std::uint32_t(member.at("account").as_int64())};
                    }
                    entry.RestoreGrace=Now+PresenceLeaseMs;
                }
                entry.StoredRevision=entry.Revision=record.Revision; return true;
            }
            catch(...) { return false; }
        }
        boost::json::value Document(Entry const& entry,ChannelState const& state,std::map<std::uint64_t,Identity> const& identities)
        {
            boost::json::array bans; for(auto guid:state.Bans) bans.push_back(guid);
            boost::json::array members;
            for(auto const& member:state.Members)
            {
                auto const& identity=identities.at(member.first);
                members.push_back(boost::json::object{{"guid",member.first},{"incarnation",member.second.Incarnation},{"team",member.second.Team},
                    {"flags",member.second.Flags},{"cross_faction",member.second.CrossFaction},{"node",identity.Node},
                    {"generation",identity.Generation},{"name",identity.Name},{"account",identity.Account}});
            }
            return boost::json::object{{"name",entry.Name},{"team",entry.Team},{"channel_id",entry.Id},{"announce",state.Announce},
                {"ownership",state.Ownership},{"password_verifier",state.PasswordVerifier},{"bans",std::move(bans)},
                {"runtime",boost::json::object{{"owner",state.Owner},{"members",std::move(members)}}}};
        }
        ChannelUpdate View(Entry const& entry,Pending const& work,bool snapshot,std::uint64_t recipient)
        {
            ChannelUpdate update; update.Name=entry.Name; update.ChannelId=entry.Id; update.Team=entry.Team;
            update.Actor=work.Request.Actor; update.ActorTeam=work.Command.ActorTeam; update.ActorCrossFaction=work.Command.CrossFaction; update.Action=work.Command.Action; update.Revision=entry.Revision;
            update.Owner=entry.State.Owner; update.Announce=entry.State.Announce; update.Ownership=entry.State.Ownership;
            update.Snapshot=snapshot; update.Error=std::uint8_t(work.Result.Status); update.TargetName=work.Command.TargetName;
            if(work.Command.Action==ChannelAction::QueryOwner)
            {
                auto identity=entry.Identities.find(entry.State.Owner);
                update.TargetName=identity==entry.Identities.end()?"Nobody":identity->second.Name;
                auto member=entry.State.Members.find(entry.State.Owner);
                if(member!=entry.State.Members.end())
                    if(auto owner=Presence.FindAny(entry.Realm,member->first,member->second.Incarnation,Now)) update.TargetName=owner->Name;
            }
            if(work.Command.Action==ChannelAction::Speak)
            {
                update.Text=work.Command.Text; update.Language=work.Command.Language; update.ChatTag=work.Command.ChatTag;
                if(auto sender=Presence.FindAny(entry.Realm,work.Request.Actor,work.Request.Incarnation,Now)) update.TargetName=sender->Name;
            }
            if(work.Result.Status==ChannelState::Error::None && work.Command.Action==ChannelAction::Leave) update.Removed=work.Request.Actor;
            if(work.Result.Status==ChannelState::Error::None && (work.Command.Action==ChannelAction::Kick || work.Command.Action==ChannelAction::Ban)) update.Removed=work.Command.Target;
            for(auto const& member:entry.State.Members)
                if(snapshot || member.first==recipient || member.first==work.Request.Actor || member.first==work.Command.Target || member.first==entry.State.Owner ||
                    (member.second.Flags & ChannelState::OwnerFlag))
                {
                    ChannelMemberView view{member.first,member.second.Incarnation,member.second.Flags};
                    if(auto profile=Presence.FindAny(entry.Realm,member.first,member.second.Incarnation,Now))
                    { view.Security=profile->Security; view.ProfileKnown=profile->ProfileKnown; view.Visible=profile->Visible; }
                    update.Members.push_back(view);
                }
            for(auto const& notice:work.Result.Notices) if(notice.Recipient==recipient)
                update.Notices.push_back({notice.Type,notice.OldFlags,notice.NewFlags,notice.Actor,notice.Target});
            return update;
        }
        void Finish(Entry& entry,ServiceStatus status)
        {
            auto work=std::move(entry.Work); if(!work) return;
            if(status!=ServiceStatus::Ok) { work->Complete({work->Request.Sequence,status,{}}); return; }
            if(work->Result.Status==ChannelState::Error::None)
            {
                std::map<std::uint64_t,std::uint64_t> recipients;
                for(auto const& member:entry.State.Members) recipients[member.first]=member.second.Incarnation;
                entry.State=std::move(work->Candidate); entry.Identities=std::move(work->Identities); if(work->Durable) entry.Revision=entry.StoredRevision;
                for(auto it=entry.Identities.begin();it!=entry.Identities.end();)
                    if(!entry.State.Members.count(it->first)) it=entry.Identities.erase(it); else ++it;
                for(auto const& member:entry.State.Members) recipients[member.first]=member.second.Incarnation;
                recipients[work->Request.Actor]=work->Request.Incarnation;
                if (work->Command.Action==ChannelAction::Invite && work->Command.Target)
                    if(auto target=Presence.FindByName(entry.Realm,work->Command.TargetName,Now)) recipients[target->Guid]=target->Incarnation;
                std::vector<ServiceEvent> events;
                for(auto const& recipient:recipients)
                {
                    if(recipient.first==work->Request.Actor) continue;
                    auto update=View(entry,*work,false,recipient.first);
                    if (Presence.FindAny(entry.Realm,recipient.first,recipient.second,Now))
                        events.push_back({ServiceDomain::Channel,recipient.first,recipient.second,EncodeChannelUpdate(update).Bytes});
                }
                if(!events.empty() && !EmitEvent(entry.Realm,std::move(events))) entry.Fenced=true;
            }
            if(entry.Fenced) { work->Complete({work->Request.Sequence,ServiceStatus::Unknown,{}}); return; }
            auto update=View(entry,*work,true,work->Request.Actor);
            work->Complete({work->Request.Sequence,ServiceStatus::Ok,EncodeChannelUpdate(update).Bytes});
        }
        void Apply(Entry& entry,CryptoResult const& crypto)
        {
            auto& work=*entry.Work; if(!crypto.Success) { Finish(entry,ServiceStatus::Unavailable); return; }
            if(!Presence.Find(entry.Realm,work.Identities.at(work.Request.Actor).Node,work.Request.Generation,work.Request.Actor,work.Request.Incarnation,Now))
            { Finish(entry,ServiceStatus::Rejected); return; }
            ChannelState::Command cmd; cmd.Type=ChannelState::Action(work.Command.Action); cmd.Actor=work.Request.Actor; cmd.Incarnation=work.Request.Incarnation;
            cmd.Target=work.Command.Target; cmd.Team=work.Command.ActorTeam; cmd.TargetTeam=work.Command.TargetTeam; cmd.Override=work.Command.Override;
            cmd.Silent=work.Command.Silent; cmd.CrossFaction=work.Command.CrossFaction; cmd.TargetCrossFaction=work.Command.TargetCrossFaction; cmd.Value=work.Command.Value;
            cmd.PasswordMatches=crypto.Match; cmd.PasswordVerifier=crypto.Verifier;
            work.Result=work.Candidate.Apply(cmd);
            bool mutation=work.Command.Action!=ChannelAction::Speak && work.Command.Action!=ChannelAction::List &&
                work.Command.Action!=ChannelAction::QueryOwner && work.Command.Action!=ChannelAction::Invite;
            work.Durable=mutation && work.Result.Status==ChannelState::Error::None;
            if(work.Durable)
            {
                unsigned char receipt[16]; if(RAND_bytes(receipt,sizeof(receipt))!=1) { Finish(entry,ServiceStatus::Unavailable); return; }
                work.Receipt=Hex(receipt,sizeof(receipt));
                if(!Persistence.Submit(entry.Realm,"channels",work.Receipt,entry.Key,entry.StoredRevision,work.Request.Actor,Document(entry,work.Candidate,work.Identities)))
                { Finish(entry,ServiceStatus::Unavailable); return; }
                work.Saving=true; return;
            }
            Finish(entry,ServiceStatus::Ok);
        }
    };
    ChannelAuthority::ChannelAuthority(PresenceDirectory& presence,SocialPersistence& persistence,Emit emit):_impl(new Impl(presence,persistence,std::move(emit))){}
    ChannelAuthority::~ChannelAuthority()=default;
    void ChannelAuthority::Handle(std::uint32_t realm,std::string const& node,ServiceRequest const& request,Completion complete)
    {
        auto& self=*_impl; self.ObserveOwnership(realm); ChannelCommand command;
        auto actor=self.Presence.Find(realm,node,request.Generation,request.Actor,request.Incarnation,self.Now);
        if(!actor || actor->Account!=request.Account || !DecodeChannelCommand(request.Payload,command)) { complete({request.Sequence,ServiceStatus::Rejected,{}}); return; }
        if(!self.Persistence.Enabled() || !self.Persistence.Ready(realm)) { complete({request.Sequence,ServiceStatus::Unavailable,{}}); return; }
        auto key=Key(command.Team,command.Name); auto identity=std::make_pair(realm,key); auto found=self.Entries.find(identity);
        if(found==self.Entries.end())
        {
            if(self.Entries.size()>=8192) { complete({request.Sequence,ServiceStatus::Rejected,{}}); return; }
            Impl::Entry entry; entry.Name=command.Name; entry.Key=key; entry.Realm=realm; entry.Team=command.Team; entry.Id=command.ChannelId;
            entry.State.Constant=command.ChannelId!=0; if(entry.State.Constant) entry.State.Announce=entry.State.Ownership=false;
            if(!self.Load(entry)) { complete({request.Sequence,ServiceStatus::Unavailable,{}}); return; }
            if(!entry.StoredRevision && command.Action!=ChannelAction::Join && command.Action!=ChannelAction::Ownership)
            { complete({request.Sequence,ServiceStatus::Rejected,{}}); return; }
            found=self.Entries.emplace(identity,std::move(entry)).first;
        }
        auto& entry=found->second;
        if(command.Action==ChannelAction::InviteDecision)
        {
            if(!entry.Work || !entry.Work->Inviting || entry.Work->Command.Target!=request.Actor ||
                entry.Work->InviteIncarnation!=request.Incarnation || entry.Work->InviteToken!=command.Text)
            { complete({request.Sequence,ServiceStatus::Rejected,{}}); return; }
            auto& original=*entry.Work;
            if(!command.Value) { original.Result.Status=ChannelState::Error::NotFound; self.Finish(entry,ServiceStatus::Ok); }
            else
            {
                original.Command.TargetTeam=command.ActorTeam; original.Command.TargetCrossFaction=command.CrossFaction;
                original.Inviting=false; self.Apply(entry,{true,true,{}});
            }
            complete({request.Sequence,ServiceStatus::Ok,{}}); return;
        }
        if(command.Action==ChannelAction::InviteCheck) { complete({request.Sequence,ServiceStatus::Rejected,{}}); return; }
        std::size_t memberships=0; for(auto const& item:self.Entries) memberships+=item.second.State.Members.size();
        if(command.Action==ChannelAction::Join && memberships>=65536) { complete({request.Sequence,ServiceStatus::Unavailable,{}}); return; }
        if(entry.Fenced || entry.Id!=command.ChannelId) { complete({request.Sequence,ServiceStatus::Unavailable,{}}); return; }
        if(entry.Work)
        {
            if(entry.Waiting.size()>=32) complete({request.Sequence,ServiceStatus::Unavailable,{}});
            else entry.Waiting.push_back({node,request,std::move(complete),self.Now});
            return;
        }
        std::size_t pending=0; for(auto const& item:self.Entries) if(item.second.Work) ++pending;
        if(pending>=16) { complete({request.Sequence,ServiceStatus::Unavailable,{}}); return; }
        auto work=std::make_unique<Impl::Pending>(); work->Request=request; work->Command=command; work->Complete=std::move(complete);
        work->Candidate=entry.State; work->Identities=entry.Identities; work->Started=self.Now;
        auto previous=entry.Identities.find(request.Actor);
        auto previousMember=entry.State.Members.find(request.Actor);
        if(previousMember!=entry.State.Members.end() && (previous==entry.Identities.end() || previous->second.Node!=node ||
            previous->second.Generation!=request.Generation || previousMember->second.Incarnation!=request.Incarnation))
        {
            if(command.Action!=ChannelAction::Join) { work->Complete({request.Sequence,ServiceStatus::Rejected,{}}); return; }
            ChannelState::Command departed; departed.Type=ChannelState::Action::Leave; departed.Actor=request.Actor;
            departed.Incarnation=previousMember->second.Incarnation; departed.Silent=true; work->Candidate.Apply(departed);
        }
        work->Identities[request.Actor]={node,request.Generation,actor->Name,actor->Account};
        if(!command.TargetName.empty())
        {
            for(auto const& member:entry.Identities) if(Normalize(member.second.Name)==Normalize(command.TargetName)) { work->Command.Target=member.first; break; }
            if(command.Action==ChannelAction::Invite)
            {
                auto target=self.Presence.FindByName(realm,command.TargetName,self.Now);
                work->Command.Target=target?target->Guid:0;
                work->InviteIncarnation=target?target->Incarnation:0;
            }
        }
        if(command.Action==ChannelAction::Invite && work->Command.Target)
        {
            // Admission checks happen before revealing presence through a remote offer.
            ChannelState::Command probe; probe.Type=ChannelState::Action::Invite; probe.Actor=request.Actor;
            probe.Incarnation=request.Incarnation; probe.Target=work->Command.Target;
            probe.Team=probe.TargetTeam=command.ActorTeam;
            work->Result=work->Candidate.Apply(probe);
            if(work->Result.Status!=ChannelState::Error::None)
            { entry.Work=std::move(work); self.Finish(entry,ServiceStatus::Ok); return; }
            unsigned char nonce[16];
            if(RAND_bytes(nonce,sizeof(nonce))!=1) { work->Complete({request.Sequence,ServiceStatus::Unavailable,{}}); return; }
            work->Inviting=true; work->InviteToken=Hex(nonce,sizeof(nonce));
            ChannelUpdate offer; offer.Name=entry.Name; offer.ChannelId=entry.Id; offer.Team=entry.Team;
            offer.Action=ChannelAction::InviteCheck; offer.Actor=request.Actor; offer.ActorTeam=command.ActorTeam;
            offer.ActorCrossFaction=command.CrossFaction; offer.Text=work->InviteToken;
            if(!self.EmitEvent(realm,{{ServiceDomain::Channel,work->Command.Target,work->InviteIncarnation,EncodeChannelUpdate(offer).Bytes}}))
            { work->Complete({request.Sequence,ServiceStatus::Unavailable,{}}); return; }
            // Release the source connection so its worker can poll receiver decisions.
            work->Complete({request.Sequence,ServiceStatus::Ok,{}});
            auto source=request.Actor, incarnation=request.Incarnation;
            work->Complete=[&self,realm,source,incarnation,name=entry.Name,team=entry.Team,id=entry.Id](ServiceResponse response)
            {
                if(response.Status!=ServiceStatus::Ok)
                {
                    ChannelUpdate failed; failed.Name=name; failed.Team=team; failed.ChannelId=id; failed.Actor=source;
                    failed.Action=ChannelAction::Invite; failed.Error=12; response.Payload=EncodeChannelUpdate(failed).Bytes;
                }
                if(!response.Payload.empty())
                    self.EmitEvent(realm,{{ServiceDomain::Channel,source,incarnation,std::move(response.Payload)}});
            };
            entry.Work=std::move(work); return;
        }
        bool crypto=command.Action==ChannelAction::Password || (command.Action==ChannelAction::Join && !entry.State.PasswordVerifier.empty());
        entry.Work=std::move(work);
        if(crypto)
        {
            auto verifier=entry.State.PasswordVerifier; auto password=command.Password; bool create=command.Action==ChannelAction::Password;
            entry.Work->Crypto=std::async(std::launch::async,[password=std::move(password),verifier=std::move(verifier),create]{return Password(password,verifier,create);});
        }
        else self.Apply(entry,{true,true,{}});
    }
    void ChannelAuthority::Update(std::uint64_t now,std::vector<SocialPersistence::Result> const& results)
    {
        auto& self=*_impl; self.Now=now;
        for(auto const& realm:self.Epochs) self.ObserveOwnership(realm.first);
        for(auto const& result:results) if(result.Domain=="channels")
            for(auto& item:self.Entries)
            {
                auto& entry=item.second; if(entry.Realm!=result.Realm || !entry.Work || entry.Work->Receipt!=result.Request) continue;
                if(result.Status==SocialPersistence::Outcome::Committed) { entry.StoredRevision=result.Revision; self.Finish(entry,ServiceStatus::Ok); }
                else { entry.Fenced=true; self.Finish(entry,result.Status==SocialPersistence::Outcome::Unknown?ServiceStatus::Unknown:ServiceStatus::Unavailable); }
                break;
            }
        std::vector<std::pair<std::uint32_t,Impl::Queued>> waiting;
        bool expire=now-self.LastExpire>=1000; if(expire) self.LastExpire=now;
        for(auto& item:self.Entries)
        {
            auto& entry=item.second;
            if(expire && !entry.Fenced && !entry.Work && now>=entry.RestoreGrace)
            {
                std::vector<std::pair<std::uint64_t,std::uint64_t>> departed;
                for(auto const& member:entry.State.Members)
                {
                    auto identity=entry.Identities.find(member.first);
                    if(identity==entry.Identities.end() || !self.Presence.Find(entry.Realm,identity->second.Node,identity->second.Generation,member.first,member.second.Incarnation,now))
                        departed.push_back({member.first,member.second.Incarnation});
                }
                for(auto const& member:departed)
                {
                    auto work=std::make_unique<Impl::Pending>(); work->Request.Actor=member.first; work->Request.Incarnation=member.second;
                    work->Command.Action=ChannelAction::Leave; work->Candidate=entry.State; work->Identities=entry.Identities;
                    work->Complete=[](ServiceResponse){}; ChannelState::Command command;
                    command.Type=ChannelState::Action::Leave; command.Actor=member.first; command.Incarnation=member.second; command.Silent=true;
                    work->Result=work->Candidate.Apply(command); work->Durable=true; work->Started=now;
                    unsigned char receipt[16];
                    if(RAND_bytes(receipt,sizeof(receipt))!=1) { entry.Fenced=true; break; }
                    work->Receipt=Hex(receipt,sizeof(receipt)); work->Saving=true;
                    if(!self.Persistence.Submit(entry.Realm,"channels",work->Receipt,entry.Key,entry.StoredRevision,member.first,self.Document(entry,work->Candidate,work->Identities)))
                    { entry.Fenced=true; break; }
                    entry.Work=std::move(work); break;
                }
            }
            if(entry.Work && entry.Work->Crypto.valid() && entry.Work->Crypto.wait_for(std::chrono::seconds(0))==std::future_status::ready)
                self.Apply(entry,entry.Work->Crypto.get());
            if(entry.Work && now-entry.Work->Started>10000)
            {
                bool uncertain=entry.Work->Saving; entry.Fenced=entry.Fenced || uncertain;
                self.Finish(entry,uncertain?ServiceStatus::Unknown:ServiceStatus::Unavailable);
            }
            while(!entry.Waiting.empty() && (entry.Fenced || now-entry.Waiting.front().Started>4000))
            {
                auto request=std::move(entry.Waiting.front()); entry.Waiting.pop_front();
                request.Complete({request.Request.Sequence,ServiceStatus::Unavailable,{}});
            }
            if(!entry.Work && !entry.Waiting.empty())
            {
                waiting.push_back({entry.Realm,std::move(entry.Waiting.front())}); entry.Waiting.pop_front();
            }
        }
        for(auto& queued:waiting) Handle(queued.first,queued.second.Node,queued.second.Request,std::move(queued.second.Complete));
    }
}
