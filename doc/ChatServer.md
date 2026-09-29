# Clustered chat daemon

The native chat daemon owns realm-scoped chat routing, channel membership and
moderation, and guild social administration when the authority options are enabled.
Character service commits social records, command outcomes and the legacy guild
projection in one transaction. Worlds retain client packet encoding, spatial and
group gameplay checks, visibility, ignore filters, RBAC, script callbacks and guild
bank/item gameplay. Guild recruitment remains world-owned.

Guild/officer messages, whispers, party/group messages and channels use the chat
transport. Addon payloads preserve their byte contents and prefix checks. Guild
invitations can cross world connections within a realm; the receiving world checks
faction, ignore, auto-decline and current guild/invitation state. A receiver without
the guild in its catalog rejects the invitation. Current gameplay fallback uses
one active world writer per realm, with the standby loading state on promotion.

## Build and initial cutover

Build and install matching hubserver, chatserver and worldserver binaries and the
character-service scripts. Chatserver uses the existing Boost/OpenSSL/shared
libraries and requires neither the game library nor DBC/DB2 files. CMake INSTALL
ships `chatserver.conf.dist`; existing live configurations are not overwritten.
Compare templates before updating their `ConfVersion` values.

For a first authority cutover, stop the realm's worlds and social daemons, back up
the character database, and follow [SocialPersistence.md](SocialPersistence.md).
Apply the character social migrations `001_social_domain_store.sql`,
`002_social_guild_membership.sql`, `003_social_consumed_items.sql`, and
`004_social_failover.sql` (or their promoted release names). Import legacy social
state once with the offline importer; do not reimport an existing authority store.
An already migrated deployment only needs the new pending migrations.

Set character `social_guild_projection = true` and include every authorized chat
certificate identity in `allowed_chat_nodes`. This allowlist is separate from
world identities. Enable these world settings after the migration:

```ini
ChatService.Enable = 1
ChatService.Messages = 1
Chat.ChannelAuthority.Enable = 1
Chat.GuildAuthority.Enable = 1
```

`CharacterService.Enable` must also be enabled. Authority settings are fixed for
the world process lifetime; switching them requires a restart. Do not resume
legacy social writers after import.

## Chat configuration and realm authorization

Each daemon needs a unique `Cluster.NodeKey` and matching certificate identity.
Its certificate needs client authentication for hub/character connections, server
authentication for world connections, and a SAN matching its advertised endpoint.
Configure the hub endpoint, CA, `Chat.BindIP`, `Chat.Port` and
`Cluster.AdvertiseAddress`. Certificate paths resolve relative to the chat config.

`Chat.Realms = "1 2"` supports up to 64 positive realm IDs. Configure
`Chat.AllowedWorlds` with exact world certificate identities and
`Chat.WorldRealms = "world-primary=1 world-other=2"` with their permitted realms.
An allowed identity without a realm scope can probe but cannot publish players.
Standby world identities need their own entries. These permissions do not grant
player or GM privileges.

Enable `Chat.Persistence.Enable = 1`. For each realm configure the same durable
character endpoint on every chat node that can serve that realm:

```ini
Chat.Persistence.Realm.1.Host = "127.0.0.1"
Chat.Persistence.Realm.1.Port = 54930
Chat.Persistence.Realm.1.NodeKey = "characters-1"
```

Use a numeric character endpoint address and a matching certificate SAN. Worlds
connect through `ChatService.Host`, `ChatService.Port` and
`ChatService.NodeKey`, reusing their own cluster certificate and CA. `RealmID`
must be positive. Warm standby worlds publish presence only after activation.

## Chat standby

Run a second chat process with its own configuration, unique node key, certificate,
listener port and hub managed-service key. Both processes use the same durable
character backend for each shared realm, and both identities belong in that
backend's `allowed_chat_nodes`. Give both chat configurations equivalent world
allowlists and realm scopes. They must not use separate copies of the social DB.

Configure the world with both exact endpoint identities, for example:

```ini
ChatService.Host = "chat-primary.example.net"
ChatService.Port = 54940
ChatService.NodeKey = "skyfire-chat-primary"
ChatService.Standby.Host = "chat-standby.example.net"
ChatService.Standby.Port = 54941
ChatService.Standby.NodeKey = "skyfire-chat-standby"
```

An empty standby host disables endpoint failover. World probes validate readiness
for their own realm, then publish fresh presence before sending work to the selected
endpoint. A reachable listener alone is not readiness. The backend grants one
15-second SQL-time lease per realm across the channel and guild domains. Native
readiness expires conservatively 10 seconds from the start of the last successful
lease-bearing request. Failed fencing withdraws readiness and old sessions; a new
owner reloads both domains before serving requests. Healthy lease contention is
reported as standby; network, certificate and configuration errors remain failures.

Ownership is per realm: two chat nodes may own different realm subsets. Overall
readiness means at least one owned realm; every probe and operation still checks
its specific realm. There is no simultaneous ownership of one realm's social data.
Failover includes lease expiry, reconnect and presence publication, so messages in
flight can fail instead of being delivered twice. Do not blindly retry chat text or
administrative mutations.

## Durable command recovery

Guild mutations store an immutable outcome in the same transaction as their social
and legacy projections. The identity includes the authenticated world node, realm,
world generation, sequence, account, actor and session incarnation. A world that
loses the reply retains its callback and fences the affected guild's economic
operations. It queries the original outcome even after the chat process dies or a
standby takes over. It never replays the mutation to discover its outcome.

A missing lookup writes a terminal not-executed record under the ownership fence,
which prevents a delayed original request from committing afterward. A committed
lookup returns its original revision and projection, including deletion. The world
will not replace a newer observed projection with that historical response, and
refreshes current state before releasing the recovery fence. Consumed charter
protection prevents stale inventory saves from resurrecting a committed charter.
If the backend cannot establish an outcome, the guild remains fenced until it can.

## Hub operation and validation

Register each daemon as a managed service with `service_kind=5`, a unique service
key and the matching `cluster_key`, executable, configuration and working directory.
Reload hub DB records, then use web Start/Stop/Restart or the corresponding console
commands. Coordinated restart includes managed chat daemons. The service launch
token and configured certificate identity are checked independently.

The health interface reports uptime, configured realms, connections, requests,
presence and routing counters. Expired metrics are unavailable, not zero. Test
normal and addon chat, moderation, guild changes and GM authorization after cutover;
then stop the active chat node while leaving the world running. Verify takeover,
fresh presence, unchanged guild revision for an uncertain mutation, and recovery
without replay. Automated fixtures exercise SQL lease fencing, outcome tombstones,
native mutual TLS routing and lost-reply recovery; retain application logs when
investigating a client-visible failure.
