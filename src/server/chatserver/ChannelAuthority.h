/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_CHANNEL_AUTHORITY_H
#define SKYFIRE_CHANNEL_AUTHORITY_H
#include "Cluster/ChatService.h"
#include "SocialPersistence.h"
#include <memory>
namespace Skyfire::Chat
{
    class ChannelAuthority
    {
    public:
        using Completion = std::function<void(ServiceResponse)>;
        using Emit = std::function<bool(std::uint32_t,std::vector<ServiceEvent>)>;
        ChannelAuthority(PresenceDirectory& presence, SocialPersistence& persistence, Emit emit);
        ~ChannelAuthority();
        void Handle(std::uint32_t realm, std::string const& node, ServiceRequest const& request, Completion complete);
        void Update(std::uint64_t now, std::vector<SocialPersistence::Result> const& results);
    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
#endif
