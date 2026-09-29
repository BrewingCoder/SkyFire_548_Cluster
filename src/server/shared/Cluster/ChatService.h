/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_CHAT_SERVICE_H
#define SKYFIRE_CHAT_SERVICE_H
#include "ChatPresence.h"
#include <functional>

namespace Skyfire::Chat
{
    constexpr std::size_t MaxServicePayload = 256 * 1024;
    constexpr std::size_t MaxServiceFrame = MaxServicePayload + 256;
    enum class ServiceDomain : std::uint8_t { Channel = 1, Guild = 2, Whisper = 3, Group = 4 };
    enum class ServiceStatus : std::uint8_t { Ok, Rejected, Unavailable, Unknown };
    struct ServiceRequest
    {
        ServiceDomain Domain = ServiceDomain::Channel;
        std::string Generation;
        std::uint64_t Sequence = 0;
        std::uint32_t Account = 0;
        std::uint64_t Actor = 0, Incarnation = 0;
        std::vector<std::uint8_t> Payload;
    };
    struct ServiceResponse
    {
        std::uint64_t Sequence = 0;
        ServiceStatus Status = ServiceStatus::Unavailable;
        std::vector<std::uint8_t> Payload;
    };
    struct ServiceEvent
    {
        ServiceDomain Domain = ServiceDomain::Channel;
        std::uint64_t Recipient = 0, Incarnation = 0;
        std::vector<std::uint8_t> Payload;
    };
    inline bool ServiceGeneration(std::string const& value)
    { return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos; }
    inline void WriteServiceBytes(Cluster::Writer& out, std::vector<std::uint8_t> const& bytes)
    { out.U32(std::uint32_t(bytes.size())); out.Bytes.insert(out.Bytes.end(), bytes.begin(), bytes.end()); }
    inline bool ReadServiceBytes(Cluster::Reader& in, std::vector<std::uint8_t>& bytes)
    {
        std::uint32_t length;
        if (!in.U32(length) || length > MaxServicePayload) return false;
        bytes.clear(); bytes.reserve(length);
        for (std::uint32_t i = 0; i < length; ++i)
        { std::uint8_t byte; if (!in.U8(byte)) return false; bytes.push_back(byte); }
        return true;
    }
    inline Cluster::Writer EncodeServiceRequest(ServiceRequest const& request)
    {
        Cluster::Writer out; out.U8(std::uint8_t(request.Domain)); out.String(request.Generation);
        Write64(out, request.Sequence); out.U32(request.Account); Write64(out, request.Actor);
        Write64(out, request.Incarnation); WriteServiceBytes(out, request.Payload); return out;
    }
    inline bool DecodeServiceRequest(std::vector<std::uint8_t> const& bytes, ServiceRequest& request)
    {
        if (bytes.size() > MaxServiceFrame) return false;
        Cluster::Reader in(bytes); ServiceRequest result; std::uint8_t domain;
        if (!in.U8(domain) || domain < 1 || domain > 4 || !in.String(result.Generation, 64) ||
            !ServiceGeneration(result.Generation) || !Read64(in, result.Sequence) || !result.Sequence ||
            !in.U32(result.Account) || !Read64(in, result.Actor) ||
            !Read64(in, result.Incarnation) || !ReadServiceBytes(in, result.Payload) || !in.End()) return false;
        if ((!result.Account || !result.Actor || !result.Incarnation) &&
            !(domain == std::uint8_t(ServiceDomain::Guild) && !result.Account && !result.Actor && !result.Incarnation)) return false;
        result.Domain = ServiceDomain(domain); request = std::move(result); return true;
    }
    inline Cluster::Writer EncodeServiceResponse(ServiceResponse const& response)
    {
        Cluster::Writer out; Write64(out, response.Sequence); out.U8(std::uint8_t(response.Status));
        WriteServiceBytes(out, response.Payload); return out;
    }
    inline bool DecodeServiceResponse(std::vector<std::uint8_t> const& bytes, ServiceResponse& response)
    {
        if (bytes.size() > MaxServiceFrame) return false;
        Cluster::Reader in(bytes); ServiceResponse result; std::uint8_t status;
        if (!Read64(in, result.Sequence) || !result.Sequence || !in.U8(status) || status > 3 ||
            !ReadServiceBytes(in, result.Payload) || !in.End()) return false;
        result.Status = ServiceStatus(status); response = std::move(result); return true;
    }
    inline void WriteServiceEvent(Cluster::Writer& out, ServiceEvent const& event)
    {
        out.U8(std::uint8_t(event.Domain)); Write64(out, event.Recipient); Write64(out, event.Incarnation);
        WriteServiceBytes(out, event.Payload);
    }
    inline bool ReadServiceEvent(Cluster::Reader& in, ServiceEvent& event)
    {
        std::uint8_t domain;
        if (!in.U8(domain) || domain < 1 || domain > 4 || !Read64(in, event.Recipient) || !event.Recipient ||
            !Read64(in, event.Incarnation) || !event.Incarnation || !ReadServiceBytes(in, event.Payload)) return false;
        event.Domain = ServiceDomain(domain); return true;
    }
    using ServiceCompletion = std::function<void(ServiceResponse)>;
    using ServiceHandler = std::function<void(std::uint32_t, std::string const&, ServiceRequest const&, ServiceCompletion)>;
}
#endif
