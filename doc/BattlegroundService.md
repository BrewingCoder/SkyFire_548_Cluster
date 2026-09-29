# Battleground matchmaking service

The first phase moves unrated battleground queue selection to the native
`battlegroundserver` process. Worldserver still admits players to queues, validates
eligibility, sends invitations, creates instances, teleports players, and runs
combat. Arenas and rated matchmaking retain their existing worldserver path.

Build and install the normal server targets. INSTALL includes `battlegroundserver`
and `battlegroundserver.conf.dist`. Copy the distribution configuration to
`battlegroundserver.conf` and assign a unique cluster node key. Authorize service
kind 6 and capability 4096 in the hub, and issue its certificate using the hub's
certificate workflow. This daemon opens no character or world database connection.

In the existing hub configuration, retain the deployed CA and enable the cluster
listener if necessary:

```ini
Hub.Cluster.Enable = 1
Hub.Port = 9100
```

The hub authenticates registration with its trusted CA and certificate role;
there is no separate hub node-allowlist configuration key. With hub PKI enabled,
use certificate role **Battleground (6)** for `skyfire-battleground-primary`.
Managed Start provisions credentials when `Hub.PKI.Enable = 1`; remote enrollment
uses the same role. See [HubCertificates.md](HubCertificates.md) for CA adoption
and enrollment. The daemon advertises capability 4096 automatically.

Add a disabled managed-service record to the **hub database**, substituting your
absolute INSTALL directory:

```sql
INSERT INTO hub_managed_services
  (service_key, name, executable_path, config_path, working_directory,
   enabled, service_kind, cluster_key)
VALUES
  ('battleground', 'Battleground matchmaking', 'battlegroundserver',
   'battlegroundserver.conf', '/srv/skyfire/bin', 0, 6,
   'skyfire-battleground-primary');
```

On Windows use `battlegroundserver.exe` and the Windows INSTALL directory instead.
Keep this row disabled until the certificate, configuration, and updated binaries
are ready. Then set `enabled = 1` for this service key, run `.reload db_records`
in the hub console, and use the hub service Start control. Starting the process
does not enable routing from worlds; enable each world's client separately.

These are the actual daemon-side world authorization keys:

```ini
Battleground.Realms = "1"
Battleground.AllowedWorlds = "skyfire-world-primary skyfire-world-standby"
Battleground.WorldRealms = "skyfire-world-primary=1 skyfire-world-standby=1"
```

Every allowed world certificate identity requires an explicit realm scope.
After the daemon reports ready, configure each participating world:

```ini
BattlegroundService.Enable = 1
BattlegroundService.Host = "127.0.0.1"
BattlegroundService.Port = 54950
BattlegroundService.NodeKey = "skyfire-battleground-primary"
```

Use the daemon's reachable DNS name or IP for remote hosts, with that endpoint
included in its certificate. Listener certificate files are refreshed every
30 seconds; a failed reload makes the daemon unavailable until corrected.

Configure its cluster certificate, private key, CA, optional CRL, and hub endpoint.
Set `Battleground.Realms`, `Battleground.AllowedWorlds`, and
`Battleground.WorldRealms` explicitly. For example, two world identities may both
be scoped to realm 1 for world failover, while another identity serves realm 2.
Each realm has one live queue owner; a second world generation must wait for the
15-second owner lease to expire before taking over. Retired generations and
snapshot sequence watermarks are retained for the daemon's lifetime. The safety
history allows 4096 retired world generations per realm. If exhausted, the daemon
rejects further takeovers and logs a recovery instruction. Stop all participating
worlds for that realm, restart the battleground daemon to clear its volatile
history, and then start the intended active world. Do not clear history while an
old world generation can still send requests.

Enable `BattlegroundService.Enable` in worldserver and set its `Host`, `Port`
(default 54950), and `NodeKey` to the daemon's endpoint and certificate identity.
Worldserver uses its existing cluster TLS credentials. The listener requires
mutual TLS, checks the certificate identity and realm allowlist, and stops
accepting operations whenever its hub connection loses registration. Use a
private service network and restrict the port to authorized world nodes.

Each request carries a bounded snapshot of eligible groups for one battleground
type and level bracket. It includes complete groups, faction, queue age, premade status,
configured team limits, invitation policy, and available slots in running matches.
The daemon returns group-ID proposals; worldserver must validate every proposal
against its current queue before inviting anyone. A rejected or lost response
does not trigger automatic replay or local admission of that proposal. A later
fresh snapshot can produce a new proposal based on the current world state.

Queue slots, frame size, groups, members, match proposals, connections, and request
deadlines are bounded. Worlds publish on a dedicated worker, so DNS and TLS waits
do not block world updates. The worker retains only the latest unsent snapshot
per queue key. Hub metrics report uptime, connections, requests, failures, queued
groups, queued players, proposals, and realm coverage. Under heavy load, the world
selects a bounded oldest-first window of complete groups; these counts describe the submitted
window, not necessarily the entire local queue. Empty contexts stop publishing
after acknowledgment. Counts expire after 20 seconds without a fresh snapshot.
They are not persisted across daemon restart.

Validation targets include `battleground_protocol_tests` and the pure matcher /
queue ownership tests. After compilation, run the native mutual-TLS fixture:

```text
python src/tests/battleground_service_test.py --battlegroundserver build/bin/Release/battlegroundserver.exe
```

The fixture uses disposable certificates and a local mock hub. It verifies real
matching, replay rejection, realm and identity isolation, malformed snapshots,
and hub-disconnect fencing. Live validation should additionally cover solo and
premade queues, running-match backfill, queue cancellation while a proposal is
in flight, world failover, and daemon restart. This phase does not provide a
durable battleground-service standby lease or migrate battleground gameplay.
