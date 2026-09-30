/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "BattlegroundMessage.h"
#include <iostream>

int main()
{
    using Skyfire::FormatBattlegroundPlayerName;
    if (FormatBattlegroundPlayerName("$n captured the Horde flag!", "Walton") != "Walton captured the Horde flag!" ||
        FormatBattlegroundPlayerName("The flag was picked up by $n!", "Walton") != "The flag was picked up by Walton!" ||
        FormatBattlegroundPlayerName("$n: $n", "Walton") != "Walton: Walton" ||
        FormatBattlegroundPlayerName("Flags reset.", "Walton") != "Flags reset." ||
        FormatBattlegroundPlayerName("$n", "") != "$n" ||
        FormatBattlegroundPlayerName("$n", "Name$n") != "Name$n")
    {
        std::cerr << "Battleground player-name substitution failed.\n";
        return 1;
    }
    return 0;
}
