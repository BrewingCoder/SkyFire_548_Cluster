/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "BattlegroundClient.h"
#include "CertificateTools.h"
#include "HandoffClient.h"
#include "Config.h"
#include "Log.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <deque>
#include <map>
namespace Skyfire::BattlegroundService
{
    namespace
    {
        struct Client
        {
            Cluster::AgentOptions Options;
            std::string NodeKey, Generation;
            std::uint32_t Realm = 0;
            std::uint64_t Sequence = 0;
            std::mutex Lock;
            std::condition_variable Wake;
            using Key = std::pair<std::uint32_t,std::uint8_t>;
            struct Queued { Snapshot Request; std::chrono::steady_clock::time_point Deadline; };
            std::map<Key,Queued> Pending;
            std::deque<Result> Results;
            bool Stopping = false;
            std::thread Worker;
            ~Client()
            {
                { std::lock_guard<std::mutex> lock(Lock); Stopping = true; }
                Wake.notify_all(); if(Worker.joinable()) Worker.join();
            }
            bool VerifyIdentity(SSL* stream)
            {
                X509* certificate = SSL_get_peer_certificate(stream);
                if (!certificate) return false;
                X509_NAME const* subject = X509_get_subject_name(certificate);
                int index = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
                bool valid = false;
                if (index >= 0 && X509_NAME_get_index_by_NID(subject, NID_commonName, index) < 0)
                {
                    ASN1_STRING const* value = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, index));
                    int length = ASN1_STRING_length(value);
                    if (length > 0 && length <= 64)
                        valid = std::string(reinterpret_cast<char const*>(ASN1_STRING_get0_data(value)), std::size_t(length)) == NodeKey;
                }
                X509_free(certificate); return valid;
            }
            bool Exchange(std::uint16_t operation, Cluster::Writer const& payload, std::vector<std::uint8_t>* routed)
            {
                try
                {
                    if (payload.Bytes.size() > MaxFrame) return false;
                    boost::asio::io_context io;
                    boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
                    if (!SSL_CTX_set_min_proto_version(tls.native_handle(), TLS1_2_VERSION)) return false;
                    tls.load_verify_file(Options.CA); tls.use_certificate_chain_file(Options.Certificate);
                    tls.use_private_key_file(Options.PrivateKey, boost::asio::ssl::context::pem);
                    if (!SSL_CTX_check_private_key(tls.native_handle())) return false;
                    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(io, tls);
                    boost::asio::ip::tcp::resolver resolver(io);
                    stream.set_verify_mode(boost::asio::ssl::verify_peer);
                    stream.set_verify_callback(boost::asio::ssl::host_name_verification(Options.Host));
                    if (!SSL_set_tlsext_host_name(stream.native_handle(), Options.Host.c_str())) return false;
                    Cluster::Writer request;
                    request.U8('S'); request.U8('F'); request.U8('B'); request.U8('G');
                    request.U16(1); request.U16(operation); request.U32(1); request.U32(Realm);
                    request.U32(std::uint32_t(payload.Bytes.size()));
                    request.Bytes.insert(request.Bytes.end(), payload.Bytes.begin(), payload.Bytes.end());
                    std::vector<std::uint8_t> response(16);
                    std::array<std::uint8_t, 4> responseLength{};
                    bool success = false;
                    resolver.async_resolve(Options.Host, std::to_string(Options.Port),
                        [&](boost::system::error_code ec, auto endpoints)
                    {
                        if (ec) { io.stop(); return; }
                        boost::asio::async_connect(stream.next_layer(), endpoints, [&](boost::system::error_code connected, auto const&)
                        {
                            if (connected) { io.stop(); return; }
                            stream.async_handshake(boost::asio::ssl::stream_base::client, [&](boost::system::error_code handshake)
                            {
                                if (handshake || !VerifyIdentity(stream.native_handle()) || !Skyfire::Certificates::PeerAllowed(stream.native_handle())) { io.stop(); return; }
                                boost::asio::async_write(stream, boost::asio::buffer(request.Bytes), [&](boost::system::error_code sent, std::size_t)
                                {
                                    if (sent) { io.stop(); return; }
                                    boost::asio::async_read(stream, boost::asio::buffer(response), [&](boost::system::error_code received, std::size_t)
                                    {
                                        auto expected = request.Bytes; expected.resize(16); expected[6] = 0x80;
                                        if (received || response != expected) { io.stop(); return; }
                                        if (!routed) { success = true; io.stop(); return; }
                                        boost::asio::async_read(stream, boost::asio::buffer(responseLength), [&](boost::system::error_code lengthError, std::size_t)
                                        {
                                            std::uint32_t length = (std::uint32_t(responseLength[0]) << 24) | (std::uint32_t(responseLength[1]) << 16) |
                                                (std::uint32_t(responseLength[2]) << 8) | responseLength[3];
                                            if (lengthError || length < 2 || length > MaxFrame) { io.stop(); return; }
                                            routed->resize(length);
                                            boost::asio::async_read(stream, boost::asio::buffer(*routed), [&](boost::system::error_code bodyError, std::size_t)
                                            { success = !bodyError; io.stop(); });
                                        });
                                    });
                                });
                            });
                        });
                    });
                    // All network work runs on this worker; no DNS/TLS/socket waits on world ticks.
                    io.run_for(std::chrono::seconds(5));
                    resolver.cancel(); boost::system::error_code ignored; stream.next_layer().close(ignored);
                    return success;
                }
                catch (std::exception const&) { return false; }
            }
            void Run()
            {
                Key previous{};
                bool unavailable=false;
                for(;;)
                {
                    Queued queued;
                    {
                        std::unique_lock<std::mutex> lock(Lock);
                        Wake.wait(lock,[&]{return Stopping || (!Pending.empty() && Results.size()<64);});
                        if(Stopping) return;
                        auto next=Pending.upper_bound(previous); if(next==Pending.end()) next=Pending.begin();
                        previous=next->first; queued=std::move(next->second); Pending.erase(next);
                    }
                    Result result; result.Request=std::move(queued.Request);
                    std::vector<std::uint8_t> bytes;
                    result.Success=std::chrono::steady_clock::now()<queued.Deadline &&
                        Exchange(2,EncodeSnapshot(result.Request),&bytes) && DecodeResponse(bytes,result.Matches) &&
                        result.Matches.Sequence==result.Request.Sequence && std::chrono::steady_clock::now()<queued.Deadline;
                    if(!result.Success && !unavailable)
                        SF_LOG_WARN("server.battleground","Battleground matchmaking unavailable for type %u bracket %u; queues are waiting for a fresh proposal.",
                            result.Request.Type,unsigned(result.Request.Bracket));
                    else if(result.Success && unavailable)
                        SF_LOG_INFO("server.battleground","Battleground matchmaking recovered; fresh queue proposals are available.");
                    unavailable=!result.Success;
                    // Proposals can cause invitations; never replay a transmitted snapshot.
                    std::lock_guard<std::mutex> lock(Lock); Results.push_back(std::move(result));
                }
            }
        };
        std::unique_ptr<Client> Active;
    }
    bool StartClient(Cluster::AgentOptions options,std::string& error)
    {
        if(!sConfigMgr->GetBoolDefault("BattlegroundService.Enable",false)) return true;
        auto client=std::make_unique<Client>();
        int realm=sConfigMgr->GetIntDefault("RealmID",0),port=sConfigMgr->GetIntDefault("BattlegroundService.Port",54950);
        options.Host=sConfigMgr->GetStringDefault("BattlegroundService.Host","127.0.0.1");
        client->NodeKey=sConfigMgr->GetStringDefault("BattlegroundService.NodeKey","skyfire-battleground-primary");
        if(Active || !options.Enabled || realm<=0 || port<1 || port>65535 || options.Host.empty() || options.Host.size()>255 ||
            !Cluster::ValidUtf8(options.Host) || !Cluster::ValidKey(client->NodeKey))
        { error="Invalid battleground service TLS, realm, endpoint or identity configuration."; return false; }
        options.Port=std::uint16_t(port); client->Options=std::move(options); client->Realm=std::uint32_t(realm);
        client->Generation=Cluster::Handoff::RandomToken();
        if(client->Generation.empty()) { error="Cannot generate battleground world incarnation."; return false; }
        try { client->Worker=std::thread([ptr=client.get()]{ptr->Run();}); }
        catch(std::exception const&) { error="Cannot start battleground client worker."; return false; }
        Active=std::move(client); return true;
    }
    void StopClient() { Active.reset(); }
    bool Enabled() { return bool(Active); }
    std::uint64_t Submit(Snapshot snapshot)
    {
        if(!Active) return 0;
        std::lock_guard<std::mutex> lock(Active->Lock);
        if(Active->Stopping || (snapshot.Realm && snapshot.Realm!=Active->Realm)) return 0;
        snapshot.Realm=Active->Realm; snapshot.Generation=Active->Generation; snapshot.Sequence=++Active->Sequence;
        if(!ValidSnapshot(snapshot)) return 0;
        Client::Key key{snapshot.Type,snapshot.Bracket};
        if(!Active->Pending.count(key) && Active->Pending.size()>=64) return 0;
        auto sequence=snapshot.Sequence;
        Active->Pending[key]={std::move(snapshot),std::chrono::steady_clock::now()+std::chrono::seconds(10)};
        Active->Wake.notify_one(); return sequence;
    }
    std::vector<Result> TakeResults()
    {
        std::vector<Result> results; if(!Active) return results;
        std::lock_guard<std::mutex> lock(Active->Lock);
        while(!Active->Results.empty() && results.size()<32) { results.push_back(std::move(Active->Results.front())); Active->Results.pop_front(); }
        Active->Wake.notify_one(); return results;
    }
}
