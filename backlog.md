# uTimer Backlog

Derived from the deep review of `timetracker.cpp`, `databasemanager.cpp`, and
`historydialog.cpp`. Grouped by theme and ordered by recommended execution
sequence. Each item is self-contained so it can be picked up independently, but
cross-references note upstream/downstream dependencies where they exist.

Severity legend: **[F]** fundamental design flaw · **[C]** critical bug ·
**[M]** major bug · **[m]** minor / polish.

---

## Phase 1 — Low-risk local fixes (do first)

These are small, contained fixes with a clear blast radius. They remove two
loud correctness bugs and one data-loss bug before we touch the harder
architectural issues.

### T1. [DONE] [C2] Keep `rowOrigins_` in sync when splitting a row in `HistoryDialog`
- **Where:** `historydialog.cpp:560–565` (split handler), with use sites in
  `historydialog.cpp:399–406` (`saveChanges` iterating by row index).
- **Problem:** `onSplitRow` mutates `pendingChanges_[idx]` by erasing one row
  and inserting two, but never updates `rowOrigins_[idx]`. On save, rows past
  the split index are either dropped (when `row >= rowOrigins_[i].size()`) or
  routed to the wrong persistence bucket (DB vs in-memory) because the origins
  vector is now offset by one.
- **Fix:**
  1. In `onSplitRow`, after the `erase`/`insert`×2 sequence, apply the same
     `erase`/`insert`×2 to `rowOrigins_[idx]`. Both new entries inherit the
     origin of the row that was split.
  2. Add a debug-only invariant check after every mutation of `pendingChanges_`
     that asserts `rowOrigins_[i].size() == pendingChanges_[i].size()` for
     every page index `i`.
- **Tests:**
  - Add a test that loads a history with N rows on the Today page (mix of
    in-memory + DB-backed rows), splits one of them, clicks OK, and verifies
    that both new rows land in the correct bucket (check via a fake
    DatabaseManager that records which rows were passed to
    `replaceDurationsInDB` vs `setCurrentDurations`).
  - Regression test that splits a row on a non-Today page (pure-DB page) and
    verifies both children survive the save round-trip.

### T2. [DONE] [C1] Do not wipe `durations_` unconditionally on retry failure in `startTimer`
- **Status:** Done
- **Where:** `timetracker.cpp:92–128`.
- **Problem:** On `startTimer` from `None`, if `has_unsaved_data_` is set, we
  retry `appendDurationsToDB()`. When the retry fails, the code logs
  "CRITICAL: Retry save failed - data will be lost" and then unconditionally
  runs `durations_.clear()` / `has_unsaved_data_ = false` a few lines later,
  actually losing the data.
- **Fix:**
  1. Gate the clear + flag reset at `timetracker.cpp:127–128` behind the retry
     outcome. On failure, keep `durations_` and `has_unsaved_data_` intact so
     the *next* `startTimer`/`pauseTimer`/`stopTimer` can try again.
  2. Emit a user-visible warning (status bar or tray notification) on retry
     failure so the user knows their last session is unsaved and can
     investigate disk/permissions before closing the app.
  3. Make the retry idempotent — if `durations_` already has a finalized
     previous-session segment plus the new session's starting row, the retry
     must only re-append the unsaved portion. Add a marker on the segment
     entries or keep them in a separate `unsaved_durations_` vector.
- **Tests:**
  - Unit test with a fake DatabaseManager that returns `false` from
    `appendDurations` the first time and `true` the second. Drive
    `startTimer` → `stopTimer` → `startTimer` (retry fails) → `startTimer`
    (retry succeeds) and assert that the original segments land in the DB on
    the second retry, not lost.
  - Unit test that a failed retry leaves `has_unsaved_data_ == true` and
    `durations_` non-empty.

### T3. [DONE] [M7] Surface `loadDurations` skip/repair decisions to the user
- **Where:** `databasemanager.cpp:492–540`.
- **Problem:** Rows with invalid type/timestamps are silently skipped with only
  a log line. Rows whose stored duration disagrees with the computed duration
  by more than 5 ms have the computed value silently substituted. The user
  sees "wrong" history without knowing why.
- **Fix:**
  1. Return a `LoadResult` struct from `loadDurations`: `{ durations, skipped,
     repaired }` counts, plus optionally a list of row ids that were touched.
  2. Propagate the counts up to HistoryDialog / mainwin and show a one-line
     banner at the top of the history dialog: "N rows skipped due to corrupt
     data, M rows auto-repaired." with a link to the log file.
  3. Raise the 5 ms tolerance to 100 ms (one UI frame at 60 Hz is ~16 ms, but
     wall-clock drift under load can exceed that) and document the number as
     a named constant `kDurationReconciliationToleranceMs`.
- **Tests:**
  - Unit test that seeds the DB with one row whose `type` is out of range and
    asserts `LoadResult::skipped == 1` and the row is absent from the result.
  - Unit test that seeds a row whose stored duration is 200 ms off from
    computed and asserts `LoadResult::repaired == 1` and the returned duration
    matches computed.

---

## Phase 2 — Crash recovery & stale checkpoint reconciliation

These items address **F3**, the single most likely source of "phantom"
entries in history, and they establish the startup invariants we need before
doing the schema rework in Phase 4.

### T4. [DONE] [F3] Add a `durations.is_finalized` column and a startup reconciliation pass
- **Where:** `databasemanager.cpp` schema at line 101; `timetracker.cpp`
  startup path at 86–149; new reconciliation method.
- **Problem:** `saveCheckpoint` writes a row that represents the currently
  running segment. If the app crashes before the next `pauseTimer`/`stopTimer`
  finalizes that row, it stays in the DB as a normal-looking entry. The next
  launch treats it as a finished historical entry.
- **Fix:**
  1. Add `is_finalized INTEGER NOT NULL DEFAULT 0` to the `durations` schema.
     All writes from `saveCheckpoint` insert with `is_finalized = 0`. All writes
     from `updateDurationsInDB`/`replaceDurationsInDB` / explicit finalize
     paths insert with `is_finalized = 1`. Migration: on schema upgrade, set
     every existing row to `is_finalized = 1` (we cannot distinguish orphans
     retroactively — log the assumption).
  2. On `DatabaseManager::lazyOpen`, after the existing retention cleanup,
     run a reconciliation query: `SELECT * FROM durations WHERE
     is_finalized = 0`. Pass the results to a new method
     `TimeTracker::reconcileOrphanCheckpoints(const std::vector<Duration>&)`.
  3. `reconcileOrphanCheckpoints` inspects each orphan and chooses one of:
     - **Commit as-is**: if the orphan's `end_time` is recent (e.g. within
       `2 × checkpoint_interval`), commit it as a finalized row. This is the
       "crash happened, keep the work we had saved" path.
     - **Truncate to last known good**: if there is evidence of a longer gap,
       trim and commit. (In practice the row's own `end_time` already
       represents the last checkpoint; commit as-is is almost always right.)
     - **Drop**: only if the orphan's duration is below a minimum threshold
       (say 1 second).
  4. The reconciled row's id must be preserved where possible (use `UPDATE
     durations SET is_finalized = 1 WHERE id = ?`), not DELETE+INSERT, so that
     `current_checkpoint_id_` semantics remain sane if we later restart a
     session.
  5. Show a non-intrusive notification on startup: "Recovered N seconds from
     the last session after an unclean shutdown."
- **Dependencies:** Must land before T13 (stable segment IDs) so we do not
  have to migrate the schema twice.
- **Tests:**
  - Simulate crash: start a session, fire one or more checkpoints, kill the
    TimeTracker without calling `stopTimer`, reopen DB, assert the orphan is
    detected and reconciled.
  - Reconciliation is idempotent: running it twice on the same DB does not
    double-count.
  - Orphan older than 24 h or below the 1-second threshold is dropped, not
    committed.

### T5. [DONE] [F3 follow-up] Write a `last_clean_shutdown` marker for diagnostic clarity
- **Where:** `DatabaseManager` settings table (add one if absent) or
  `user-settings.ini`.
- **Problem:** The reconciliation pass in T4 needs to *know* a crash happened
  to avoid noisy notifications when the app was shut down cleanly.
- **Fix:**
  1. On successful `stopTimer` / app quit, set a `last_clean_shutdown` value
     to the current timestamp.
  2. On startup, if the marker is missing or older than the oldest orphan,
     show the recovery notification; otherwise reconcile quietly.
  3. Clear the marker as the very first thing on startup so an interrupted
     restart still gets diagnosed as unclean.
- **Tests:**
  - Marker-present path: reconciliation runs silently.
  - Marker-absent path: reconciliation surfaces a user-visible notification.

---

## Phase 3 — HistoryDialog correctness

These items decouple the dialog from the checkpoint subsystem and stop the
"dialog saved this row but the next checkpoint silently overwrites it" class
of failures. They depend on Phase 1 being done but can overlap with Phase 2.

### T6. [DONE] [C3 / F4] Reset TimeTracker checkpoint tracking when HistoryDialog saves
- **Where:** `historydialog.cpp:408, 434`; `timetracker.cpp:420–424`
  (`setCurrentDurations`).
- **Problem:** `HistoryDialog::saveChanges` calls `replaceDurationsInDB`
  (wipes and rewrites the table with fresh row ids) then `setCurrentDurations`
  (which does *not* touch `current_checkpoint_id_` or `segment_start_time_`).
  The next checkpoint tick UPDATEs a stale id → 0 rows → falls through to the
  INSERT path → collides with the row HistoryDialog just wrote → `ON CONFLICT
  REPLACE` silently clobbers the user's confirmed state.
- **Fix:**
  1. Add a new public method `TimeTracker::resetCheckpointTrackingForOngoing(
     const Duration& ongoing)`. It sets `current_checkpoint_id_ = -1`,
     refreshes `segment_start_time_ = ongoing.startTime`, and re-issues one
     `saveCheckpoint` call immediately so the DB row representing the ongoing
     segment is rebuilt with a fresh, valid id.
  2. Extend `setCurrentDurations` signature so callers must pass the new
     ongoing duration explicitly (or make `resetCheckpointTrackingForOngoing`
     the *only* way to set current durations from outside the class) and
     route `HistoryDialog::saveChanges` through it.
  3. While the dialog is open and the timer is running, pause checkpointing
     (`checkpoint_timer_->stop()`) and resume on close; this prevents a
     checkpoint from firing against the pre-replace state.
- **Dependencies:** T13 makes this even simpler because id-based updates will
  naturally no-op when the row was replaced; keep the explicit reset anyway
  for defense in depth.
- **Tests:**
  - Open HistoryDialog with a running timer, edit an unrelated row, save,
    advance the mock clock past the next checkpoint interval, and assert
    that the previously-saved row was **not** mutated and a new checkpoint
    row exists for the ongoing segment.
  - Verify `checkpoint_timer_` is paused for the dialog's lifetime.

### T7. [C4] Stop stripping DB backing from in-memory current-session rows
- **Where:** `historydialog.cpp:402–408`; `databasemanager.cpp:386–394`.
- **Problem:** `replaceDurationsInDB` deletes the whole `durations` table, then
  re-inserts only `historyDurations`. Rows classified as `CurrentMemory` used
  to have backing DB rows (from prior checkpoints / pauseTimer); after OK they
  exist only in memory and a crash before the next finalize loses them.
- **Fix:**
  1. Change `replaceDurationsInDB` to take **both** the history rows and the
     current-session rows as separate arguments, and rewrite the table with
     all of them. Mark current-session rows `is_finalized = 0` (depends on
     T4).
  2. Alternative if T4 is not yet landed: change the contract so current-
     session rows are preserved by id (the dialog must retain the pre-load
     ids on its `rowOrigins_` entries for `CurrentMemory` rows and pass them
     through) and the wipe+rewrite is scoped to historical rows only.
  3. Add a regression test that asserts the DB row count after dialog-save
     equals history rows + current-session rows.
- **Tests:**
  - Open dialog with a running timer, click OK without editing anything,
    simulate a crash (no further `stopTimer`), reopen the DB, assert the
    current segment is still present as an unfinalized row.

### T8. [C5] Replace fragile dedup key in `HistoryDialog::createPages`
- **Where:** `historydialog.cpp:80–94`.
- **Problem:** Dedup key is
  `QString::number(d.startTime.toMSecsSinceEpoch()) + "|" +
  QString::number((int)d.type)`. Any millisecond drift (from cleanDurations
  merges, UTC round-trips, DST edges) breaks dedup and shows phantom
  duplicates that then get persisted on save.
- **Fix:**
  1. Depends on T13: once segments have stable ids, use
     `segment_id`-based dedup and delete the start-time key entirely.
  2. Interim fix before T13: widen the dedup to treat DB and memory rows as
     duplicates if `type` matches **and** `abs(db.startTime -
     memory.startTime) < 1 second` **and** `abs(db.endTime - memory.endTime)
     < 1 second`. Document the 1-second window as a tolerance constant.
- **Tests:**
  - Deliberately perturb the in-memory entry's `startTime` by 2 ms and verify
    the interim fix still dedups.
  - After T13, verify dedup works with perturbed start times and different
    ids — and refuses to dedup when ids differ.

### T9. [M5] Freeze ongoing duration snapshot for dialog lifetime
- **Where:** `historydialog.cpp:111–113` (capture) and `424–427` (save).
- **Problem:** `createPages` captures the ongoing duration with `endTime =
  now_1`. `saveChanges` recomputes it with `endTime = now_2`. The user sees
  `now_1` but the DB gets `now_2`. If the dialog sits open for minutes, the
  saved segment is materially longer than the displayed one.
- **Fix:**
  1. Snapshot the ongoing duration once in `createPages` into a member
     variable `ongoing_snapshot_`. Use this snapshot in both display and
     save paths.
  2. If the user edits the ongoing row, the edited values override the
     snapshot. If not, the snapshot is used verbatim.
  3. On dialog close (OK), the TimeTracker's in-memory `durations_` is
     updated to the snapshot so the next checkpoint starts from that point.
     (This overlaps with T6.)
- **Tests:**
  - Use a fake clock. Open the dialog, advance the clock by 10 minutes, click
    OK without editing, assert the saved ongoing row's `endTime` equals the
    snapshot time, not `now_2`.

### T10. [m] Fix off-by-one in the "minimum 3 seconds" user message
- **Where:** `historydialog.cpp:519`.
- **Problem:** Error text reads "minimum 3 seconds" but the code rejects
  `duration <= 2` seconds (i.e. accepts 3+). The threshold logic is correct;
  the message wording is fine, but worth confirming both agree.
- **Fix:** Either change the text to "must be more than 2 seconds" or change
  the condition to `< 3`. Pick whichever matches product intent; document it.
- **Tests:** Boundary tests for exactly 2, 3, and 4 seconds.

### T11. [m] Correct historical grouping for cross-midnight entries
- **Where:** `historydialog.cpp:95`.
- **Problem:** Grouping uses `endTime.date()`. A cross-midnight entry (only
  possible if the midnight auto-split path is skipped) is attached to the end
  day, not the start day, which is inconsistent with the Today-page logic.
- **Fix:** Use `startTime.date()` for historical grouping to match the Today
  page. Cross-midnight entries should already be split by
  `addDurationWithMidnightSplit`; this is defense in depth.
- **Tests:** Seed the DB with an unsplit cross-midnight row, verify it
  appears on its start date.

### T12. [m] Clarify Prev/Next navigation direction
- **Where:** `historydialog.cpp:346–347`.
- **Problem:** Prev/Next enable logic is inverted relative to natural UX
  (Prev goes backward in time, but the array index math makes this confusing
  when reading the code).
- **Fix:** Rename `prevPageButton_`/`nextPageButton_` handlers to `onOlder`
  and `onNewer`, or introduce a local `currentIndex` with comments clarifying
  that `index == 0` is the oldest page. No behavior change, just readability.

---

## Phase 4 — Schema & identity rework (the big one)

Addresses **F1**, **F2**, and the upsert-by-start-time class of bugs
(**C3**, **C5**, **C6**, **M2**). Everything above should be landed and
stable before starting this phase because it rewrites the persistence
contract.

### T13. [F1] Introduce stable `segment_id` as the row identity
- **Where:** `databasemanager.cpp:101` (schema), all write paths, all upsert
  sites, `TimeTracker::durations_` container, `Duration` struct.
- **Problem:** The `UNIQUE(start_date, start_time, type) ON CONFLICT REPLACE`
  schema conflates segment identity with a volatile millisecond timestamp.
  Every `INSERT OR REPLACE` path silently destroys rows under clock drift or
  `cleanDurations` perturbation.
- **Fix:**
  1. **Schema change:** add `segment_id TEXT NOT NULL` (UUIDv4 or equivalent
     monotonic id) with a `UNIQUE` index. Remove the
     `UNIQUE(start_date, start_time, type)` constraint.
  2. **Migration:** on schema upgrade, assign a fresh UUID to every existing
     row. Log count.
  3. **In-memory `Duration`:** add `QString segment_id` field. Assign at
     creation time in `TimeTracker` (the single place where segments are
     born). Never re-create: subsequent edits / merges / splits preserve (or
     for splits, assign a new one to the second half).
  4. **All write paths:**
     - `appendDurationsToDB`: plain `INSERT` of rows with their assigned
       `segment_id`. No ON CONFLICT behavior needed; a conflict is a bug.
     - `updateDurationsByStartTime`: rename to `updateDurationsById`. Match
       solely on `segment_id`. Remove all start-time matching logic.
     - `saveCheckpoint`: `UPDATE durations WHERE segment_id = ?`. If 0 rows
       affected, log error (this should be impossible once the tracker owns
       the id) and `INSERT` a fresh row. Never silently `REPLACE`.
     - `replaceDurationsInDB`: still wipes and rewrites, but preserves
       `segment_id` from each input row.
  5. **`cleanDurations`:** now free to merge/reassign start times; merges
     must choose one surviving `segment_id` (the earlier one) and the
     caller is responsible for DELETE-ing rows for any `segment_id` that
     disappeared (pass a `removed_ids` out-param).
  6. Delete `current_checkpoint_id_` entirely — replace with
     `current_checkpoint_segment_id_`.
- **Dependencies:** T4 (is_finalized) should land first so we only migrate
  the schema once.
- **Tests:**
  - Full migration test from a pre-upgrade DB dump. Verify every pre-upgrade
    row has a segment_id after migration, no data loss.
  - Drift test: perturb an in-memory `startTime` by 5 s after insert, call
    `updateDurationsById`, verify the row is updated (not duplicated).
  - Checkpoint test: run a long-simulated session with many checkpoints,
    assert exactly one DB row per segment at every point.
  - Split test: split a row, verify the original id is kept by one half and
    a fresh id is assigned to the other.

### T14. [F2] Make `cleanDurations` honor segment identity
- **Where:** `helpers.cpp:102–104, 111, 143`.
- **Problem:** `cleanDurations` mutates `prevIt->startTime` in merge branches.
  That was safe when there were no DB rows keyed by startTime; after T13 it
  is also safe, but the function must now return information about which
  in-memory entries correspond to which DB rows so the caller can clean up.
- **Fix:**
  1. After T13, `cleanDurations` takes `std::vector<Duration>&` and returns
     `std::vector<QString> removed_segment_ids` — the ids of entries that
     were merged away.
  2. All three callers (`appendDurationsToDB`, `updateDurationsInDB`,
     `replaceDurationsInDB`) receive that list and issue
     `DELETE FROM durations WHERE segment_id IN (...)` before their main
     write.
  3. `helpers.cpp` tests must cover merge branches that drop a segment.
- **Tests:**
  - Construct a two-entry input where the second merges into the first;
    verify the returned removed list contains the second's id and that the
    DB no longer has that row after the write.

### T15. [C6] Remove the silent-REPLACE fallback from `saveCheckpoint`
- **Where:** `databasemanager.cpp:640–667`.
- **Problem:** When `UPDATE` by id returns 0 rows (row deleted by retention
  cleanup or REPLACE from an earlier bug), the fallback `INSERT` can collide
  with another row at the same start-time key and silently REPLACE it.
- **Fix:** After T13, the fallback `INSERT` uses the segment_id, which is
  unique by schema. A conflict means an invariant has been violated and we
  should log + return error, not silently overwrite.
- **Dependencies:** T13.
- **Tests:** Unit test where the row backing the current checkpoint is
  deleted out from under the tracker; verify the fallback re-inserts cleanly
  and the segment_id is preserved.

### T16. [M2] Simplify `backpauseTimer` given stable ids
- **Where:** `timetracker.cpp:241–253`.
- **Problem:** `backpauseTimer` adds two segments (truncated Activity,
  Pause) then upserts. With start-time keying, a `cleanDurations`-induced
  drift can orphan the original checkpoint row and produce a duplicate.
- **Fix:** After T13, the Activity segment keeps its original
  `segment_id`, so `updateDurationsById` updates the correct row and the
  new Pause segment is inserted with a fresh id. No silent REPLACE path.
  Remove the `current_checkpoint_id_ = -1` line and replace with an
  explicit "start new checkpoint for Pause" call.
- **Tests:** Backpause scenario with a mocked lock-state watcher firing at
  a known time; verify exactly one Activity row and one Pause row land in
  the DB with distinct ids.

---

## Phase 5 — TimeTracker state hygiene

Independent of the schema rework but easier to land after it.

### T17. [F4 / M1] Move session state into a dedicated struct
- **Where:** `timetracker.cpp` / `timetracker.h`.
- **Problem:** `current_checkpoint_id_`, `segment_start_time_`,
  `has_unsaved_data_`, `durations_`, and timer objects are all raw fields
  on `TimeTracker`. Several code paths (notably `addDurationWithMidnightSplit`
  at `timetracker.cpp:566`) mutate them as side effects.
- **Fix:**
  1. Introduce `struct SessionState { QString segment_id; QDateTime
     segment_start_time; bool has_unsaved_data; ... };` and make all
     mutations go through explicit `SessionState::transitionTo...` methods
     that log the old and new state.
  2. `addDurationWithMidnightSplit` becomes a pure function that returns
     the list of new durations and the new segment state, and the caller
     applies the transition explicitly.
  3. Add a debug-build invariant that logs and optionally asserts whenever
     a state field changes between entry and exit of a public method
     without an explicit transition call.
- **Tests:** State transition tests for every public `TimeTracker` method,
  asserting the expected `SessionState` after each call.

### T18. [M3] Make `hasEntriesForDate` distinguish "no" from "unknown"
- **Where:** `databasemanager.cpp:549–575`; `timetracker.cpp:119–120`.
- **Problem:** Returns `false` when history is disabled or `lazyOpen` fails,
  causing `startTimer` to think this is the first session of the day and add
  boot time again.
- **Fix:**
  1. Change the return type to `enum class EntriesForDateResult { Yes, No,
     Unknown };` (or `std::optional<bool>`).
  2. When history is disabled or the DB is inaccessible, return `Unknown`.
  3. `startTimer` treats `Unknown` as "don't add boot time" — never double-
     count.
- **Tests:**
  - History disabled + already-running-day → no boot time added.
  - History enabled + empty DB → boot time added once.
  - History enabled + DB has entries for today → boot time not added.

### T19. [M4] Persist Pause rows when they start, not at the next pause/stop
- **Where:** `timetracker.cpp:60–85` (resume-from-Pause path).
- **Problem:** Resuming from Pause creates a Pause entry in memory only.
  A crash before the next save loses it, so the reloaded timeline shows two
  Activities back-to-back with no gap.
- **Fix:**
  1. On `startTimer` from `Pause`, call `appendDurationsToDB` for the new
     Pause row immediately (marked `is_finalized = 1` once T4 lands, since
     the Pause is complete the moment the user un-pauses).
  2. Covered by checkpointing for the subsequent Activity.
- **Tests:**
  - `startTimer` → `pauseTimer` → advance clock → `startTimer` → crash
    (skip `stopTimer`) → reopen → verify the Pause row is in history.

### T20. [m] Audit `addDurationWithMidnightSplit` for timing races
- **Where:** `timetracker.cpp:165` (called from `pauseTimer` just after
  `timer_.restart()` at 164).
- **Problem:** Between `timer_.restart()` and the `now` captured for
  `addDurationWithMidnightSplit`, thread preemption or clock skew can cause
  ordering issues with the next `saveCheckpoint`.
- **Fix:** Capture `now` once at the top of each public `TimeTracker` method
  and pass it through. No calls to `QDateTime::currentDateTime()` inside
  helpers — they take a `now` parameter.
- **Tests:** N/A (hard to reproduce); the refactor is the fix.

---

## Phase 6 — DatabaseManager robustness

### T21. [M8] Protect `createBackup` with a mutex and drop the close/reopen dance
- **Where:** `databasemanager.cpp:275–342`.
- **Problem:** `createBackup` closes the DB, `QFile::copy`s the file, then
  reopens. Any concurrent DatabaseManager call in the window will fail. The
  flow is also race-prone with WAL mode.
- **Fix:**
  1. Add a `QRecursiveMutex db_mutex_` to `DatabaseManager` and wrap all
     public methods in `QMutexLocker`.
  2. Replace the close/copy/reopen dance with SQLite's Online Backup API
     (`sqlite3_backup_init`/`_step`/`_finish`). Access via
     `QSqlDriver::handle()` to reach the underlying `sqlite3*`.
  3. If the Online Backup API path is too much scope, at minimum hold the
     mutex for the whole copy and issue a `PRAGMA wal_checkpoint(TRUNCATE)`
     before the copy so WAL mode (if enabled) captures in-flight data.
- **Tests:**
  - Run `createBackup` while another thread fires checkpoints; verify no
    errors and the backup is valid.

### T22. [M9] Run retention cleanup at most once per session (and not on every `lazyOpen`)
- **Where:** `databasemanager.cpp:140–165`.
- **Problem:** Retention `DELETE` runs on every DB operation, including every
  checkpoint tick (~every 5 minutes). Pointless overhead and a failed
  cleanup is only logged, not retried.
- **Fix:**
  1. Gate the cleanup behind a `retention_cleanup_done_` flag that resets on
     app start. Alternatively, run it once per day based on a persisted
     "last cleanup" timestamp.
  2. If the cleanup transaction fails, mark it for retry on the next
     `lazyOpen` call rather than swallowing the warning.
- **Tests:**
  - Open the DB ten times in a row; verify the `DELETE` runs once.
  - Simulate a cleanup failure; verify it retries on the next startup.

### T23. [m] Use `db.rollback()` for read-only transactions in `loadDurations`
- **Where:** `databasemanager.cpp:543`.
- **Problem:** Calls `db.commit()` at the end of a read-only `SELECT` path.
  Qt accepts this but it is idiomatically wrong.
- **Fix:** Replace with `db.rollback()`.
- **Tests:** N/A.

### T24. [m] Set `PRAGMA synchronous=FULL` at connection open, not at flush
- **Where:** `databasemanager.cpp:833` (currently in `flushToDisc`).
- **Problem:** Every normal write runs at SQLite's default
  synchronous level. Only the shutdown path gets FULL durability.
- **Fix:**
  1. Decide on the durability vs throughput trade-off. For a time-tracking
     app that values correctness over throughput, set `PRAGMA
     synchronous=NORMAL` at `lazyOpen` (safe in WAL mode, fast enough for
     a few writes per minute) and promote to `FULL` only if we observe
     actual corruption under WAL.
  2. If we don't switch to WAL mode, use `FULL` from the start.
- **Tests:** Benchmark flush path before/after to verify no regression.

### T25. [m] Stop using `reinterpret_cast<quintptr>(this)` for connection names
- **Where:** `databasemanager.cpp:37`.
- **Problem:** After object destruction and reallocation at the same
  address, the connection name collides. Harmless today but surprising.
- **Fix:** Use an atomic counter `QAtomicInteger<quint64>
  s_connection_seq` to mint unique names.
- **Tests:** Unit test that creates and destroys 100 DatabaseManagers and
  verifies all connection names are unique.

### T26. [optional] Evaluate enabling WAL mode at `lazyOpen`
- **Where:** `databasemanager.cpp` open path.
- **Problem:** `flushToDisc` uses `PRAGMA wal_checkpoint` implying WAL
  mode, but `lazyOpen` never issues `PRAGMA journal_mode=WAL`. The DB is
  effectively in rollback-journal mode.
- **Fix:**
  1. Decide: do we want WAL? Pros: concurrent reads during writes, fewer
     fsyncs. Cons: sidecar files complicate backup.
  2. If yes: issue `PRAGMA journal_mode=WAL` at open, update T21 to handle
     WAL sidecars, and keep the `wal_checkpoint` calls in `flushToDisc`.
  3. If no: remove the `wal_checkpoint` calls entirely and document the
     decision in `databasemanager.cpp` at the top.
- **Dependencies:** Landed after T21 so the backup path is already robust.

---

## Phase 7 — Test infrastructure and invariants

These are not bug fixes — they are the scaffolding that keeps the bugs from
coming back.

### T27. Add a fake clock and a fake DatabaseManager to the test harness
- **Problem:** Many of the bugs above are only reproducible with
  deterministic time control and a DB that can fail on demand.
- **Fix:**
  1. Introduce a `Clock` interface (wrapping `QDateTime::currentDateTime()`)
     with a fake implementation that the tests drive forward explicitly.
  2. Introduce an `IDatabaseManager` interface with a test double that
     records every call and can be programmed to return failure on any
     method.
  3. Rewrite enough of the existing TimeTracker tests to use both.
- **Tests:** This *is* the test infrastructure. Cover under every Phase
  1–6 item above.

### T28. Add invariant checks in debug builds
- **Fix:**
  1. `TimeTracker`: after every public method, assert `durations_.size()
     >= 0`, segment ordering is non-decreasing, no overlapping segments
     in the same day.
  2. `DatabaseManager`: after every write, assert that the number of rows
     with the given `segment_id` is exactly 1 (after T13).
  3. Wrap assertions in `#ifndef QT_NO_DEBUG` so release builds pay nothing.
- **Tests:** Invariant violations become test failures, not runtime crashes.

---

## Recommended execution order

1. **Phase 1** — quick, safe, high-signal fixes (T1–T3).
2. **Phase 2** — T4, T5. Establishes crash recovery before we rework the
   schema.
3. **Phase 3** — T6–T12. Stops HistoryDialog from corrupting the DB;
   T6/T7/T9 are the most important. T8 is deferred if T13 lands soon.
4. **Phase 7 bootstrap** — T27 at least, so Phase 4 has test coverage.
5. **Phase 4** — T13–T16. The schema/identity rework. Expect it to touch
   every write path and to surface latent bugs that the upsert schema was
   masking.
6. **Phase 5** — T17–T20. Clean up state ownership once the schema is
   stable.
7. **Phase 6** — T21–T26. Database robustness and polish.
8. **Phase 7 completion** — T28. Lock down invariants now that the
   architecture is right.

The common theme: segment identity must live as a stable key, not as
`(start_date, start_time, type)`. Almost every sporadic
duplication/overlap symptom traces back to that assumption breaking under
normal operation — which is why Phase 4 is load-bearing even though it is
scheduled last among the architectural items.
