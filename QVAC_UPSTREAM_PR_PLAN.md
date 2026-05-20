# QVAC Upstream PR Plan

This fork branch is intentionally broader than any single upstream PR should be.
Use it as the integration branch for QVAC validation, then split upstream work
into small, reviewable branches.

## Principles

- Keep `exec()` behavior unchanged unless a PR explicitly says otherwise.
- Prefer one native capability per PR, with matching tests in the same PR.
- Avoid QVAC-specific naming in upstream-facing code and commit messages.
- Do not include mobile vector/RAG work in the query API PR series.
- Rebase each focused branch on upstream `main`, not on this integration branch,
  unless it depends on an earlier accepted PR.

## Suggested PR Sequence

### PR 1: VFS File-Control Fallthrough

Scope:

- Return `SQLITE_NOTFOUND` from the default VFS `xFileControl` handler for
  unhandled operations.
- Preserve existing VFS behavior except allowing SQLite PRAGMAs and other
  file-control callers to continue through SQLite's fallback path.

Tests:

- `PRAGMA table_info(...)` returns rows through the public query path once that
  query API exists, or through the smallest available prepared-statement test
  harness for this PR.

### PR 2: Prepared Statement Run Mode

Scope:

- Add `binding.query(dbHandle, sql, params, "run")`.
- Add public `db.query(sql, params, "run")`.
- Prepare and finalize statements with `sqlite3_prepare_v2` and
  `sqlite3_finalize`.
- Bind only `null`, finite `number`, and `string`.
- Reject unsupported param types.
- Reject bind-count mismatch.
- Return `{ changes, lastInsertRowid }`.

Tests:

- create table through `run`
- insert with params
- changes and last insert rowid
- bad SQL rejects
- too few and too many params reject
- statement works after an error

### PR 3: Typed Scalar Result Values

Scope:

- Add `values` mode returning `Array<Array<SqliteValue>>`.
- Extract `NULL`, `INTEGER`, `FLOAT`, and `TEXT` with typed SQLite column APIs.
- Copy result values before statement finalization.

Tests:

- multiple selected rows as arrays
- null returns null
- integer and float return numbers
- text returns string

### PR 4: Object Rows And Single Row Mode

Scope:

- Add `all` mode returning row objects keyed by column name.
- Add `get` mode returning the first row object or `null`.
- Copy column names before statement finalization.

Tests:

- multiple selected rows as objects
- first row returned for `get`
- `get` returns null when no rows match
- unique index lookup

### PR 5: Uint8Array/BLOB Support

Scope:

- Bind `Uint8Array` values with `sqlite3_bind_blob`.
- Extract BLOB columns into new `Uint8Array` instances.
- Keep all JS memory copied before worker execution.

Tests:

- small blob byte-for-byte round trip
- large blob byte-for-byte round trip
- repeated blob insert/select loop

### PR 6: Async Close And Error Hardening

Scope:

- Serialize public operations so `close()` waits for queued work.
- Ensure validation failures reject asynchronously in Bare and Node.
- Confirm every error path frees copied SQL, params, rows, values, column names,
  and SQLite error strings.

Tests:

- close after pending query
- close after completed query
- unsupported param type rejects without crashing Bare
- subsequent query after validation error still works

### PR 7: CI Matrix Verification

Scope:

- Keep lint, `test:bare`, and `test:node` in CI.
- Preserve existing prebuild targets.
- Add only minimal CI edits required by the query API series.
- Ensure iOS device and simulator prebuilds use an explicit deployment target
  low enough for supported devices. QVAC physical-device smoke found that an
  addon built with SDK 26.5 as the minimum OS would not load on an iOS 26.4.2
  device; local validation used a rebuilt iOS arm64 prebuild with min iOS 15.1.

Targets to preserve:

- linux x64/arm64
- darwin x64/arm64
- win32 x64/arm64
- android arm/arm64/ia32/x64
- ios arm64
- ios simulator

## Out Of Scope For This Series

- SQLite-Vector integration.
- Extension loading changes.
- QVAC wrapper or Drizzle adapter code.
- JS cosine fallback or wasm-backed SQLite.
