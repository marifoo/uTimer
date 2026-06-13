# simple-code.md — Simplification Plan for uTimer

A tiered, trackable plan for making uTimer simpler in the sense of John
Ousterhout's *A Philosophy of Software Design* (see `philosophy.md`).

## Overview

A full four-way review of the tree (timer engine, persistence, GUI glue,
peripherals) found uTimer already in **good simplicity health**: deep modules,
errors designed out of existence, specific naming. **No structural redesigns are
warranted.** What remains is a tail of surgical wins in three flavors:

1. **Duplicated bodies** that have begun to drift (Tier 1).
2. **Shallow or misnamed surfaces** (Tier 2).
3. **Comments/docs out of sync with the code**, plus a few localized cleanups
   (Tiers 3–4).

Several findings that first arrived as "dead code, delete it" were **falsified by
grepping the test suite** — they are test-exercised API. They live in the
[Do NOT do](#do-not-do-verified) appendix so nobody re-discovers and "fixes" them.

## How to read this

- Tiers are ordered by **recommended execution sequence**. Within a tier, items
  are independent and each is shippable behind existing tests.
- Recommended order rationale: Tier 3/4 comment-and-constant fixes are
  near-zero-risk and make the code honest; Tier 1 de-duplications sit behind
  solid test coverage; Tier 2 renames ripple into `main.cpp`, `FakeSessionStore`,
  and tests, so do them deliberately, one commit each. (If you prefer impact
  first, start at Tier 1 — T1.1 is the single biggest payoff.)
- Every item carries a `- [ ]` checkbox so this file doubles as a tracker.

## Verification (global)

- Build: `qmake-qt5 && make -j4` (Qt5).
- Tests: `cd qtest && qmake-qt5 && make -j4 && ./qtest`.
- **Success bar:** all tests pass with **zero `QWARN`** (per `AGENTS.md`).
- Each item below names the specific suite(s) under **Verify**.

---

## Tier 1 — Real duplication, highest value

### - [x] T1.1 — Collapse `updateTable`'s four duplicated row-builders
- **Principle:** Togetherness vs Separation (#7); Deep Modules (#1) — a ~190-line method.
- **Location:** `historydialog.cpp:392-522`.
- **Problem:** `updateTable` builds table rows four times — completed
  (392–441), ongoing (443–474), cross-midnight (476–497), continuation
  (499–522) — with copy-pasted item construction (`typeStr`, `startEndItem`,
  `durationItem`, `setItem`). The cross-midnight and continuation blocks are
  nearly identical, down to declaring `const QColor grayBg(...)` twice. This
  length + duplication is the single biggest readability cost in the codebase.
- **Change:**
  - Extract `addReadOnlyRow(int row, const TimeDuration& d, const QString& suffix)`
    for the two gray, display-only blocks (cross-midnight / continuation). This
    collapses ~45 near-identical lines to ~15 and removes the duplicated `grayBg`.
  - Optionally extract `addEditableRow(...)` for the completed/ongoing pair, but
    **keep the distinct checkbox `stateChanged` lambdas inline** — they capture
    different state (page index vs ongoing flag) and are not safely mergeable.
  - Preserve exactly: row ordering, suffix text (`"(spans midnight)"`, `"(cont.)"`,
    `"  (Running)"`), the editable/disabled flags, and the colors
    (`QColor(200,200,200,180)` gray, `QColor(180,216,228,255)` modified-highlight).
- **Severity:** Medium · **Effort:** M · **Risk:** Core display path; row order,
  suffixes and colors are asserted indirectly via the dialog tests.
- **Verify:** `test_historydialog`.

### - [x] T1.2 — Unify the two "replace everything" write paths
- **Principle:** Togetherness vs Separation (#7) — duplicated conjoined logic that has drifted.
- **Location:** `sqlitesessionstore.cpp:480` (`saveDurations`) and `:578` (`replaceAll`).
- **Problem:** Both methods run the same dance: `ensureOpen` → `createBackup` →
  `db.transaction()` → `DELETE FROM durations` → insert-loop → `commit` → debug
  `checkSegmentIdUniqueness()`. They have **already drifted**: `saveDurations`
  uses an `ON CONFLICT` upsert, `replaceAll` uses plain inserts.
- **Change:**
  - Extract a private helper for the insert loop only, e.g.
    `bool insertRows(QSqlQuery& query, const std::deque<TimeDuration>& rows)`,
    and use it from both methods' insert loops.
  - **Nuance — do NOT over-merge:** `saveDurations` additionally handles
    `removedSegmentIds` orphan deletes and uses an upsert (needed for the Append
    path / `OrphanCheckpoint.id` preservation). `replaceAll` does a full `DELETE`
    first, then writes finalized rows (`is_finalized=1`) and current-session rows
    (`is_finalized=0`) with plain inserts. Keep those differences; only the bind +
    `exec` + rollback-on-error loop is the shared factor. `replaceAll` must still
    emit the `is_finalized=0` rows.
- **Severity:** Medium · **Effort:** M · **Risk:** Both paths are tested; the
  finalized/unfinalized split must be preserved.
- **Verify:** `test_historydialog`, `test_integration`, `test_database`.

### - [x] T1.3 — De-duplicate the debug-guard boilerplate in `replaceCurrentDurations`
- **Principle:** Pull Complexity Downwards (#4); Comments (#6).
- **Location:** `timer.cpp:853-913`.
- **Problem:** The method has several guard clauses, and each repeats the
  identical `#ifndef QT_NO_DEBUG / checkDurationInvariants() / #endif` block — it
  appears 5× in one method, burying the actual control flow (when does the
  checkpoint get written?) under boilerplate.
- **Change:** Extract the checkpoint-write portion into a private helper (e.g.
  `maybeReanchorCheckpoint(const TimeDuration& seg)`) that may return early
  freely, and call `checkDurationInvariants()` exactly once at the end of
  `replaceCurrentDurations`. The invariant check is debug-only and idempotent, so
  consolidating to one exit call changes nothing observable.
- **Severity:** Medium · **Effort:** M · **Risk:** Low — pure restructuring.
- **Verify:** `test_timer`.

---

## Tier 2 — Shallow modules & naming

### - [x] T2.1 — Rename `MainWin::update()`
- **Principle:** Better Naming (#5).
- **Location:** `mainwin.h:47`, `mainwin.cpp:72`; connection in `main.cpp`.
- **Problem:** `update()` is maximally generic **and** shadows
  `QWidget::update()` (the repaint method) in a `QMainWindow` subclass — actively
  misleading. It actually means "refresh time labels + run health warnings each
  tick."
- **Change:** Rename to `onTick()` (or `refreshOnTick()`); update the heartbeat
  `connect(...)` in `main.cpp`. Grep for any other call site first.
- **Severity:** Medium · **Effort:** S · **Risk:** Must update the `main.cpp`
  connection in the same change; check tests don't invoke `update()` by name.
- **Verify:** build + `./qtest` (no specific suite; compile-checked rename).

### - [x] T2.2 — Remove the empty `reactOnLockState` slot
- **Principle:** Deep vs Shallow Modules (#1) — a wired-up no-op.
- **Location:** `mainwin.cpp:108`, `mainwin.h:51`; connection at `main.cpp:159`.
- **Problem:** The slot has an empty body and a comment noting GUI transitions are
  now driven by `Timer::modeChanged()`. It exists only to satisfy a signal/slot
  connection. **Verified:** no test references `reactOnLockState`.
- **Change:** Delete the slot (declaration + definition) and its
  `desktopLockEvent → reactOnLockState` connection in `main.cpp`. Leave the
  `LockStateWatcher::desktopLockEvent` signal itself (still consumed by `Timer`).
- **Severity:** Medium · **Effort:** S · **Risk:** Low — confirm no other consumer
  of that specific connection.
- **Verify:** build + `./qtest`.

### - [x] T2.3 — Inline `SegmentIdTracker` into `SessionState`
- **Principle:** Deep vs Shallow Modules (#1) — shallow wrapper.
- **Location:** `timer.h:27-33` and its use across `SessionState` transitions.
- **Problem:** `SegmentIdTracker` wraps a single public `SegmentId current` field
  with three one-line methods. The field is read directly everywhere
  (`session_.id_tracker.current`), so the wrapper hides nothing; the "named,
  intentional mutation" role is already filled by `SessionState`'s logged
  transition methods (`beginNewSegment`, `clearSegment`, `adoptOngoingSegment`),
  which are its only callers.
- **Change:** Replace the tracker with a plain `SegmentId segment_id` member on
  `SessionState`; delete `SegmentIdTracker`. Mechanically rename
  `id_tracker.current → segment_id` at all call sites (`takeStateSnapshot`,
  `getOngoingDuration`, the `addDuration` sites, `saveCheckpointInternal`).
- **Severity:** Medium · **Effort:** M · **Risk:** Low — mechanical rename, no
  behavior change; StateGuard snapshot reads the same value.
- **Verify:** `test_timer`.

### - [x] T2.4 — Drop the unused `duration` param from `saveCheckpoint`
- **Principle:** Deep vs Shallow Modules / narrower interface (#1).
- **Location:** `sqlitesessionstore.cpp:824` (`Q_UNUSED(duration)`), `sessionstore.h:136`.
- **Problem:** `saveCheckpoint(type, duration, start, end, segmentId)` immediately
  `Q_UNUSED`s `duration` because it is recomputed from timestamps at load and the
  schema has no duration column. The param widens the pure-virtual interface and
  falsely implies the value is persisted.
- **Change:** Remove `duration` from the virtual in `sessionstore.h`, the impl,
  the `Timer` call site, and `FakeSessionStore`. All call sites are in-repo and
  compiler-checked.
- **Severity:** Medium · **Effort:** S · **Risk:** Low — value was already discarded.
- **Verify:** `test_database`, `test_timer`.

### - [x] T2.5 — Fix `getBackpauseMin()` name/type
- **Principle:** Better Naming (#5); minor Information Hiding (#2).
- **Location:** `settings.h:39`, `settings.cpp:108`; one caller in `contentwidget.cpp`.
- **Problem:** `getBackpauseMin()` returns a **pre-stringified `QString`**, while
  its sibling `getBackpauseMsec()` returns `qint64`. The name implies "minutes"
  (an int) but the type is a display string, and it pushes number→string
  formatting into `Settings`.
- **Change:** Rename to `getBackpauseMinString()` (lowest-risk, one caller), or
  return `int` and stringify in `contentwidget`. Update the caller and tests.
- **Severity:** Low · **Effort:** S · **Risk:** Low.
- **Verify:** `test_settings`.

---

## Tier 3 — Comment / doc truth & named constants

### - [x] T3.1 — Purge stale `lazyOpen` references
- **Principle:** Comments (#6) — comment/doc names a symbol that no longer exists.
- **Location:** `sessionstore.h:13`; `doc/persistence.md:15` and `:42`;
  `qtest/test_database.cpp:36,48` (comments).
- **Problem:** All four spots name a method `lazyOpen` that was renamed; the
  actual helpers are `ensureOpen` / `ensureSchema` / `lazyClose`. Anyone grepping
  the doc for `lazyOpen` finds nothing.
- **Change:** Replace `lazyOpen` with `ensureSchema` (schema/PRAGMA setup) or
  `ensureOpen` (connection health) as appropriate in each location.
- **Severity:** Low · **Effort:** S · **Risk:** None (comments/docs only).
- **Verify:** N/A (no code change).

### - [x] T3.2 — Correct `loadDurations`' doc comment
- **Principle:** Comments (#6) — comment contradicts code.
- **Location:** `sqlitesessionstore.cpp:671-682`.
- **Problem:** The header comment promises "duration must be non-negative" and
  "computed duration must match stored duration within reconciliation tolerance."
  Neither happens — there is no stored duration column to compare against; the
  only check is `start.msecsTo(end) > 0`.
- **Change:** Rewrite the comment to the actual validations: valid type enum,
  valid/parseable UTC timestamps, `start ≤ end`, positive computed duration.
  Remove the "stored duration / tolerance" bullet.
- **Severity:** Low · **Effort:** S · **Risk:** None (comment only).
- **Verify:** N/A.

### - [x] T3.3 — Name the `normalized()` decision table
- **Principle:** Comments (#6) — missing *why*; Information Hiding (#2) — leaked thresholds.
- **Location:** `timeline.cpp:135-206`.
- **Problem:** The merge loop has seven numbered, order-dependent,
  mutually-exclusive branches with magic thresholds (50, 500, 100 ms). A reader
  cannot tell that branch ordering is significant or whether the cases are
  exhaustive. This is the most error-prone code in the slice.
- **Change:** **Do not touch the logic.** Promote the literals to named
  `constexpr` (e.g. `kNearDuplicateMs = 50`, `kSmallGapMergeMs = 500`,
  `kSlightOverlapMs = 100`) with a one-line rationale each, and add a 2-line block
  comment stating the precondition (sorted by start, then end) and that branches
  are mutually exclusive + order-dependent. Constants must equal the literals.
- **Severity:** Medium · **Effort:** S · **Risk:** Very low if constants equal
  literals; heavily tested — do not alter any condition or threshold value.
- **Verify:** `test_timeline`, `test_cleanduration`.

### - [x] T3.4 — De-duplicate ini-key strings
- **Principle:** Information Hiding (#2) / Togetherness (#7).
- **Location:** `settings.cpp:30-43` (read) vs `48-58` (write).
- **Problem:** Every ini key literal appears twice — once in `readSettingsFile`,
  once in `writeSettingsFile`. The two must stay in lockstep; a typo in one
  silently drops a setting. The keys are the one detail `Settings` should fully
  own and not repeat.
- **Change:** Hoist the keys into named `static const QString` (or `constexpr`)
  constants referenced by both functions.
- **Severity:** Low · **Effort:** S · **Risk:** Low — round-trip test catches any mismatch.
- **Verify:** `test_settings`.

### - [x] T3.5 — Trim purely-restating comments
- **Principle:** Comments (#6) — redundant restatement.
- **Location:** e.g. `mainwin.cpp:233` (`// Return TRUE to allow shutdown` above
  `*result = TRUE;`); `historydialog.cpp` `// Connect signals` above `connect(...)`,
  `// Dialog control buttons`.
- **Problem:** A handful of comments restate the immediately following line with no
  added *why*, adding noise.
- **Change:** Delete the purely-restating one-liners. **Keep** the genuine
  *why*-comments (the codebase has good ones, e.g. the RichText/openExternalLinks
  explanation in `historydialog.cpp`). Low priority.
- **Severity:** Low · **Effort:** S · **Risk:** None (comments only).
- **Verify:** N/A.

---

## Tier 4 — Localized cleanups

### - [x] T4.1 — Remove the impossible null-check in Logger
- **Principle:** Define Errors Out of Existence (#3).
- **Location:** `logger.cpp:39-48`.
- **Problem:** `out << msg` is guarded by `if (logfile_ != nullptr)`, but
  `logfile_` is `new`'d unconditionally in the constructor and is never null here
  — the guard checks an impossible scenario.
- **Change:** Remove the `if (logfile_ != nullptr)` guard. Optionally trim the
  perf-speculation comment to one line.
- **Severity:** Low · **Effort:** S · **Risk:** Low — guard was always true.
- **Verify:** `test_logger`.

### - [x] T4.2 — Drop the redundant inner `isValid()`
- **Principle:** Comments (#6) / minor duplication.
- **Location:** `lockstatewatcher.cpp:195` (inner), guarded by `:194` (outer).
- **Problem:** On the `LongOngoingLock` path the outer condition already gates on
  `lock_timer_.isValid()`, so the inner `if (lock_timer_.isValid())` is always
  true — dead-condition noise in a hot 100 ms path.
- **Change:** Remove the redundant inner `if`. (Optional: extract a tiny
  `logLockDuration()` to remove the duplicated log string.)
- **Severity:** Low · **Effort:** S · **Risk:** Low — behavior unchanged.
- **Verify:** `test_lockstatewatcher`.

### - [x] T4.3 — Tidy ShutdownCoordinator's double event-loop pump
- **Principle:** Pull Complexity Downwards (#4); Comments (#6).
- **Location:** `shutdowncoordinator.cpp:33-46`.
- **Problem:** The path stops the timer, busy-waits ~150 ms pumping events, then
  re-checks and does a second stop + ~70 ms pump as a "somehow still running"
  fallback. The magic durations (150/70/30 ms) and duplicated stop+pump make the
  control flow subtle.
- **Change:** Extract `stopAndPump(int budgetMs)` to remove the duplicated
  busy-wait, and add a one-line *why* comment naming the reason a second pump can
  be needed. **Keep the two-attempt semantics** unless proven unnecessary.
- **Severity:** Medium · **Effort:** M · **Risk:** Timing-sensitive; runs during
  real OS shutdown — change carefully.
- **Verify:** `test_shutdowncoordinator`.

### - [ ] T4.4 — (Optional / stretch) Share the SELECT row-parser
- **Principle:** Togetherness vs Separation (#7).
- **Location:** `sqlitesessionstore.cpp:710` (`loadDurations`) and `:1097`
  (`loadUnfinalizedCheckpoints`).
- **Problem:** Both loops parse the same column shape (segment_id, type, start_utc,
  end_utc), validate the type enum and timestamps, and emit near-identical logs —
  so they can drift (one logs skipped rows, the other silently `continue`s).
- **Change:** Extract a private helper that parses one row into a validated
  `std::optional<{SegmentId, DurationType, start, end}>`, used by both loaders.
  **Only proceed if** it preserves each loader's distinct logging and
  `loadDurations`' cross-midnight acceptance. Lower confidence — skip if it adds
  friction.
- **Severity:** Low · **Effort:** M · **Risk:** Both loaders are tested.
- **Verify:** `test_database`, `test_integration`.

---

## Do NOT do (verified)

These surfaced in review as "dead / removable" but were **falsified by grepping
the test suite or tracing data flow**. Leave them alone.

- **`createSegmentId()`** — `types.h:57`. A duplicate of `SegmentId::mint()`, but
  **used by 4 sites** in `qtest/test_historydialog.cpp`. Not dead.
- **`appendDurationsToDB()`** — `timer.cpp:970`. **Used by** `test_timer.cpp:1073`.
- **`setSecondSegmentType()`** — `historydialog.cpp:907`. Production `onSplitRow`
  doesn't call it, but **4 test sites** in `test_historydialog.cpp` do.
- **`LoadResult.repaired`** — `sessionstore.h:47`. Not dead: plumbed via `Timer`
  into the HistoryDialog "N rows auto-repaired" banner and pinned by a dedicated
  test (`test_database_load_..._increments_repaired_...`). Whether the store still
  sets it `>0` is a **behavior decision**, not a cleanup — out of scope here.
- **Splitting `historydialog.cpp` (936 lines)** — the length driver is
  `updateTable` (see T1.1), not the two-class (`HistoryDialog` + `SplitDialog`)
  layout. Don't force a file split.

