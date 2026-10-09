# mod-solocraft for SkyFire 5.4.8

A **Solocraft** module for the **SkyFire 5.4.8** emulator (Mists of Pandaria,
client build 18414).

When a player enters a dungeon or raid with fewer people than it was built for,
the module scales them up so a solo player or a small group (a family server,
say) can clear content designed for a full group. The scaling is applied on
entry and removed on exit.

It is a port of AzerothCore's `mod-solocraft`, rebuilt around SkyFire's APIs and
extended for real solo play: weapon-damage scaling, an all-school incoming
damage reduction, extra health and armor knobs, and two optional quality-of-life
core patches.

- [Features](#features)
- [How scaling works](#how-scaling-works)
- [Installation](#installation)
- [Configuration](#configuration)
- [Commands](#commands)
- [Tuning guide](#tuning-guide)
- [Logging and troubleshooting](#logging-and-troubleshooting)
- [Limitations](#limitations)
- [Compatibility](#compatibility)
- [Changelog](#changelog)
- [Attribution and license](#attribution-and-license)

## Features

| Feature | Where it lives | Default |
|---|---|---|
| Primary-stat scaling (Str/Agi/Sta/Int/Spi) | module | on when enabled |
| Max-health scaling + extra health multiplier | module | on, extra x1.0 |
| Weapon-damage scaling (melee/ranged) | module | x1.0 (scales with group size) |
| Armor multiplier | module | off (x1.0) |
| All-school incoming damage reduction | module (`OnDamage` hook) | off (0%) |
| Dungeons and raids, every MoP difficulty | module | both on |
| `.solocraft status/on/off` commands | module | — |
| Instance corpse persistence | **core patch 0001** | off |
| No durability loss | **core patch 0002** | off |

The module itself needs **no core changes**. The two core patches are optional
and only needed for the two quality-of-life features marked above.

## How scaling works

Each time a player enters a map or logs in, the module clears any scaling it
applied before, then checks where they are:

```
if map is a dungeon (MAP_DUNGEON) or raid (MAP_RAID), and that type is enabled:
    maxPlayers = InstanceMap::GetMaxPlayers()     # from the map's difficulty
    players    = players in the instance, not counting GMs
    if players < maxPlayers:
        scale      = maxPlayers / players
        multiplier = 1 + (scale - 1) * Solocraft.Balance     # capped at MaxMultiplier
```

`GetMaxPlayers()` comes from the map's difficulty entry, so 5-man, 10/25-man,
LFR, Flex, Challenge Mode and legacy raids all work without a lookup table.

With that multiplier, the module applies these unit modifiers (`TOTAL_PCT` via
`Unit::HandleStatModifier`):

| Term | Scaled by |
|---|---|
| Strength, Agility, Stamina, Intellect, Spirit | `multiplier` |
| Max health (if `ScaleHealth`) | `multiplier x HealthMultiplier` |
| Armor | `ArmorMultiplier` (flat, independent of group size) |
| Main-hand, off-hand and ranged weapon damage | `multiplier x DamageMultiplier` |

Attack power, spell power and health follow from the primary stats through the
normal formulas. Secondary ratings (crit, haste, mastery) are not scaled.

The exact amounts are remembered per character, so leaving the instance or
logging out removes precisely what was added.

**Incoming damage reduction** is separate from the stat modifiers. A
`UnitScript::OnDamage` hook reduces every hit a scaled player takes by
`DamageTakenReduction` percent. `Unit::DealDamage` is the single path for melee,
spell, periodic and environmental damage, so this covers every school,
including physical.

### Why weapon damage needs its own term

A melee or hunter's damage comes mostly from the weapon's damage range, which
is an item property. Scaling primary stats raises attack power, but that's a
small part of each swing. Without `DamageMultiplier`, a x5 stat boost barely
changes physical damage. At the default of 1.0, weapon damage scales with the
group size like everything else.

## Installation

1. **Add the module.** Copy (or clone) this repository into your core as
   `modules/mod-solocraft`. The folder name matters: the SkyFire module loader
   calls `Addmod_solocraftScripts()`, derived from the folder name.

   ```
   <core>/modules/mod-solocraft/src/mod_solocraft.cpp
   <core>/modules/mod-solocraft/src/mod_solocraft_loader.cpp
   <core>/modules/mod-solocraft/conf/solocraft.conf.dist
   ```

2. **(Optional) Apply the core patches** for the quality-of-life features. Each
   patch is independent; apply only the ones you want. From the core's root:

   ```sh
   git apply modules/mod-solocraft/patches/0001-instance-corpse-persistence.patch
   git apply modules/mod-solocraft/patches/0002-prevent-durability-loss.patch
   ```

   | Patch | Files | Enables |
   |---|---|---|
   | `0001-instance-corpse-persistence.patch` | `Creature.cpp` | `Solocraft.InstanceCorpsePersist` |
   | `0002-prevent-durability-loss.patch` | `Player.cpp` | `Solocraft.PreventDurabilityLoss` |

   Both patches read the config key and default to off, so applying one
   changes nothing until you turn its key on.

3. **Re-run CMake and rebuild** the worldserver. Modules are on by default
   (`-DMODULES=1`). CMake prints `+ module: mod-solocraft` when it picks it up.

4. **Configure.** The module loader copies `solocraft.conf.dist` next to the
   worldserver, but the core does not load module config files by itself.
   Copy the `Solocraft.*` keys you want into `worldserver.conf` (or your
   config overrides) and set at least:

   ```ini
   Solocraft.Enable = 1
   ```

5. Restart the worldserver. Enter a dungeon alone; you'll get a
   `[Solocraft] scaled your stats ...` message if `Announce` is on.

## Configuration

Every key, with its default. Full descriptions are in
[`conf/solocraft.conf.dist`](conf/solocraft.conf.dist).

| Key | Default | Range | What it does |
|---|---|---|---|
| `Solocraft.Enable` | `0` | 0/1 | Master switch. |
| `Solocraft.Dungeons` | `1` | 0/1 | Scale in 5-man dungeons. |
| `Solocraft.Raids` | `1` | 0/1 | Scale in raids. |
| `Solocraft.MinLevel` | `1` | ≥1 | Don't scale players below this level. |
| `Solocraft.Announce` | `1` | 0/1 | Tell the player their multiplier on entry. |
| `Solocraft.Balance` | `0.6` | ≥0 | Fraction of full linear scaling. `1.0` = solo 5-man gets x5. |
| `Solocraft.MaxMultiplier` | `10.0` | ≥1 | Cap on the multiplier. |
| `Solocraft.ScaleHealth` | `1` | 0/1 | Scale max health and heal to full on apply. |
| `Solocraft.HealthMultiplier` | `1.0` | ≥0.1 | Extra health on top of the multiplier. |
| `Solocraft.ArmorMultiplier` | `1.0` | ≥0.1 | Flat armor multiplier while scaled. |
| `Solocraft.DamageMultiplier` | `1.0` | ≥0.1 | Weapon damage scales by multiplier x this. |
| `Solocraft.UseAura` | `1` | 0/1 | Switch for the damage reduction (historical name; no aura). |
| `Solocraft.DamageTakenReduction` | `0` | 0–95 | % of all incoming damage removed while scaled. |
| `Solocraft.InstanceCorpsePersist` | `0` | 0/1 | **Patch 0001.** Corpses and loot stay until instance reset. |
| `Solocraft.PreventDurabilityLoss` | `0` | 0/1 | **Patch 0002.** No durability loss, server-wide. |

Config is read at startup and on `.reload config`. Players who are already
scaled keep their current amounts until they next change map.

## Commands

| Command | Who | What |
|---|---|---|
| `.solocraft status` | GM (`server info` permission); works from the console | Shows the active settings, how many players are scaled, and, in game, your own applied bonuses. |
| `.solocraft on` | GM (`server set` permission) | Turns scaling on at runtime. Applies on each player's next map change. |
| `.solocraft off` | GM (`server set` permission) | Turns scaling off at runtime. Existing bonuses clear as players change map or log out. |

`on`/`off` are not saved; the config value applies again after a restart.

Example `status` output:

```
Solocraft: ENABLED | dungeons:on raids:on | balance:1.00 cap:x10.0 healthx:1.00 armorx:2.00 dmgx:1.00 | corpses:persist (core patch) | active buffs:1
  durability loss: normal | incoming-dmg reduction: ON -40% (code hook, all schools)
  You are scaled: +400% stats, +400% health, +100% armor, +400% weapon dmg.
```

## Tuning guide

Starting points we've found work well for a single, reasonably geared player:

| Goal | Settings |
|---|---|
| Dungeons feel like a normal group run | `Balance 1.0`, `DamageMultiplier 1.0`, `DamageTakenReduction 30–40` |
| Raids soloable but still dangerous | `Balance 1.0`, `MaxMultiplier 10`, `ArmorMultiplier 2.0`, `DamageTakenReduction 40` |
| More forgiving, less burst | add `HealthMultiplier 1.5` |
| Gentler, more traditional solocraft | `Balance 0.6`, everything else at defaults |

Things to keep in mind:

- **Health grows twice.** Stamina is scaled, which raises health, and
  `ScaleHealth` scales max health again. Leave `HealthMultiplier` at 1.0 until
  you've seen how tanky you already are.
- **Damage reduction is the survivability lever.** Raid bosses hit for numbers
  tuned for a tank with raid healing. Armor only helps against physical hits;
  `DamageTakenReduction` covers spells too.
- **Casters scale through Intellect.** `DamageMultiplier` only touches weapon
  damage, so a caster's damage comes from the scaled Intellect/spell power.
- **Picking up items mid-fight.** Weapon scaling is a percentage of whatever
  weapon you hold, so swapping to a weaker weapon (for example an
  encounter-provided one) lowers your damage accordingly.

## Logging and troubleshooting

Every decision is logged to the `misc` logger at INFO level, including the
cases where it decides not to scale. That makes a silent no-op easy to
diagnose:

```
[Solocraft] Name: scaled x10.0 (health x10.0) on map 550 instance 2 difficulty 4 (1/25 players)
[Solocraft] Name: no scaling on map 36 instance 5 difficulty 1 (maxPlayers=5, players=5)
[Solocraft] Name: no scaling on map 548 (raids disabled in config)
[Solocraft] Name: no scaling on map 1 (not an InstanceMap)
```

Make sure your `misc` logger level shows INFO if you want to see these.

| Symptom | Likely cause |
|---|---|
| No message on entering a dungeon | `Solocraft.Enable` is 0, the key isn't in `worldserver.conf`, or the player is below `MinLevel`. |
| Scaled in dungeons but not raids | `Solocraft.Raids = 0`. |
| Corpses still decay / durability still drops | The matching core patch isn't applied (the status line still reports the key, but nothing enforces it). |
| Damage reduction seems to do nothing | `DamageTakenReduction` is 0 or `UseAura` is 0. There is no buff icon; compare hit sizes in your combat log. |
| Multiplier didn't change when a friend joined | Scaling is set on map entry. Leave and re-enter. |

## Limitations

- Scaling is calculated on map entry and login only. It does not update when
  players join or leave mid-instance.
- Only players are scaled. Pets, guardians and companions keep their normal
  stats.
- Spell damage scales through Intellect/spell power only; there is no separate
  "all damage done" multiplier.
- The damage reduction applies only to players the module has scaled, so a
  full group gets none.
- `PreventDurabilityLoss` is server-wide, not limited to instances.

## Compatibility

Written against SkyFire 5.4.8 (build 18414) with the dynamic `modules/` loader,
and checked by building a stock SkyFire core with the module and both patches.
It uses only stock script hooks: `PlayerScript::OnMapChanged` / `OnLogin` /
`OnLogout`, `UnitScript::OnDamage`, `WorldScript::OnConfigLoad`, and
`CommandScript`.

It should build on any SkyFire 5.4.8 descendant that keeps those hooks and
`InstanceMap::GetMaxPlayers` / `Unit::HandleStatModifier`. If your core lacks
the `modules/` loader, add the two `src/` files to your scripts project and call
`AddSC_mod_solocraft()` from your script loader.

Note for anyone porting from TrinityCore/AzerothCore: on SkyFire,
`Map::IsDungeon()` is true for 5-man dungeons only. Raids are `IsRaid()`. The
module checks both.

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## Attribution and license

Port of the **AzerothCore `mod-solocraft`** module
(<https://github.com/azerothcore>). Original concept and implementation by the
AzerothCore community and the module's authors. This port adapts it to
SkyFire's API, makes scaling difficulty-agnostic via `GetMaxPlayers()`, and adds
the features listed above.

Licensed under the **GNU Affero General Public License v3.0**, the same license
as the original module. See [`LICENSE`](LICENSE).
