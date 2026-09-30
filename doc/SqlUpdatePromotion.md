# SQL updates and upstream merges

Auth, characters, world and hub use the same pending promotion workflow on the
`clustering` branch. CI checks incoming released SQL before promoting pending SQL.
It compares against the branch commit from before the push to preserve existing
clustering releases.

For a local merge, save the current clustering commit before merging the normal
repository. After resolving Git conflicts, preview the SQL check:

```powershell
python tools/dev/reconcile_sql_updates.py --base-ref <pre-merge-clustering-commit>
```

The preview exits nonzero when reconciliation is needed. Add `--apply` to:

- Keep the existing clustering release when incoming SQL uses its date, database
  and sequence. Move the incoming file into the matching pending folder.
- Restore the published clustering SQL when incoming content overwrote the same
  path, and move the incoming content to pending. Review these files before
  release: the check cannot split a combined SQL script into individual changes.
- Skip incoming byte-identical SQL copies in the same database (ignoring only
  CRLF versus LF). It does not guess equivalence from comments or SQL semantics.

Pending collision names contain the original filename and a content digest. The
promotion script assigns the next free release sequence in sorted filename order.
Historical deletions and collisions involving existing promotion metadata require
manual review; the tool stops without changing files. Existing historical files
are never removed solely because they have the same contents.

Then preview/run `tools/dev/promote_pending_updates.ps1` with `-Apply` as needed.
CI performs reconciliation and promotion together on SQL pushes. For a manual
workflow run, supply the pre-merge commit in `base_ref`; `HEAD` is suitable when
only promoting pending files. Git conflicts must be resolved before either tool
can operate. Review migration dependencies; matching numbers do not prove that
two different updates can execute in either order.

Use unique pending names and preserve the generated `.sql.pending-name` metadata.
An existing database that already imported a pending file receives its released
tracking name only after the hashes match. See the
[pending SQL guide](../sql/pending_updates/README.md) and
[hub setup guide](ClusteringSetup.md#hub-database-setup-and-updates).

Validation without building server binaries:

```powershell
python tools/dev/tests/reconcile_sql_updates.test.py
./tools/dev/tests/promote_pending_updates.test.ps1
```

The native `database_setup_tests` target covers fresh hub setup, promotion
tracking, hash mismatches and stale pending files left by INSTALL.
