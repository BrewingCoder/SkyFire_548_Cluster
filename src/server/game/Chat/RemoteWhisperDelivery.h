/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_REMOTE_WHISPER_DELIVERY_H
#define SKYFIRE_REMOTE_WHISPER_DELIVERY_H
#include "Cluster/ChatClient.h"
class Player;
namespace Skyfire::Chat::RemoteDelivery
{
    bool Send(Player* sender, std::string const& name, std::string const& text, std::string const& prefix = {});
    void Complete(ServiceResult const& result);
    void Receive(ServiceEvent const& event);
    void Update();
}
#endif
