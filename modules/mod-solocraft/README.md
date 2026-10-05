# mod-solocraft — SkyFire 5.4.8 port

Scales a player's stats up when they enter an **under-manned instance**, so a
solo player or a small group (e.g. a family server) can clear dungeon and raid
content designed for a full group. The buff is applied on entering an instance
and removed on leaving.

## How it works

On `OnMapChanged` (and `OnLogin`), if the player is in a dungeon/raid instance:

```
scale      = InstanceMap::GetMaxPlayers() / players_in_instance
multiplier = 1 + (scale - 1) * Solocraft.Balance      (capped at MaxMultiplier)
```

The multiplier is applied to the five primary stats (and, optionally, max
health) via `Unit::HandleStatModifier(..., TOTAL_PCT, ...)`, which cascades to
attack power / spell power / health through the normal stat formulas. The exact
amount is tracked per-character and removed on leaving the instance, logging
out, or regrouping. Because `GetMaxPlayers()` comes from the map's difficulty
entry, **all MoP difficulties** (5-man, 10/25, LFR, Flex, Challenge) are handled
automatically with no hardcoded table.

### Extra health (survivability knob)

`Solocraft.HealthMultiplier` applies an **additional** multiplier to max health
*on top of* the core multiplier, leaving primary stats (and therefore damage) at
the core multiplier. Example: at a core ×5 with `HealthMultiplier = 1.5`, stats
are ×5 but health is ×7.5. This follows the modern-WoW principle for solo/scaled
content — you still have to work your rotation and the mechanics, you're just
given a longer window to do it. `1.0` = no extra health.

### Instance corpse persistence

`Solocraft.InstanceCorpsePersist` keeps killed mob corpses (and their loot) in an
instance until the instance resets, instead of the normal ~60s decay. Soloing a
dungeon means you often **clear a room before looting**, or **die and run back** —
both of which lose loot to corpse decay on a normal server. With this on, unlooted
corpses wait for you; looted corpses still decay normally. It hooks `OnCreatureKill`,
so it applies to **direct player kills** (a pet/guardian killing blow uses a
different code path and is not covered).

Config: see `conf/solocraft.conf.dist`. In-game: `.solocraft status|on|off`.

> Note: on this server the knobs are set via the worldserver config/overrides
> because module `.conf.dist` files are not auto-loaded by the core.

## Attribution & license

This is a **port of the AzerothCore `mod-solocraft`** module
(<https://github.com/azerothcore>) to the SkyFire 5.4.8 core. Original concept
and implementation by the AzerothCore community and the module's original
authors. This port adapts it to SkyFire's API (`PlayerScript::OnMapChanged`,
`InstanceMap::GetMaxPlayers`, `Unit::HandleStatModifier`) and uses
`GetMaxPlayers()` for difficulty-agnostic scaling.

Licensed under the **GNU Affero General Public License v3** (AGPLv3), the same
license as the original module. See the AzerothCore project for the full license
text. Changes from the original are described above.
