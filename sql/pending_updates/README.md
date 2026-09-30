# Pending SQL Updates

Use this directory for SQL changes that need to be tested against a local or
staging database before they are promoted into the normal update stream.

By default, the server database updater does not read files from this directory.
On worldserver, enable `WorldDatabase.ImportPendingUpdates` and/or
`CharacterDatabase.ImportPendingUpdates` to import matching pending folders
during startup update checks. Hubserver uses `HubDatabase.ImportPendingUpdates`
for `pending_updates/hub`. Leave those options disabled for production.

When automatic import is disabled, apply pending updates manually while testing.
After an update is verified, move it into the matching `sql/updates/<database>`
directory so the normal database setup and update tracking system can apply it.

## Layout

- `auth` - pending auth database updates
- `characters` - pending character database updates
- `world` - pending world database updates
- `hub` - pending hub database updates

## Promotion Checklist

1. Add the test SQL file to the matching pending folder.
2. Apply it manually to a test database, or enable the matching
   `ImportPendingUpdates` option on a non-production worldserver.
3. Verify server startup, affected commands, and any in-game behavior.
4. Name the file clearly for review. It does not need the final update name.
   The promotion script assigns `YYYY_MM_DD_<database>_NN.sql` automatically.
5. Push the pending SQL file to `clustering` and let the promotion workflow move it
   into `sql/updates/<database>`. All four domains, including hub, use the same script.
6. Let the database setup system record it through `skyfire_db_updates` and
   `db_update`.

## Hub update ordering

Place hub SQL directly in `sql/pending_updates/hub/`, with sortable names such as
`001_add_setting.sql`, then `002_backfill_setting.sql` when one depends on another.
CI assigns release names in that order, continuing the date's existing sequence.

To preview locally, run `tools/dev/promote_pending_updates.ps1 -Database hub`.
Add `-Apply` to promote the files. Preserve dependency order and never rename or
edit released files. Startup applies sorted releases before sorted pending files.

The script preserves SQL bytes and writes `<release>.sql.pending-name` containing
the original filename. Ship this metadata with the SQL. The updater verifies the
recorded pending hash against the release before recording its new name without
executing SQL again. Missing or mismatched hashes stop startup, even with the hash
override enabled. Manually applied, untracked SQL needs deliberate reconciliation;
the updater cannot infer that it ran. Never reuse a promoted pending filename.
An identical pending copy left by INSTALL is ignored; a conflicting copy is rejected.
Production should import released updates only. See
[hub database setup](../../doc/ClusteringSetup.md#hub-database-setup-and-updates).
