/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "ChatServer.h"
#include "SocialPersistence.h"
#include "Cluster/ChatRemoteWhisper.h"
#include "Cluster/ChatGroupRelay.h"
#include "Cluster/CertificateTools.h"
#include "Cluster/ChatProtocol.h"
#include "Cluster/ChatPresence.h"
#include "Cluster/ChatWhisper.h"
#include "Cluster/ChatRouting.h"
#include "Cluster/GuildWire.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/x509.h>
#include <openssl/rand.h>
#include <chrono>
#include <array>
#include <limits>
#include <deque>
#include <tuple>
#include <stdexcept>

namespace Skyfire::Chat
{
    struct Server::State
    {
        struct Session;
        boost::asio::io_context Io;
        boost::asio::ssl::context Tls{boost::asio::ssl::context::tls_server};
        boost::asio::ip::tcp::acceptor Acceptor{Io};
        Options Config;
        Cluster::AgentOptions Credentials;
        std::set<std::shared_ptr<Session>> Sessions;
        Cluster::ChatMetrics Counters;
        PresenceDirectory Presence;
        MessageRouter Router;
        SocialPersistence Persistence;
        std::map<std::uint32_t, std::uint64_t> Ownership;
        std::uint64_t CurrentEpoch(std::uint32_t realm) const
        { return Persistence.Enabled() ? Persistence.OwnershipEpoch(realm) : (Persistence.Ready(realm) ? 1 : 0); }
        ServiceHandler Handler;
        std::function<bool(std::uint32_t, ServiceEvent)> Emit;
        std::function<bool(std::uint32_t, std::vector<ServiceEvent>)> EmitBatch;
        struct RemotePending
        {
            std::uint32_t Realm;
            std::uint64_t Receiver, Incarnation, Expires;
            std::string Node, Generation;
            ServiceCompletion Complete;
        };
        std::map<std::uint64_t, RemotePending> RemoteWhispers;
        std::uint64_t RemoteSequence = 0;
        void Remote(std::uint32_t realm, std::string const& node, ServiceRequest const& request, ServiceCompletion completion, std::uint64_t now)
        {
            if (request.Payload.empty()) { completion({request.Sequence, ServiceStatus::Rejected, {}}); return; }
            if (request.Payload[0] == 2)
            {
                RemoteWhisperReply reply;
                if (!DecodeRemoteWhisperReply(request.Payload, reply)) { completion({request.Sequence, ServiceStatus::Rejected, {}}); return; }
                auto receiver = Presence.FindAny(realm, request.Actor, request.Incarnation, now);
                auto pending = RemoteWhispers.find(reply.Token);
                if (!receiver || receiver->Name != reply.Name || pending == RemoteWhispers.end() || pending->second.Realm != realm || pending->second.Receiver != request.Actor ||
                    reply.Receiver != request.Actor || pending->second.Incarnation != request.Incarnation || pending->second.Node != node ||
                    pending->second.Generation != request.Generation || now >= pending->second.Expires)
                { completion({request.Sequence, ServiceStatus::Rejected, {}}); return; }
                auto result = std::move(pending->second.Complete); RemoteWhispers.erase(pending);
                result({0, ServiceStatus::Ok, request.Payload}); completion({request.Sequence, ServiceStatus::Ok, {}}); return;
            }
            RemoteWhisper message;
            auto sender = Presence.FindAny(realm, request.Actor, request.Incarnation, now);
            if (!DecodeRemoteWhisper(request.Payload, message) || message.Token || message.Sender != request.Actor ||
                message.Incarnation != request.Incarnation || !sender || message.SenderName != sender->Name)
            { completion({request.Sequence, ServiceStatus::Rejected, {}}); return; }
            auto receiver = Presence.FindByName(realm, message.ReceiverName, now);
            if (!receiver) { completion({request.Sequence, ServiceStatus::Rejected, {}}); return; }
            if (RemoteWhispers.size() >= 128) { completion({request.Sequence, ServiceStatus::Unavailable, {}}); return; }
            std::string receiverNode, receiverGeneration;
            if (!Presence.Locate(realm, receiver->Guid, receiver->Incarnation, now, receiverNode, receiverGeneration))
            { completion({request.Sequence, ServiceStatus::Unavailable, {}}); return; }
            // A receiver may still hold an offer from the previous daemon.
            // Randomize the token namespace so its late acknowledgement cannot
            // accidentally complete a different offer after a chat restart.
            if (!RemoteSequence)
            {
                std::array<unsigned char, 8> random{};
                if (RAND_bytes(random.data(), int(random.size())) != 1)
                { completion({request.Sequence, ServiceStatus::Unavailable, {}}); return; }
                for (auto byte : random) RemoteSequence = (RemoteSequence << 8) | byte;
            }
            if (RemoteSequence == (std::numeric_limits<std::uint64_t>::max)())
            { completion({request.Sequence, ServiceStatus::Unavailable, {}}); return; }
            message.Token = ++RemoteSequence;
            ServiceEvent event{ServiceDomain::Whisper, receiver->Guid, receiver->Incarnation, EncodeRemoteWhisper(message).Bytes};
            if (!Emit || !Emit(realm, std::move(event))) { completion({request.Sequence, ServiceStatus::Unavailable, {}}); return; }
            auto senderGuid = request.Actor, senderIncarnation = request.Incarnation, sequence = request.Sequence;
            RemoteWhispers.emplace(message.Token, RemotePending{realm, receiver->Guid, receiver->Incarnation, now + 4000, std::move(receiverNode), std::move(receiverGeneration),
                [this, realm, senderGuid, senderIncarnation, sequence, node, generation = request.Generation](ServiceResponse result)
                {
                    auto current = std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                    if (!Presence.Find(realm, node, generation, senderGuid, senderIncarnation, current)) return;
                    RemoteWhisperReply reply;
                    if (result.Status != ServiceStatus::Ok || !DecodeRemoteWhisperReply(result.Payload, reply)) return;
                    reply.Token = sequence;
                    if (Emit) Emit(realm, {ServiceDomain::Whisper, senderGuid, senderIncarnation, EncodeRemoteWhisperReply(reply).Bytes});
                }});
            // Delivery acknowledgment returns as an event: a synchronous wait would deadlock two worlds whispering to each other.
            completion({request.Sequence, ServiceStatus::Ok, {}});
        }
        struct Mailbox
        {
            std::string Epoch;
            Mailbox()
            {
                std::array<unsigned char, 32> random{};
                if (RAND_bytes(random.data(), int(random.size())) != 1) throw std::runtime_error("Cannot create chat delivery epoch");
                for (auto byte : random) { Epoch.push_back("0123456789abcdef"[byte >> 4]); Epoch.push_back("0123456789abcdef"[byte & 15]); }
            }
            std::uint64_t LastRequest = 0, LastEvent = 0, Acknowledged = 0;
            std::size_t Bytes = 0;
            struct Delivery { std::uint64_t Sequence; ServiceEvent Event; std::uint64_t Expires; };
            std::deque<Delivery> Events;
        };
        using MailKey = std::tuple<std::uint32_t, std::string, std::string>;
        std::map<MailKey, Mailbox> Mailboxes;
        std::size_t EventBytes = 0;
        std::chrono::steady_clock::time_point Started = std::chrono::steady_clock::now();
        bool Stopping = false;
        void Accept();
    };
    struct Server::State::Session : std::enable_shared_from_this<Session>
    {
        State& Owner;
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> Stream;
        boost::asio::steady_timer Deadline;
        std::array<std::uint8_t, ProbeSize> Input{};
        std::vector<std::uint8_t> Output, Body, Routed;
        std::array<std::uint8_t, 4> Length{};
        std::string Identity;
        std::uint32_t Realm = 0;
        std::uint64_t Epoch = 0;
        bool OwnsRealm() const
        {
            auto owned = Owner.Ownership.find(Realm);
            return Epoch && Owner.CurrentEpoch(Realm) == Epoch && owned != Owner.Ownership.end() && owned->second == Epoch;
        }
        bool Closed = false, Replied = false;
        Session(State& owner, boost::asio::ip::tcp::socket socket) :
            Owner(owner), Stream(std::move(socket), owner.Tls), Deadline(owner.Io) { }
        void Close(bool failure = false)
        {
            if (Closed) return;
            Closed = true;
            if (failure) ++Owner.Counters.Failures;
            Deadline.cancel();
            boost::system::error_code ec;
            Stream.next_layer().close(ec);
            Owner.Sessions.erase(shared_from_this());
        }
        bool Authorized()
        {
            X509* cert = SSL_get_peer_certificate(Stream.native_handle());
            if (!cert) return false;
            X509_NAME const* name = X509_get_subject_name(cert);
            int const index = X509_NAME_get_index_by_NID(name, NID_commonName, -1);
            bool valid = false;
            if (index >= 0 && X509_NAME_get_index_by_NID(name, NID_commonName, index) < 0)
            {
                ASN1_STRING const* value = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(name, index));
                int const length = ASN1_STRING_length(value);
                if (length > 0 && length <= 64)
                {
                    std::string key(reinterpret_cast<char const*>(ASN1_STRING_get0_data(value)), std::size_t(length));
                    valid = Cluster::ValidKey(key) && Owner.Config.WorldKeys.count(key) != 0;
                    if (valid) Identity = key;
                }
            }
            X509_free(cert);
            return valid && Certificates::PeerAllowed(Stream.native_handle());
        }
        void Reply()
        {
            if (Closed || Replied) return;
            if (!OwnsRealm()) { Close(true); return; }
            Replied = true;
            ++Owner.Counters.Requests;
            Output.assign(Input.begin(), Input.end()); Output[6] = 0x80;
            if (Input[7] == 3) { ++Owner.Counters.WhisperRelays; Output.insert(Output.end(), Body.begin(), Body.end()); }
            if (Input[7] >= 4)
            {
                Cluster::Writer length; length.U32(std::uint32_t(Routed.size()));
                Output.insert(Output.end(), length.Bytes.begin(), length.Bytes.end());
                Output.insert(Output.end(), Routed.begin(), Routed.end());
            }
            auto self = shared_from_this();
            boost::asio::async_write(Stream, boost::asio::buffer(Output),
                [self](boost::system::error_code ec, std::size_t) { self->Close(bool(ec)); });
        }
        void Start()
        {
            auto self = shared_from_this();
            // One total deadline covers handshake, request and reply; slow clients cannot retain slots.
            Deadline.expires_after(std::chrono::seconds(Owner.Config.TimeoutSeconds));
            Deadline.async_wait([self](boost::system::error_code ec) { if (!ec) self->Close(true); });
            Stream.async_handshake(boost::asio::ssl::stream_base::server, [self](boost::system::error_code ec)
            {
                if (self->Closed) return;
                if (ec || !self->Authorized()) { self->Close(true); return; }
                boost::asio::async_read(self->Stream, boost::asio::buffer(self->Input),
                    [self](boost::system::error_code readError, std::size_t)
                {
                    if (self->Closed) return;
                    std::uint32_t id = 0, realm = 0;
                    auto probe = self->Input;
                    bool const presence = probe[6] == 0 && (probe[7] >= 2 && probe[7] <= 6);
                    if (presence) probe[7] = 1;
                    if (readError || !DecodeProbe(probe, id, realm) || !self->Owner.Config.Realms.count(realm))
                    { self->Close(true); return; }
                    self->Realm = realm; self->Epoch = self->Owner.CurrentEpoch(realm);
                    if (!self->OwnsRealm()) { self->Close(true); return; }
                    auto scope = self->Owner.Config.WorldRealms.find(self->Identity);
                    if (scope == self->Owner.Config.WorldRealms.end() || !scope->second.count(realm))
                    { self->Close(true); return; }
                    if (!presence) { self->Reply(); return; }
                    boost::asio::async_read(self->Stream, boost::asio::buffer(self->Length),
                        [self, realm](boost::system::error_code lengthError, std::size_t)
                    {
                        if (self->Closed) return;
                        auto const& b = self->Length;
                        std::uint32_t length = (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) |
                            (std::uint32_t(b[2]) << 8) | b[3];
                        if (lengthError || !length || length > (self->Input[7] == 3 ? MaxWhisperBytes :
                            self->Input[7] == 4 ? MaxRoutingBytes : self->Input[7] >= 5 ? MaxServiceFrame : MaxPresenceBytes)) { self->Close(true); return; }
                        self->Body.resize(length);
                        boost::asio::async_read(self->Stream, boost::asio::buffer(self->Body),
                            [self, realm](boost::system::error_code bodyError, std::size_t)
                        {
                            if (self->Closed) return;
                            if (!self->OwnsRealm()) { self->Close(true); return; }
                            PresenceSnapshot snapshot;
                            auto now = std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count());
                            if (self->Input[7] == 5)
                            {
                                ServiceRequest request;
                                if (bodyError || !DecodeServiceRequest(self->Body, request)) { self->Close(true); return; }
                                auto player = self->Owner.Presence.Find(realm, self->Identity, request.Generation,
                                    request.Actor, request.Incarnation, now);
                                bool console = request.Domain == ServiceDomain::Guild && !request.Account && !request.Actor && !request.Incarnation &&
                                    self->Owner.Presence.HasGeneration(realm, self->Identity, request.Generation, now);
                                GuildRequest guild;
                                bool recovery = request.Domain == ServiceDomain::Guild &&
                                    DecodeGuildRequest(request.Payload, guild) && guild.Command.Type == GuildState::Action::Status &&
                                    self->Owner.Presence.HasGeneration(realm, self->Identity, request.Generation, now);
                                // Recovery proves the original actor tuple durably; it must
                                // still work after that player logs out of this world generation.
                                if (!console && !recovery && (!player || player->Account != request.Account)) { self->Close(true); return; }
                                auto& mailbox = self->Owner.Mailboxes[{realm, self->Identity, request.Generation}];
                                if (request.Sequence <= mailbox.LastRequest) { self->Close(true); return; }
                                mailbox.LastRequest = request.Sequence;
                                auto completion = [weak = std::weak_ptr<Session>(self), sequence = request.Sequence](ServiceResponse response)
                                {
                                    auto active = weak.lock();
                                    if (!active || active->Closed || active->Replied) return;
                                    if (response.Payload.size() > MaxServicePayload) { active->Close(true); return; }
                                    response.Sequence = sequence;
                                    active->Routed = EncodeServiceResponse(response).Bytes; active->Reply();
                                };
                                if (request.Domain == ServiceDomain::Group)
                                {
                                    GroupRelay group;
                                    if (!DecodeGroupRelay(request.Payload, group) || group.Sender != request.Actor ||
                                        group.Incarnation != request.Incarnation || group.Name != player->Name)
                                    { completion({request.Sequence, ServiceStatus::Rejected, {}}); return; }
                                    std::vector<ServiceEvent> events;
                                    for (auto guid : group.Members)
                                    {
                                        if (guid == group.Ignore) continue;
                                        auto member = self->Owner.Presence.FindGuid(realm, guid, now);
                                        if (member) events.push_back({ServiceDomain::Group, member->Guid, member->Incarnation, request.Payload});
                                    }
                                    bool success = self->Owner.EmitBatch && self->Owner.EmitBatch(realm, std::move(events));
                                    completion({request.Sequence, success ? ServiceStatus::Ok : ServiceStatus::Unavailable, {}}); return;
                                }
                                if (request.Domain == ServiceDomain::Whisper)
                                { self->Owner.Remote(realm, self->Identity, request, std::move(completion), now); return; }
                                if (!self->Owner.Handler) { completion({request.Sequence, ServiceStatus::Unavailable, {}}); return; }
                                self->Owner.Handler(realm, self->Identity, request, std::move(completion)); return;
                            }
                            if (self->Input[7] == 6)
                            {
                                Cluster::Reader in(self->Body); std::string generation, epoch; std::uint64_t acknowledged;
                                if (bodyError || !in.String(generation, 64) || !ServiceGeneration(generation) ||
                                    !Read64(in, acknowledged) || !in.String(epoch, 64) || !ServiceGeneration(epoch) || !in.End() ||
                                    !self->Owner.Presence.HasGeneration(realm, self->Identity, generation, now))
                                { self->Close(true); return; }
                                auto& mailbox = self->Owner.Mailboxes[{realm, self->Identity, generation}];
                                if (epoch != mailbox.Epoch) acknowledged = mailbox.Acknowledged;
                                if (acknowledged < mailbox.Acknowledged || acknowledged > mailbox.LastEvent)
                                { self->Close(true); return; }
                                while (!mailbox.Events.empty() && mailbox.Events.front().Sequence <= acknowledged)
                                {
                                    auto bytes = mailbox.Events.front().Event.Payload.size();
                                    mailbox.Bytes -= bytes; self->Owner.EventBytes -= bytes; mailbox.Events.pop_front();
                                }
                                mailbox.Acknowledged = acknowledged;
                                Cluster::Writer events; std::uint16_t count = 0; std::uint64_t cursor = acknowledged;
                                for (auto const& event : mailbox.Events)
                                {
                                    if (count >= 32 || events.Bytes.size() + event.Event.Payload.size() + 21 > MaxServicePayload) break;
                                    if (event.Expires && now >= event.Expires)
                                    {
                                        auto expired = event.Event; expired.Payload.clear(); WriteServiceEvent(events, expired);
                                    }
                                    else WriteServiceEvent(events, event.Event);
                                    cursor = event.Sequence; ++count;
                                }
                                Cluster::Writer out; out.String(mailbox.Epoch); Write64(out, cursor); out.U16(count);
                                out.Bytes.insert(out.Bytes.end(), events.Bytes.begin(), events.Bytes.end());
                                self->Routed = std::move(out.Bytes); self->Reply(); return;
                            }
                            if (self->Input[7] == 4)
                            {
                                AudienceProjection projection; RoutedMessage message;
                                if (bodyError || !DecodeRoute(self->Body, projection, message)) { self->Close(true); return; }
                                bool const control = projection.Kind >= AudienceKind::ChannelControl;
                                if (!self->Owner.Router.Project(self->Owner.Presence, realm, self->Identity, std::move(projection), now) ||
                                    !self->Owner.Router.Route(self->Owner.Presence, realm, self->Identity, message, now))
                                { self->Close(true); return; }
                                auto deliveries = self->Owner.Router.Take(realm, self->Identity, message.Generation, now);
                                if (deliveries.size() != 1 || deliveries.front().Message.Sequence != message.Sequence)
                                { self->Close(true); return; }
                                self->Routed = EncodeRecipients(deliveries.front().Recipients).Bytes;
                                if (control) ++self->Owner.Counters.RoutedControls;
                                else ++self->Owner.Counters.RoutedMessages;
                                self->Owner.Counters.RoutedRecipients += std::uint32_t(deliveries.front().Recipients.size());
                                self->Reply(); return;
                            }
                            if (self->Input[7] == 3)
                            {
                                Whisper message;
                                if (bodyError || !DecodeWhisper(self->Body, message) ||
                                    !AuthorizeWhisper(self->Owner.Presence, realm, self->Identity, message, now))
                                { self->Close(true); return; }
                                self->Reply(); return;
                            }
                            if (bodyError || !DecodePresence(self->Body, snapshot) ||
                                !self->Owner.Presence.Replace(realm, self->Identity, std::move(snapshot), now))
                            { self->Close(true); return; }
                            self->Reply();
                        });
                    });
                });
            });
        }
    };
    void Server::State::Accept()
    {
        if (Stopping) return;
        Acceptor.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket)
        {
            if (Stopping) return;
            if (!ec)
            {
                if (Sessions.size() >= Config.MaxConnections) ++Counters.Failures;
                else
                {
                    try
                    {
                        Tls.use_certificate_chain_file(Credentials.Certificate);
                        Tls.use_private_key_file(Credentials.PrivateKey, boost::asio::ssl::context::pem);
                    }
                    catch (...) { ++Counters.Failures; Accept(); return; }
                    auto session = std::make_shared<Session>(*this, std::move(socket));
                    Sessions.insert(session); session->Start();
                }
            }
            else ++Counters.Failures;
            Accept();
        });
    }
    Server::Server() = default;
    Server::~Server() { Stop(); }
    bool Server::Open(Options options, Cluster::AgentOptions const& tls, std::string& error)
    {
        if (_state || !options.Port || !options.MaxConnections || options.MaxConnections > 128 ||
            !options.TimeoutSeconds || options.TimeoutSeconds > 30 || options.WorldKeys.empty() || options.WorldKeys.size() > 128 ||
            options.Realms.empty() || options.Realms.size() > 64 || options.Realms.count(0))
        { error = "Invalid chat endpoint, connection limit, deadline or world allowlist."; return false; }
        for (auto const& key : options.WorldKeys)
            if (!Cluster::ValidKey(key)) { error = "Invalid chat world certificate identity."; return false; }
        for (auto const& scope : options.WorldRealms)
        {
            if (!options.WorldKeys.count(scope.first) || scope.second.empty())
            { error = "Presence scope requires an allowed world identity and realm list."; return false; }
            for (auto realm : scope.second)
                if (!options.Realms.count(realm)) { error = "Presence scope contains an unserved realm."; return false; }
        }
        try
        {
            auto state = std::make_unique<State>(); state->Config = std::move(options); state->Credentials = tls;
            if (!state->Persistence.Start(state->Config.Realms, tls, error)) return false;
            state->Counters.Realms.assign(state->Config.Realms.begin(), state->Config.Realms.end());
            if (!SSL_CTX_set_min_proto_version(state->Tls.native_handle(), TLS1_2_VERSION))
            { error = "Cannot require TLS 1.2."; return false; }
            state->Tls.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert);
            state->Tls.load_verify_file(tls.CA);
            state->Tls.use_certificate_chain_file(tls.Certificate);
            state->Tls.use_private_key_file(tls.PrivateKey, boost::asio::ssl::context::pem);
            if (!SSL_CTX_check_private_key(state->Tls.native_handle()))
            { error = "Chat certificate and private key do not match."; return false; }
            boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::make_address(state->Config.Address), state->Config.Port);
            state->Acceptor.open(endpoint.protocol());
            state->Acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
            state->Acceptor.bind(endpoint); state->Acceptor.listen(128);
            _state = std::move(state); _state->Emit = [this](std::uint32_t realm, ServiceEvent event) { return EmitServiceEvent(realm, std::move(event)); }; _state->EmitBatch = [this](std::uint32_t realm, std::vector<ServiceEvent> events) { return EmitServiceEvents(realm, std::move(events)); }; _state->Accept(); return true;
        }
        catch (std::exception const&) { Stop(); error = "Cannot open chat listener; check endpoint and TLS files."; return false; }
    }
    void Server::Update()
    {
        if (_state)
            for (auto realm : _state->Config.Realms)
            {
                auto epoch = _state->CurrentEpoch(realm);
                auto& previous = _state->Ownership[realm];
                if (epoch == previous) continue;
                previous = epoch;
                _state->Presence.ClearRealm(realm); _state->Router.ClearRealm(realm);
                for (auto it = _state->RemoteWhispers.begin(); it != _state->RemoteWhispers.end();)
                    if (it->second.Realm == realm) it = _state->RemoteWhispers.erase(it); else ++it;
                for (auto it = _state->Mailboxes.begin(); it != _state->Mailboxes.end();)
                    if (std::get<0>(it->first) == realm)
                    { _state->EventBytes -= it->second.Bytes; it = _state->Mailboxes.erase(it); }
                    else ++it;
                for (auto it = _state->Sessions.begin(); it != _state->Sessions.end();)
                { auto session = *it++; if (session->Realm == realm) session->Close(); }
            }
        if (_state) _state->Presence.Expire(std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));
        if (_state)
        {
            auto now = std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            for (auto it = _state->RemoteWhispers.begin(); it != _state->RemoteWhispers.end();)
                if (now >= it->second.Expires)
                { auto complete = std::move(it->second.Complete); it = _state->RemoteWhispers.erase(it); complete({0, ServiceStatus::Unknown, {}}); }
                else ++it;
            for (auto it = _state->Mailboxes.begin(); it != _state->Mailboxes.end();)
                if (!_state->Presence.HasGeneration(std::get<0>(it->first), std::get<1>(it->first), std::get<2>(it->first), now))
                { _state->EventBytes -= it->second.Bytes; it = _state->Mailboxes.erase(it); }
                else ++it;
        }
        // Bound work per main-loop iteration so service-control heartbeats cannot starve.
        for (unsigned count = 0; _state && count < 64 && _state->Io.poll_one(); ++count) { }
    }
    void Server::SetServiceHandler(ServiceHandler handler)
    { if (_state) _state->Handler = std::move(handler); }
    PresenceDirectory& Server::GetPresence()
    { if (!_state) throw std::logic_error("Chat listener is not open"); return _state->Presence; }
    SocialPersistence& Server::GetPersistence()
    { if (!_state) throw std::logic_error("Chat listener is not open"); return _state->Persistence; }
    bool Server::CanEmitServiceEvents(std::uint32_t realm, std::vector<ServiceEvent> const& events) const
    {
        if (!_state || events.size() > 16384) return false;
        auto owned = _state->Ownership.find(realm);
        auto epoch = _state->CurrentEpoch(realm);
        if (!epoch || owned == _state->Ownership.end() || owned->second != epoch) return false;
        auto now = std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        std::map<State::MailKey, std::pair<std::size_t, std::size_t>> needed;
        std::size_t total = 0;
        for (auto const& event : events)
        {
            // Leave enough space for one event and its poll framing.
            if (event.Payload.size() > MaxServicePayload - 64 ||
                (event.Domain != ServiceDomain::Channel && event.Domain != ServiceDomain::Guild && event.Domain != ServiceDomain::Whisper && event.Domain != ServiceDomain::Group)) return false;
            std::string node, generation;
            if (!_state->Presence.Locate(realm, event.Recipient, event.Incarnation, now, node, generation)) return false;
            auto& budget = needed[{realm, node, generation}]; ++budget.first; budget.second += event.Payload.size(); total += event.Payload.size();
        }
        if (_state->EventBytes + total > 64 * 1024 * 1024) return false;
        for (auto const& budget : needed)
        {
            auto mailbox = _state->Mailboxes.find(budget.first);
            auto count = mailbox == _state->Mailboxes.end() ? 0 : mailbox->second.Events.size();
            auto bytes = mailbox == _state->Mailboxes.end() ? 0 : mailbox->second.Bytes;
            if (count + budget.second.first > 4096 || bytes + budget.second.second > 4 * 1024 * 1024) return false;
        }
        return true;
    }
    bool Server::EmitServiceEvents(std::uint32_t realm, std::vector<ServiceEvent> events)
    {
        if (!CanEmitServiceEvents(realm, events)) return false;
        auto now = std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        std::vector<State::MailKey> destinations;
        for (auto const& event : events)
        {
            std::string node, generation;
            if (!_state->Presence.Locate(realm, event.Recipient, event.Incarnation, now, node, generation)) return false;
            destinations.emplace_back(realm, node, generation);
        }
        for (std::size_t i = 0; i < events.size(); ++i)
        {
            auto& event = events[i]; auto& mailbox = _state->Mailboxes[destinations[i]];
            mailbox.Bytes += event.Payload.size(); _state->EventBytes += event.Payload.size();
            auto expires = event.Domain == ServiceDomain::Whisper || event.Domain == ServiceDomain::Group || event.Domain == ServiceDomain::Guild ? now + 4000 : 0;
            mailbox.Events.push_back({++mailbox.LastEvent, std::move(event), expires});
        }
        return true;
    }
    bool Server::EmitServiceEvent(std::uint32_t realm, ServiceEvent event)
    { return EmitServiceEvents(realm, {std::move(event)}); }
    void Server::Stop()
    {
        if (!_state) return;
        _state->Stopping = true;
        boost::system::error_code ec; _state->Acceptor.close(ec);
        while (!_state->Sessions.empty()) (*_state->Sessions.begin())->Close();
        _state->Io.poll(); _state.reset();
    }
    bool Server::Ready() const { return _state && _state->Persistence.Ready(); }
    Cluster::ChatMetrics Server::Metrics() const
    {
        if (!_state) return {};
        auto result = _state->Counters;
        result.PresencePlayers = _state->Presence.Players();
        result.Connections = std::uint32_t(_state->Sessions.size());
        result.Uptime = std::uint32_t(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - _state->Started).count());
        return result;
    }
}
