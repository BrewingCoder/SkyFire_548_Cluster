# Changelog

## 1.1.0 — 2026-10-08

### Fixed
- **Builds on stock SkyFire again.** 1.0.0 called `Creature::SetCorpseRemoveTime`,
  which doesn't exist in the upstream core, so it only compiled on a modified
  core. That call is gone; the module now uses stock hooks only.
- **Looting no longer respawns instance mobs.** In 1.0.0, corpse persistence
  stretched the corpse timer from a script hook, but looting a corpse runs the
  core's looted-decay logic, which subtracted that stretched time from the
  respawn timer and brought the mob back instantly. Corpse persistence is now a
  core patch (`patches/0001`) that handles both the kill and the loot path.
  Pet and guardian killing blows are covered too.
- **Raids are now scaled.** SkyFire's `IsDungeon()` excludes raids, so
  `Solocraft.Raids` had no effect. The module now checks `IsDungeon() || IsRaid()`.
- `.solocraft status` reads the scaled-player table under its lock.

### Added
- `Solocraft.DamageMultiplier`: scales main-hand, off-hand and ranged weapon
  damage with the group-size multiplier. Without it, melee and ranged damage
  barely moved because weapon damage is an item property that stat scaling
  never touches.
- `Solocraft.DamageTakenReduction` (with `Solocraft.UseAura` as its switch):
  removes a percentage of every incoming hit for scaled players, all schools,
  via `UnitScript::OnDamage`.
- `Solocraft.ArmorMultiplier`: flat armor multiplier while scaled.
- `Solocraft.PreventDurabilityLoss` and `patches/0002`: optional server-wide
  durability-loss switch.
- Every scale/skip decision is logged to `misc` at INFO.
- `.solocraft status` works from the server console and shows the new settings
  plus your own applied bonuses.
- Full documentation: README, commented `solocraft.conf.dist`, this changelog.

### Changed
- `Solocraft.InstanceCorpsePersist` now defaults to `0` and requires
  `patches/0001`.

## 1.0.0 — 2026-10-05

Initial public release: port of AzerothCore `mod-solocraft` to SkyFire 5.4.8.
Primary-stat and health scaling driven by `InstanceMap::GetMaxPlayers()`,
`Solocraft.HealthMultiplier`, `Solocraft.InstanceCorpsePersist`, and the
`.solocraft` command.
