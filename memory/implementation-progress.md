---
name: implementation-progress
description: Phase-by-phase progress tracker for improve-design-plan.md — check here to resume after interruption
metadata:
  type: project
---

# Implementation Progress: improve-design-plan.md

**Status as of 2026-05-14:** Phases 0, 1, 2, 3, and 4 complete. Ready to begin Phase 5.

## Overall Status

| Phase | Description | Status |
|-------|-------------|--------|
| 0 | Logging gating | DONE (T0.1–T0.6, test gate, review gate, review fixes) |
| 1 | Strip TimeTracker pass-throughs | DONE (T1.1–T1.5, test gate, review gate) |
| 2 | Extract ShutdownCoordinator and HealthMonitor | DONE (T2.1–T2.5+2.6, test gate, review gate, review fixes) |
| 3 | Timeline value type | DONE (T3.1–T3.7, test gate, review gate — no non-minor findings) |
| 4 | Collapse the three write methods | Not started — ready to begin T4.1 |
| 5 | Day-boundary policy into Timer | Not started |
| 6 | Renames | Not started |

## Phase 2 — Extract ShutdownCoordinator and HealthMonitor

**Key commits:**
- T2.1: ShutdownCoordinator skeleton
- T2.2: Move shutdown body into ShutdownCoordinator (GUI calls replaced with TimeTracker calls)
- T2.3: MainWin::shutdown becomes one-liner + GUI sync; wired in main.cpp
- T2.4: HealthMonitor skeleton
- T2.5+T2.6: Move health-warning policy into HealthMonitor::check(); reset() on session start
- T2 test gate: Tests G–L (ShutdownCoordinator: G happy, H idempotent, I force-direct; HealthMonitor: J fires once, K nopause condition, L reset re-arms)
- T2 review fixes: restore GUI sync in MainWin::shutdown, remove unused Settings from ShutdownCoordinator, improve Test I timing assertion

**Notes:**
- T2.5 and T2.6 were merged into one commit because removing the _shown_ flags from MainWin forced the reset() call in the same change
- ShutdownCoordinator does NOT take Settings (removed as unused after review)
- MainWin::shutdown calls shutdown_coordinator_.run() then content_widget_->setGUItoStop()
- Also fixed a pre-existing bug: assertPendingOriginsInvariant called after std::swap in HistoryDialog::saveChanges() was wrong

## Phase 3 — Timeline value type

- [x] T3.1 — Add Timeline value type (08ebab5)
- [x] T3.2 — Make cleanDurations forward to Timeline::normalized() (1475af3)
- [x] T3.3 — Add TimeTracker::snapshot() (294fe65)
- [x] T3.4 — HistoryDialog: build pages from a Timeline (8216e1d)
- [x] T3.5 — HistoryDialog: edits produce a new Timeline (c728acf)
- [x] T3.6 — HistoryDialog: save by handing a Timeline back (6449476)
- [x] T3.7 — Delete RowOrigin, ongoingSnapshot_, calculateTotals (da4ecc6)
- [x] T3 Test Gate — Tests M–S (d6d00b2)
- [x] T3 Review Gate — no non-minor findings

**Notes:**
- RowOrigin replaced by isMemoryRow_ (vector<vector<bool>>) — same concept, no named enum
- cleanDurations in helpers.cpp now forwards to Timeline::normalized()
- snapshot() uses QRecursiveMutex so getOngoingDuration() can be called inside the lock
- calculateTotals() helper deleted (T3.7); replaced by Timeline::activeMsec()/pauseMsec()

## Phase 4 — Collapse the three write methods

- [x] T4.1 — Add IDatabaseManager::commitSession() (6e9c783)
- [x] T4.2 — Migrate TimeTracker write call-sites (a23e2b0)
- [x] T4.3 — Move orphan-ID computation into DatabaseManager::commitSession (cf9a491)
- [x] T4.4 — Remove saveDurations and updateDurationsById from IDatabaseManager (7707109)
- [x] T4.5 — replaceDurationsInDB becomes replaceAll(history, session) (3b7838a)
- [x] T4.6 — address review: move TransactionMode out of types.h (e7af4c2)
- [x] T4 Test Gate — Tests T–X (923fe78)
- [x] T4 Review Gate — one non-minor finding addressed (TransactionMode in types.h)

**Notes:**
- saveDurations and updateDurationsById kept as public non-virtual on DatabaseManager for test seeding; removed from IDatabaseManager interface only
- TransactionMode moved from types.h to databasemanager.h (no longer in global types)
- IDatabaseManager narrowed from 12 to 9 virtual methods (T4 scope; further narrowing in Phase 6)
- removedSegmentIds is now entirely internal to DatabaseManager::commitSession
- cleanDurations is no longer called by TimeTracker before write operations

## Phase 5 — Day-boundary policy into Timer

- [ ] T5.0a — Add midnight-scenario regression tests
- [ ] T5.1 — Introduce DayBoundaryWatcher inside TimeTracker
- [ ] T5.2 — Engine-owned scheduled midnight stop
- [ ] T5.3 — Engine-owned watchdog
- [ ] T5.4 — Engine emits stopped(reason) signal
- [ ] T5.5 — Remove duplicated was_active_before_autopause_
- [ ] T5.6 — Remove GUI back-channel for stops
- [ ] T5 Test Gate — Tests Y, Z, AA, AB
- [ ] T5 Review Gate

## Phase 6 — Renames

- [ ] T6.1 — Rename IDatabaseManager to SessionStore
- [ ] T6.2 — Rename DatabaseManager to SqliteSessionStore
- [ ] T6.3 — Rename FakeDatabaseManager to FakeSessionStore
- [ ] T6.4 — Rename TimeTracker to Timer
- [ ] T6.5 — Sanity sweep
- [ ] T6 Test Gate
- [ ] T6 Review Gate

## How to resume after interruption

1. Read this file to see current status
2. Find the first unchecked item
3. Read the corresponding phase in improve-design-plan.md for detailed instructions
4. Run `cd /home/mario/Coding/uTimer && git log --oneline -10` to see recent commits
5. Run build + tests to verify current state: `cd /home/mario/Coding/uTimer && qmake-qt5 && make -j4 && cd qtest && qmake-qt5 && make -j4 && ./qtest`

**Why:** User asked for disruption-safe implementation tracking on 2026-05-13.
