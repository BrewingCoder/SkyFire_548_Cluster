# mod-ah-bot — Auction House Bot

Fakes a living auction-house economy for small / family servers, where there
aren't enough real players to populate the AH. Two independent halves, both
knob-driven via `conf/ahbot.conf.dist`:

- **Seller** — keeps each enabled house topped up to a target number of live
  listings, pricing each at `item vendor SellPrice × stack × PriceMultiplier`.
- **Buyer** — sweeps *player* listings every cycle and buys out anything priced
  at or below `(SellPrice × count) × Buyer.PriceMultiplier`, so players can
  always offload loot for gold. Bought items land in the bot character's mailbox.

## Setup

1. **Create a bot character** (any level; park it and log out). This character
   owns the bot's auctions and receives the gold/items it buys. Set its GUID in
   `AuctionHouseBot.Character.GUID` (preferred) or its name in `.Character.Name`.
2. **Have auctioneer NPCs spawned** for each house you enable. The module
   auto-discovers one spawned auctioneer per faction from the world DB — no
   manual GUID config needed. (Attaching listings to a real auctioneer is what
   lets them survive a server restart.)
3. Set `AuctionHouseBot.Enable = 1` and restart the worldserver (or
   `.reload config` + `.ahbot reload`).

## In-game commands (`.ahbot …`)

| Command          | Effect                                               |
|------------------|------------------------------------------------------|
| `.ahbot status`  | Show enable state, houses, pool size, live counts.   |
| `.ahbot on/off`  | Toggle at runtime (not persisted).                   |
| `.ahbot run`     | Force one seller+buyer cycle immediately.            |
| `.ahbot reload`  | Re-read config and re-initialize on next cycle.      |

## Pricing basis

Vendor `SellPrice` is the single source of truth for value (per project design
decision). It's always present and predictable. A future enhancement could add a
quality/ilvl-weighted model, but vendor-based keeps v1 simple and stable.

## Notes

- The bot never buys its own listings.
- With `AllowTwoSide.Interaction.Auction` enabled, all three houses collapse to
  the Neutral house and the bot operates only there.
- Bought items accumulate in the bot's mailbox; purge periodically if desired.
