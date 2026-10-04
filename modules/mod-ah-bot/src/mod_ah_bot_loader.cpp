/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* Module loader. Discovered and invoked automatically by AddModulesScripts().
* The function name MUST match the folder name (mod-ah-bot -> mod_ah_bot).
*/

// Registration functions provided by this module.
void AddSC_mod_ahbot();

// Aggregate loader for the mod-ah-bot module.
void Addmod_ah_botScripts()
{
    AddSC_mod_ahbot();
}
