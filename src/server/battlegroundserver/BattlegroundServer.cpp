/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "BattlegroundServer.h"
#include "Cluster/BattlegroundQueueBook.h"
#include "Cluster/CertificateTools.h"
#include "Log.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/x509.h>
#include <chrono>
#include <stdexcept>

namespace Skyfire::BattlegroundService
{
    namespace
    {
        std::uint64_t Now()
        { return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
    }
    struct Server::State
    {
        struct Session;
        boost::asio::io_context Io;
        boost::asio::ssl::context Tls{boost::asio::ssl::context::tls_server};
        boost::asio::ip::tcp::acceptor Acceptor{Io};
        std::set<std::shared_ptr<Session>> Sessions;
        Options Config;
        Cluster::AgentOptions Credentials;
        QueueBook Queues;
        Cluster::BattlegroundMetrics Counters;
        std::uint64_t Started = Now();
        std::uint64_t NextTlsReload = 0, LastRejectionLog = 0;
        bool Ready = false, TlsHealthy = true, Stopping = false;
        void Accept();
    };
    struct Server::State::Session : std::enable_shared_from_this<Session>
    {
        State& Owner;
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> Stream;
        boost::asio::steady_timer Deadline;
        std::array<std::uint8_t,16> Header{};
        std::array<std::uint8_t,4> Length{};
        std::vector<std::uint8_t> Body,Output;
        std::string Identity;
        bool Closed = false;
        Session(State& owner,boost::asio::ip::tcp::socket socket):Owner(owner),Stream(std::move(socket),owner.Tls),Deadline(owner.Io) {}
        void Close(bool error=false)
        {
            if(Closed) return; Closed=true; if(error) ++Owner.Counters.Failures;
            Deadline.cancel(); boost::system::error_code ignored; Stream.next_layer().close(ignored); Owner.Sessions.erase(shared_from_this());
        }
        bool Authorized()
        {
            X509* certificate=SSL_get_peer_certificate(Stream.native_handle()); if(!certificate) return false;
            X509_NAME const* subject=X509_get_subject_name(certificate);
            int index=X509_NAME_get_index_by_NID(subject,NID_commonName,-1);
            bool valid=false;
            if(index>=0 && X509_NAME_get_index_by_NID(subject,NID_commonName,index)<0)
            {
                ASN1_STRING const* value=X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject,index));
                int length=ASN1_STRING_length(value);
                if(length>0 && length<=64)
                {
                    Identity.assign(reinterpret_cast<char const*>(ASN1_STRING_get0_data(value)),std::size_t(length));
                    valid=Cluster::ValidKey(Identity) && Owner.Config.WorldKeys.count(Identity);
                }
            }
            X509_free(certificate); return valid && Certificates::PeerAllowed(Stream.native_handle());
        }
        void Reply(Response const* response=nullptr)
        {
            if(Closed) return; if(!Owner.Ready || !Owner.TlsHealthy) { Close(true); return; }
            Output.assign(Header.begin(),Header.end()); Output[6]=0x80;
            if(response)
            {
                auto payload=EncodeResponse(*response); if(payload.Bytes.size()>MaxFrame) { Close(true); return; }
                Cluster::Writer size; size.U32(std::uint32_t(payload.Bytes.size())); Output.insert(Output.end(),size.Bytes.begin(),size.Bytes.end());
                Output.insert(Output.end(),payload.Bytes.begin(),payload.Bytes.end());
            }
            ++Owner.Counters.Requests; auto self=shared_from_this();
            boost::asio::async_write(Stream,boost::asio::buffer(Output),[self](auto ec,std::size_t){self->Close(bool(ec));});
        }
        void Start()
        {
            auto self=shared_from_this(); Deadline.expires_after(std::chrono::seconds(Owner.Config.TimeoutSeconds));
            Deadline.async_wait([self](auto ec){if(!ec) self->Close(true);});
            Stream.async_handshake(boost::asio::ssl::stream_base::server,[self](auto ec)
            {
                if(self->Closed) return;
                if(ec || !self->Authorized()) { self->Close(true); return; }
                boost::asio::async_read(self->Stream,boost::asio::buffer(self->Header),[self](auto error,std::size_t)
                {
                    if(self->Closed) return;
                    auto const& h=self->Header;
                    std::uint32_t realm=(std::uint32_t(h[12])<<24)|(std::uint32_t(h[13])<<16)|(std::uint32_t(h[14])<<8)|h[15];
                    auto scope=self->Owner.Config.WorldRealms.find(self->Identity);
                    if(error || !self->Owner.Ready || !self->Owner.TlsHealthy || h[0]!='S'||h[1]!='F'||h[2]!='B'||h[3]!='G'||h[4]||h[5]!=1||h[6]||
                        (h[7]!=1&&h[7]!=2)||!(h[8]|h[9]|h[10]|h[11])||!realm||!self->Owner.Config.Realms.count(realm)||
                        scope==self->Owner.Config.WorldRealms.end()||!scope->second.count(realm)) { self->Close(true); return; }
                    if(h[7]==1) { self->Reply(); return; }
                    boost::asio::async_read(self->Stream,boost::asio::buffer(self->Length),[self,realm](auto sizeError,std::size_t)
                    {
                        if(self->Closed) return;
                        auto const& b=self->Length; auto size=(std::uint32_t(b[0])<<24)|(std::uint32_t(b[1])<<16)|(std::uint32_t(b[2])<<8)|b[3];
                        if(sizeError||!size||size>MaxFrame) {self->Close(true);return;} self->Body.resize(size);
                        boost::asio::async_read(self->Stream,boost::asio::buffer(self->Body),[self,realm](auto bodyError,std::size_t)
                        {
                            if(self->Closed) return;
                            Snapshot snapshot; Response response;
                            if(bodyError||!self->Owner.Ready||!self->Owner.TlsHealthy||!DecodeSnapshot(self->Body,snapshot)||snapshot.Realm!=realm)
                            {self->Close(true);return;}
                            if(!self->Owner.Queues.Accept(self->Identity,snapshot,Now(),response))
                            {
                                auto now=Now();
                                if(now-self->Owner.LastRejectionLog>=1000)
                                {self->Owner.LastRejectionLog=now;SF_LOG_WARN("server.battleground","Queue request rejected for realm %u: %s",realm,self->Owner.Queues.LastError().c_str());}
                                self->Close(true);return;
                            }
                            self->Owner.Counters.Proposals+=std::uint32_t(response.Plans.size()); self->Reply(&response);
                        });
                    });
                });
            });
        }
    };
    void Server::State::Accept()
    {
        Acceptor.async_accept([this](auto ec,auto socket)
        {
            if(!ec && !Stopping)
            {
                if(Sessions.size()<Config.MaxConnections)
                { auto session=std::make_shared<Session>(*this,std::move(socket)); Sessions.insert(session); session->Start(); }
                else { ++Counters.Failures; boost::system::error_code ignored; socket.close(ignored); }
            }
            if(!Stopping) Accept();
        });
    }
    Server::Server()=default;
    Server::~Server(){Stop();}
    bool Server::Open(Options options,Cluster::AgentOptions const& tls,std::string& error)
    {
        if(_state||!options.Port||!options.MaxConnections||options.MaxConnections>128||!options.TimeoutSeconds||options.TimeoutSeconds>30||
            options.Realms.empty()||options.Realms.size()>64||options.Realms.count(0)||options.WorldKeys.empty()||options.WorldKeys.size()>128)
        {error="Invalid battleground listener limits, realms or world allowlist.";return false;}
        for(auto const& key:options.WorldKeys)
            if(!Cluster::ValidKey(key)||!options.WorldRealms.count(key)) {error="Every allowed world requires an explicit realm scope.";return false;}
        for(auto const& scope:options.WorldRealms)
        {
            if(!options.WorldKeys.count(scope.first)||scope.second.empty()) {error="Invalid world realm scope.";return false;}
            for(auto realm:scope.second) if(!options.Realms.count(realm)) {error="Unserved world realm scope.";return false;}
        }
        try
        {
            auto state=std::make_unique<State>(); state->Config=std::move(options);state->Credentials=tls;
            state->Counters.Realms.assign(state->Config.Realms.begin(),state->Config.Realms.end());
            if(!SSL_CTX_set_min_proto_version(state->Tls.native_handle(),TLS1_2_VERSION)) throw std::runtime_error("TLS version");
            state->Tls.set_verify_mode(boost::asio::ssl::verify_peer|boost::asio::ssl::verify_fail_if_no_peer_cert);
            state->Tls.load_verify_file(tls.CA); state->Tls.use_certificate_chain_file(tls.Certificate);
            state->Tls.use_private_key_file(tls.PrivateKey,boost::asio::ssl::context::pem);
            if(!SSL_CTX_check_private_key(state->Tls.native_handle())) throw std::runtime_error("TLS key");
            boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::make_address(state->Config.Address),state->Config.Port);
            state->Acceptor.open(endpoint.protocol());state->Acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
            state->Acceptor.bind(endpoint);state->Acceptor.listen(128);state->NextTlsReload=Now()+30000;_state=std::move(state);_state->Accept();return true;
        }
        catch(std::exception const&) {Stop();error="Cannot open battleground TLS listener; check endpoint and certificates.";return false;}
    }
    void Server::SetReady(bool ready)
    {
        if(!_state) return;
        if(_state->Ready && !ready) _state->Queues.ClearQueues();
        _state->Ready=ready;
        if(!ready) while(!_state->Sessions.empty()) (*_state->Sessions.begin())->Close();
    }
    void Server::Update()
    {
        if(!_state) return;
        auto now=Now();
        if(now>=_state->NextTlsReload)
        {
            _state->NextTlsReload=now+30000;
            try
            {
                boost::asio::ssl::context refreshed(boost::asio::ssl::context::tls_server);
                if(!SSL_CTX_set_min_proto_version(refreshed.native_handle(),TLS1_2_VERSION)) throw std::runtime_error("TLS version");
                refreshed.set_verify_mode(boost::asio::ssl::verify_peer|boost::asio::ssl::verify_fail_if_no_peer_cert);
                refreshed.load_verify_file(_state->Credentials.CA);refreshed.use_certificate_chain_file(_state->Credentials.Certificate);
                refreshed.use_private_key_file(_state->Credentials.PrivateKey,boost::asio::ssl::context::pem);
                if(!SSL_CTX_check_private_key(refreshed.native_handle())) throw std::runtime_error("TLS key");
                _state->Tls=std::move(refreshed);_state->TlsHealthy=true;
            }
            catch(std::exception const&)
            {
                _state->TlsHealthy=false;_state->Queues.ClearQueues();
                while(!_state->Sessions.empty()) (*_state->Sessions.begin())->Close();
                SF_LOG_ERROR("server.battleground","Cannot refresh battleground TLS files; listener unavailable until corrected.");
            }
        }
        _state->Queues.Expire(now);
        for(unsigned i=0;i<64&&_state->Io.poll_one();++i){}
    }
    bool Server::Ready() const {return _state && _state->Ready && _state->TlsHealthy;}
    void Server::Stop()
    {
        if(!_state) return; _state->Stopping=true; _state->Ready=false;
        boost::system::error_code ignored;_state->Acceptor.close(ignored);
        while(!_state->Sessions.empty()) (*_state->Sessions.begin())->Close();
        _state->Io.stop();_state.reset();
    }
    Cluster::BattlegroundMetrics Server::Metrics() const
    {
        if(!_state) return {}; auto result=_state->Counters;
        result.Uptime=std::uint32_t((Now()-_state->Started)/1000);result.Connections=std::uint32_t(_state->Sessions.size());
        result.QueuedGroups=std::uint32_t(_state->Queues.QueuedGroups());result.QueuedPlayers=std::uint32_t(_state->Queues.QueuedPlayers());return result;
    }
}
