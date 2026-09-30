/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#ifndef SKYFIRE_BATTLEGROUND_MESSAGE_H
#define SKYFIRE_BATTLEGROUND_MESSAGE_H

#include <string>

namespace Skyfire
{
    inline std::string FormatBattlegroundPlayerName(std::string text, std::string const& name)
    {
        if (name.empty())
            return text;
        std::size_t position = 0;
        while ((position = text.find("$n", position)) != std::string::npos)
        {
            text.replace(position, 2, name);
            position += name.size();
        }
        return text;
    }
}

#endif
