# uTimer Project Overview

uTimer is a lightweight, cross-platform (Windows & Linux) time tracking application designed for remote work. Its primary feature is automatic pause detection when the computer is locked, ensuring accurate tracking of active work time. It features a robust architecture for data integrity, handling system crashes and daily session boundaries.

## Build and Run

The project uses Qt 5 and `qmake`.

**Prerequisites:**
- Qt 5 development libraries (`qtbase5-dev`, `qt5-default` or similar on Linux)
- C++17 compliant compiler
- `libdbus-1-dev` (on Linux)

## Architecture

uTimer follows a component-based architecture using Qt's Signal/Slot mechanism. The main event loop runs at ~100ms resolution to drive UI updates and lock detection.

### Core Components

*   **`TimeTracker` (`timetracker.cpp`)**: The central engine. It manages the state machine (Activity, Pause, Stopped) and accumulates time durations. It handles the backpause logic (retroactively converting lock time to pause) and periodic checkpoints. It uses `QElapsedTimer` for elapsed time and a `std::deque<TimeDuration>` for completed segments, guarded by `QRecursiveMutex`.
*   **`LockStateWatcher` (`lockstatewatcher.cpp`)**: Handles platform-specific lock detection.
    *   **Windows**: Uses `WtsApi32.dll` to query session state.
    *   **Linux**: Probes DBus services in order: `systemd-logind` → `freedesktop.ScreenSaver` → `GNOME` → `KDE`.
    *   Uses a 5-sample buffer to debounce lock state changes.
*   **`DatabaseManager` (`databasemanager.cpp`)**: Manages SQLite persistence.
    *   Uses a **Lazy-Open** pattern (`lazyOpen()`/`lazyClose()`) to prevent file locking issues.
    *   Schema: `durations` table (id, type, duration, end_date, end_time).
    *   Creates timestamped backups (`.backup` files) before major write operations.
    *   Manages "Checkpoints" (lightweight updates) vs "Full Saves" (transactional replaces).
    *   `history_days_to_keep_ = 0` disables database entirely.
*   **`MainWin` (`mainwin.cpp`)**: The orchestrator. Manages the system tray, window state, and the **Midnight Boundary** logic. The app enforces a strict split of sessions at 23:59:59 by automatically stopping and restarting the timer to ensure accurate per-day statistics.
*   **`HistoryDialog` (`historydialog.cpp`)**: Editable history view. It pauses checkpoints while open to prevent race conditions.

### Key Data Flow

1. Lock events or button presses trigger `TimeTracker` state changes.
2. `TimeTracker` accumulates completed `TimeDuration` entries in `durations_`.
3. The ongoing segment is tracked separately by `timer_.elapsed()` and `current_checkpoint_id_`.
4. Checkpoints periodically sync the ongoing segment to the DB.
5. On stop, durations are cleaned (deduplicated/merged) and saved via `saveDurations()`.
6. `HistoryDialog` can edit both the current session and historical data.

### Checkpoints (Crash Recovery)

To prevent data loss during power failures or crashes, the app saves a "Checkpoint" every 5 minutes.
*   **Only in Activity mode**: No checkpoints during Pause or Stopped.
*   **Row reuse**: Updates the same DB row via `current_checkpoint_id_` to avoid bloat.
*   **Suspended while PC locked**: `is_locked_` prevents saves; a final checkpoint is saved on lock.
*   **Suspended while HistoryDialog open**: `checkpoints_paused_` prevents race conditions.
*   **ID reset**: `current_checkpoint_id_` resets to -1 on pause/stop/backpause.

### Autopause / Backpause

When the computer is locked:
1. `LockStateWatcher` detects the lock and emits `LockEvent::Lock`.
2. `TimeTracker` saves a final checkpoint and sets `is_locked_ = true`.
3. If the lock duration exceeds the threshold (default 15 minutes), a `LockEvent::LongOngoingLock` is emitted.
4. `backpauseTimer()` retroactively converts the last N minutes from Activity to Pause, splits the duration segments, and syncs the correction to the DB via `updateDurationsInDB()`.

### Linux Signal Handling

On Linux, the app uses a `socketpair` approach to safely handle Unix signals (`SIGTERM`, `SIGINT`). The signal handler writes a byte to a socket, which triggers a `QSocketNotifier` in the main Qt event loop, allowing a graceful shutdown (stop timers, save DB) before exit.

### Important Types (`types.h`)

```cpp
enum class LockEvent {None, Unlock, Lock, LongOngoingLock};
enum class DurationType {Activity, Pause};
struct TimeDuration { DurationType type; qint64 duration; QDateTime endTime; };
```

### Platform-Specific Code

- Windows lock detection: `lockstatewatcher.cpp` lines 46-105
- Linux lock detection: `lockstatewatcher.cpp` lines 152-291
- Conditional compilation via `Q_OS_WIN` and `Q_OS_LINUX` macros

## Development Conventions

*   **Commenting:** Comments are essential for abstraction. They must capture intent, rationale, usage conditions, and high-level behavior, not restate code. "Self-documenting code" is considered a myth here.
*   **Data Integrity:** All database writes are transactional. History editing (via `HistoryDialog`) works on a local copy and performs a full atomic replace on save.
*   **Platform Specifics:** Code for Windows/Linux is separated by `#ifdef Q_OS_WIN` / `#ifdef Q_OS_LINUX`.

## Commenting Philosophy

This project rejects the idea that "good code is self-documenting." Code can express formal structure, but only comments can express intent, rationale, and constraints.

**Comments should provide:**
- High-level descriptions of what methods do and the meaning of their results
- Rationale for design decisions
- Conditions under which it makes sense to call a particular method
- Information that reduces cognitive load and eliminates "unknown unknowns"

**Comments are not failures.** They increase the value of code and define abstractions that would otherwise be hidden in implementation details.

## Configuration

Settings are stored in `user-settings.ini` (loaded by `settings.cpp`). Key settings:
- `autopause_threshold_minutes`: Lock duration before backpause triggers (default: 15)
- `history_days_to_keep`: Days to retain history (default: 99, 0 = disable database entirely)
- `debug_log_to_file`: Enable verbose logging to `uTimer.log` (default: false)

## Build Commands

```bash
# Build (requires Qt5)
qmake-qt5 && make -j4

# Clean build
make clean

# Full clean (removes generated files)
make distclean
```

The project uses Qt5 qmake. Build output goes to `build/` directory. Executable is `uTimer`.

## Testing

Tests are in `qtest/` using Qt Test Framework. The main test suite is in `qtest/utimertest.h` (implementation in `qtest/utimertest.cpp`).

**Running tests:**
```bash
cd qtest
qmake-qt5 && make -j4
./qtest
```

**Test coverage includes:**
- `cleanDurations()` helper function (deduplication, merging, overlap handling)
- TimeTracker state machine (start, pause, resume, stop)
- Checkpoint save/restore and crash recovery
- Backpause logic and midnight boundary splits
- Lock state detection debouncing
- Database operations (CRUD, schema validation, retention)
- Explicit start time preservation across operations

**Expected output:** All tests should pass with zero `QWARN` messages (46 tests currently).

