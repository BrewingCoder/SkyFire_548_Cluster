# Clustered chat and social ownership

The native chat daemon routes realm-scoped player messages and owns channel state
and guild social administration when the authority flags are enabled. Worldserver
retains client packet encoding, player visibility, spatial/group gameplay checks,
script callbacks, RBAC and guild-bank gameplay. Character service owns durable
social records and updates the legacy guild read/economy projection atomically.

## Offline cutover

Use matching worldserver, chatserver and character-service versions on every node.
Keep all worlds and social daemons for the realm stopped throughout the import.
Back up the character database and configurations first.

1. Apply the character social migrations through the existing database update
   workflow: `001_social_domain_store.sql`, `002_social_guild_membership.sql`,
   `003_social_consumed_items.sql` and `004_social_failover.sql` (or their promoted
   release names). Existing authority stores need the new migration, not a reimport.
2. Run `python -B import_social.py --config characterserver.toml --offline` from
   the character-service directory. The default is a dry run and rolls back.
3. Resolve any reported invalid legacy rows. Repeat with `--apply` to import both
   domains together. The importer requires InnoDB, the database ownership lock
   and an empty destination; it does not overwrite an existing social snapshot.
4. Add the chat certificate node key to character `allowed_chat_nodes` and set
   `social_guild_projection = true`. Chat and world certificate identities must
   be distinct. Enable `Chat.Persistence.Enable` and configure the numeric TLS
   character endpoint and certificate identity for every `Chat.Realms` entry.
5. Enable `ChatService.Enable`, `ChatService.Messages`,
   `Chat.ChannelAuthority.Enable` and `Chat.GuildAuthority.Enable` in every world
   configuration for the realm. `CharacterService.Enable` is also required.
6. Start character service, then chatserver. Wait for persistence readiness before
   starting the active world and its standby. Do not resume legacy writers after
   importing: they would make the staged snapshot stale.

Authority flags are fixed for a world process lifetime. Changing ownership requires
a restart; a live configuration reload cannot switch back to legacy writers.

New installation defaults remain disabled so an existing deployment cannot
silently switch database ownership during an upgrade. No CI promotion changes are
part of this implementation. Setup notes and the shipped migration contract live
here; machine-specific credentials and working notes must not be committed.

## Authority and boundaries

Every world request is bound to its TLS certificate, configured realm, process
presence generation, account and player-session incarnation. Administrative
console requests use an explicit system identity from an authorized world;
player GM commands carry the action-specific RBAC check. These trusted-service
APIs must not be exposed directly to game clients.

Channel authority covers custom and built-in membership, ownership, moderation,
mute, bans, passwords, invitations, announcements, lists and messages. Passwords
are stored as salted PBKDF2-SHA256 verifiers; password work is bounded and runs off
the daemon event loop. Runtime membership and moderator flags are durable and
bound to world/session generations. Remote invitation recipients attest their
current visibility and faction permissions before an invitation is completed.

Guild authority covers membership/invitations, ranks, leader changes, notes,
MOTD/info, creation/disbanding, GM administrative operations and guild/officer
messages. Creation validates and consumes a petition and charter in the same
transaction as the new guild. Character deletion and faction changes update the
social membership and leader succession inside their character transaction.
Rank changes preserve the matching bank limits and rights. Rank permission edits
commit their social and bank limits together. Disbanding removes
bank item data atomically; world projections apply the corresponding in-memory
cleanup only after confirmation.

Guild-bank transactions, achievements/progression, recruitment finder gameplay,
calendar gameplay and inventory simulation remain with world/character services.
Chat does not become a generic SQL or raw client-opcode tunnel. Gameplay GM
commands still execute on the authoritative world as the original actor.

Remote whispers use recipient-world permission checks and a delivery
acknowledgment. Group messages use the gameplay world's roster and require a
matching gameplay group on the receiving world. Chat routing does not replicate
or create gameplay groups. Cross-world script callbacks cannot receive a remote
`Player*`; they use the sender-only chat hook.

## Persistence and recovery

The character service retains its database advisory lock. A shared realm lease
fences both channel and guild ownership to one chat certificate node and process
incarnation. The database uses a 15-second SQL-time deadline and a monotonically
increasing ownership epoch. Every social operation checks and renews its lease;
a replacement may acquire an expired lease even if the former socket has not
detached. Operations from the old epoch are rejected. A process reattaching after
expiry must acquire a new epoch and rebuild both domains.

Native readiness has a conservative 10-second budget measured from the start of
the last successful lease-bearing request. Lease loss clears volatile presence,
mailboxes, invitations and pending callbacks; no realm serves requests until its
snapshot and recovery work complete. Overall readiness requires at least one
owned realm, but probes and operations always verify their specific realm. A
healthy daemon waiting behind another owner reports standby. See
[ChatServer.md](ChatServer.md) for the two-endpoint setup.

A mutation commits its record, CAS revision, idempotency receipt, ordered outbox
and relevant guild projection/index in one transaction. A unique realm/GUID index
prevents membership in two guilds. Consumed-charter records prevent a delayed save
from resurrecting an item during the same world incarnation. Economic fields are
never accepted as arbitrary social-document fields.

Unknown commit outcomes are not replayed as new commands or executed locally.
After a lost backend reply, the daemon reconnects under the ownership fence and
checks its original durable receipt. Guild mutations also atomically store a
command outcome bound to realm, world certificate node, world generation,
sequence, account, actor, session incarnation and guild. That immutable outcome
contains the original revision and projection, including deletion.

World guild projections retain callbacks while querying uncertain commands.
Status lookup works after the chat daemon dies or a standby takes over, including
when the original actor has logged out; the authenticated world generation and
original command identity still must match. A missing lookup writes a terminal
not-executed record under the owner lock, preventing a delayed original mutation
from committing later. A world restart is not required to recover the outcome.

Bank changes remain fenced while a social change is pending or uncertain. The
world will not replace a newer observed projection with an older recovered one;
after confirmed recovery it refreshes current state before releasing its fence.
If the backend is unavailable, the operation remains uncertain and fenced rather
than being guessed or replayed. Outcomes have no time-based expiry while they may
still be needed by a live world generation.

Queued legacy bank writes lock and validate their guild/rank/member references,
so a late transaction cannot recreate a disbanded guild's rows. These economic
requests still rely on world authorization; their legacy SQL envelope does not
carry a social revision or full actor permission proof.

The event transport is bounded, uses acknowledged cursors and resets cursors on
mailbox epoch changes after daemon restart. Transient chat messages expire instead
of being delivered late. A timeout can mean delivery was not confirmed; callers
must not blindly retry a possibly delivered message.

## Character-service wire contract

The existing TLS framing is u32 payload size, followed by the payload. Replies
start with status byte 0 (success), 1 (rejection), or 2 (another live lease owner
holds the requested realm at attach). Strings use u32 byte lengths.
World character RPC remains version 2. Social operations are:

- Handshake 16: version 1, realm, 32-hex process incarnation, domain.
- Snapshot 17: JSON `after` key and page `limit` (maximum 32).
- Mutation 18: JSON `id`, `key`, `expected`, `actor`, `document`, and optional
  typed guild `context` for lifecycle/administrative proofs and optional `command`
  identity for durable guild outcomes. Response is the committed revision. Actor
  zero is reserved for attested console operations.
- Outbox 19: JSON `after` revision and bounded `limit`.
- Receipt 20: JSON `id`; returns `found` and, when present, the committed revision.
  Lookup is fenced to the current owner epoch and serialized against mutations.
- Command outcome 21: JSON `node`, `generation`, `sequence`, `account`, `actor`,
  `incarnation` and `guild`. Realm/domain come from the authenticated connection.
  A committed response contains `found: true`, `revision` and the original
  `document`; a terminal not-executed response contains `found: false`. Identity
  mismatches are rejected. A later mutation using either recorded identity is
  rejected; callers use lookup, not command replay.

Duplicate JSON keys, unsupported fields, invalid UTF-8, out-of-range integers and
oversized documents are rejected. Documents are limited to 256 KiB; the native
cache is bounded to 8,192 records/4 MiB per realm/domain.

Channel documents contain name, team, channel ID, announcement/ownership flags,
password verifier and bans. Optional runtime data stores owner and member
identity/incarnation/role information. Legacy records without runtime data remain
readable. Channel keys use SHA256 of decimal team, a colon, and the name folded
with the existing `wcharToLower` rules; Unicode casefold would incorrectly merge
some legacy names.

Guild documents contain ID, name, leader, MOTD/info, rank names/rights and member
ranks/notes. The leader is the sole rank-zero member. Text respects the legacy
utf8mb3 column and client packet limits. Guild keys are `guild-<id>`; realm is the
outer database partition.

## Verification

Native CTest targets cover channel/guild policy, protocol envelopes, presence,
routing, whispers and snapshot recovery. `character_service_test.py` accepts
`--database-config` and uses disposable databases. It exercises receipt rollback,
guild uniqueness, writer fencing, charter creation, rank bank-right migration,
character deletion and disband cleanup against the shipped schema.

`chat_service_test.py --chatserver <native-executable> --database-config <fixture-config>`
starts a private mTLS hub fixture, the real native chat daemon and a character
service with a disposable schema. It tests cross-world routing, authorization,
durable channel/guild changes and daemon-restart recovery. It does not drive an
interactive game client or reuse player credentials.
