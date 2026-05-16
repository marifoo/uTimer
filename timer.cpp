/**
 * Timer — Core timing engine managing Activity/Pause/None states.
 *
 * Architecture:
 * - State machine with three modes: Activity, Pause, None.
 * - Completed segments stored in session_.durations (deque); ongoing
 *   segment tracked by timer_.elapsed() and session_.segment_start_time.
 * - Checkpoints persist the ongoing Activity segment every 5 min.
 * - Thread-safe via QRecursiveMutex; per-session mutable state grouped
 *   in SessionState with explicit logged transitions.
 *
 * Day-boundary policy:
 * - Timer NEVER stores or persists a segment whose
 *   startTime.date() != endTime.date(). Such segments are silently
 *   discarded by addDuration() and refused by saveCheckpointInternal().
 * - The day-boundary rule is owned entirely by DayBoundaryWatcher: a
 *   scheduled stop at 23:59:59.500 and a 100 ms watchdog (via onTick())
 *   drive the engine stop path. MainWin has no midnight logic.
 * - Stops emit stopped(StopReason) so the GUI syncs without polling.
 *
 * Checkpoint behavior:
 * - Only saved during Activity mode (not Pause or Stopped)
 * - Suspended while PC is locked (is_locked_) or HistoryDialog is open (dialog_open_)
 * - Uses session_.current_checkpoint_segment_id to update same DB row instead of creating new rows
 *
 * Backpause behavior:
 * - When PC locked for longer than threshold, retroactively converts last N minutes to Pause
 * - Triggered by LockEvent::LongOngoingLock from LockStateWatcher
 */

#include "timer.h"
#include <QtDebug>
#include <QDateTime>
#include <QMutexLocker>
#include <algorithm>
#include <new>  // for std::bad_alloc
#include "logger.h"
#include "helpers.h"

namespace {
constexpr qint64 kMinRecoverableOrphanDurationMs = 1000;
constexpr qint64 kOrphanStaleAgeMs = 24LL * 60LL * 60LL * 1000LL;
}

// ============================================================================
// DayBoundaryWatcher implementation
// ============================================================================

Timer::DayBoundaryWatcher::DayBoundaryWatcher(Timer& owner)
    : owner_(owner)
{
    midnight_timer_.setSingleShot(true);
}

void Timer::DayBoundaryWatcher::tick(const QDateTime& now)
{
    // Watchdog: if the ongoing segment has crossed midnight (e.g. due to sleep
    // bypassing the scheduled stop), force the engine to stopped state.
    // The mutex is already held by the caller (Timer::onTick).
    if (owner_.dialog_open_) {
        return;
    }
    owner_.discardCrossMidnightOngoingAndStop(now);
}

void Timer::DayBoundaryWatcher::armScheduledStop(const QDateTime& now)
{
    QTime current_time = now.time();
    QTime midnight_stop_time(23, 59, 59, 500);

    qint64 msecs_until_stop;
    if (current_time < midnight_stop_time) {
        msecs_until_stop = current_time.msecsTo(midnight_stop_time);
    } else {
        // Already past 23:59:59.500 — fire in 1 ms to trigger the watchdog path.
        msecs_until_stop = 1;
    }

    Logger::Log(QString("[MIDNIGHT] Scheduled engine auto-stop in %1 seconds")
        .arg(msecs_until_stop / 1000.0, 0, 'f', 1));

    midnight_timer_.disconnect();
    QObject::connect(&midnight_timer_, &QTimer::timeout,
                     [this]() { onMidnightTimerFired(); });
    midnight_timer_.start(static_cast<int>(msecs_until_stop));
}

void Timer::DayBoundaryWatcher::cancel()
{
    midnight_timer_.stop();
    Logger::Log("[MIDNIGHT] Scheduled stop timer cancelled");
}

void Timer::DayBoundaryWatcher::onMidnightTimerFired()
{
    Logger::Log("[MIDNIGHT] Scheduled stop: engine forcing timer off at end of day");
    QMutexLocker locker(&owner_.mutex_);
    if (owner_.mode_ == Mode::None) {
        Logger::Log("[MIDNIGHT] Scheduled stop fired but engine already stopped - ignoring");
        return;
    }
    if (owner_.dialog_open_) {
        owner_.pending_midnight_stop_ = true;
        Logger::Log("[MIDNIGHT] suspended during history edit");
        return;
    }
    owner_.stopTimer(QDateTime::currentDateTime(), StopReason::MidnightScheduled);
}

// ============================================================================
// SessionState transition methods
// ============================================================================

void SessionState::beginNewSegment(const QDateTime& startTime, const Settings& settings)
{
    (void)settings;
    QString oldId = current_checkpoint_segment_id;
    QDateTime oldStart = segment_start_time;
    current_checkpoint_segment_id = TimeDuration::createSegmentId();
    segment_start_time = startTime;
    Logger::Log(QString("[STATE] beginNewSegment: segId '%1' -> '%2', start %3 -> %4")
        .arg(oldId, current_checkpoint_segment_id,
             oldStart.toString(Qt::ISODate), startTime.toString(Qt::ISODate)));
}

void SessionState::clearSegment(const Settings& settings)
{
    (void)settings;
    QString oldId = current_checkpoint_segment_id;
    QDateTime oldStart = segment_start_time;
    current_checkpoint_segment_id.clear();
    segment_start_time = QDateTime();
    Logger::Log(QString("[STATE] clearSegment: segId '%1' -> (empty), start %2 -> (invalid)")
        .arg(oldId, oldStart.toString(Qt::ISODate)));
}

void SessionState::updateSegmentStartTime(const QDateTime& newStart, const Settings& settings)
{
    (void)settings;
    QDateTime oldStart = segment_start_time;
    segment_start_time = newStart;
    Logger::Log(QString("[STATE] updateSegmentStartTime: %1 -> %2")
        .arg(oldStart.toString(Qt::ISODate), newStart.toString(Qt::ISODate)));
}

void SessionState::markUnsaved(const Settings& settings)
{
    (void)settings;
    if (!has_unsaved_data) {
        has_unsaved_data = true;
        Logger::Log("[STATE] markUnsaved: false -> true");
    }
}

void SessionState::clearUnsaved(const Settings& settings)
{
    (void)settings;
    bool old = has_unsaved_data;
    has_unsaved_data = false;
    unsaved_durations.clear();
    if (old) {
        Logger::Log("[STATE] clearUnsaved: true -> false");
    }
}

void SessionState::resetForNewSession(const Settings& settings)
{
    (void)settings;
    size_t oldSize = durations.size();
    bool oldUnsaved = has_unsaved_data;
    durations.clear();
    has_unsaved_data = false;
    unsaved_durations.clear();
    Logger::Log(QString("[STATE] resetForNewSession: durations %1 -> 0, unsaved %2 -> false")
        .arg(oldSize).arg(oldUnsaved ? "true" : "false"));
}

void SessionState::adoptOngoingSegment(const TimeDuration& ongoing, const Settings& settings)
{
    (void)settings;
    QString oldId = current_checkpoint_segment_id;
    QDateTime oldStart = segment_start_time;
    current_checkpoint_segment_id = ongoing.segment_id.isEmpty()
        ? TimeDuration::createSegmentId()
        : ongoing.segment_id;
    segment_start_time = ongoing.startTime;
    Logger::Log(QString("[STATE] adoptOngoingSegment: segId '%1' -> '%2', start %3 -> %4")
        .arg(oldId, current_checkpoint_segment_id,
             oldStart.toString(Qt::ISODate), ongoing.startTime.toString(Qt::ISODate)));
}

// ============================================================================
// Debug-build state invariant checking
// ============================================================================

#ifndef QT_NO_DEBUG

Timer::StateSnapshot Timer::takeStateSnapshot() const
{
    return {
        session_.durations.size(),
        session_.current_checkpoint_segment_id,
        session_.segment_start_time,
        session_.has_unsaved_data
    };
}

Timer::StateGuard::StateGuard(const Timer& tracker, const char* methodName)
    : tracker_(tracker), method_(methodName), entry_(tracker.takeStateSnapshot())
{
}

Timer::StateGuard::~StateGuard()
{
    if (transitioned_) {
        return; // Caller acknowledged the mutation
    }
    auto exit = tracker_.takeStateSnapshot();
    bool changed = (exit.durations_size != entry_.durations_size)
                || (exit.segment_id != entry_.segment_id)
                || (exit.segment_start_time != entry_.segment_start_time)
                || (exit.has_unsaved_data != entry_.has_unsaved_data);
    if (changed) {
        qWarning("[STATE-GUARD] Unacknowledged state mutation in %s: "
                 "durations %zu->%zu, segId '%s'->'%s', unsaved %d->%d",
                 method_,
                 entry_.durations_size, exit.durations_size,
                 qPrintable(entry_.segment_id), qPrintable(exit.segment_id),
                 entry_.has_unsaved_data, exit.has_unsaved_data);
        Q_ASSERT_X(false, method_,
                    "SessionState changed without explicit transition call");
    }
}

void Timer::StateGuard::markTransitioned()
{
    transitioned_ = true;
}

/**
 * Checks structural invariants on the in-memory duration segments.
 *
 * This runs after every public method in debug builds. It catches
 * data-structure corruption early — before it propagates to the DB
 * or produces confusing UI behavior. Violations are logged as warnings
 * (not fatal) so tests surface them as QWARN output.
 *
 * Invariants checked:
 *   1. Segment ordering: startTimes must be non-decreasing.
 *   2. No overlapping segments of the same type on the same day.
 *   3. All segment_ids must be non-empty.
 */
void Timer::checkDurationInvariants() const
{
    const auto& durations = session_.durations;
    if (durations.empty()) {
        return;
    }

    for (size_t i = 0; i < durations.size(); ++i) {
        // Invariant 3: All segment_ids must be non-empty
        if (durations[i].segment_id.isEmpty()) {
            qWarning("[INVARIANT] Duration at index %zu has empty segment_id "
                     "(type=%d, start=%s, end=%s)",
                     i,
                     static_cast<int>(durations[i].type),
                     qPrintable(durations[i].startTime.toString(Qt::ISODate)),
                     qPrintable(durations[i].endTime.toString(Qt::ISODate)));
        }

        if (i == 0) {
            continue;
        }

        // Invariant 1: startTimes must be non-decreasing
        if (durations[i].startTime < durations[i - 1].startTime) {
            qWarning("[INVARIANT] Segment ordering violation at index %zu: "
                     "startTime %s < previous startTime %s",
                     i,
                     qPrintable(durations[i].startTime.toString(Qt::ISODate)),
                     qPrintable(durations[i - 1].startTime.toString(Qt::ISODate)));
        }

        // Invariant 2: No overlapping segments of the same type on the same day
        if (durations[i].type == durations[i - 1].type
            && durations[i].startTime.date() == durations[i - 1].startTime.date()
            && durations[i].startTime < durations[i - 1].endTime) {
            qWarning("[INVARIANT] Overlapping same-type segments on %s at indices %zu and %zu: "
                     "prev=[%s..%s], curr=[%s..%s], type=%s",
                     qPrintable(durations[i].startTime.date().toString(Qt::ISODate)),
                     i - 1, i,
                     qPrintable(durations[i - 1].startTime.toString(Qt::ISODateWithMs)),
                     qPrintable(durations[i - 1].endTime.toString(Qt::ISODateWithMs)),
                     qPrintable(durations[i].startTime.toString(Qt::ISODateWithMs)),
                     qPrintable(durations[i].endTime.toString(Qt::ISODateWithMs)),
                     durations[i].type == DurationType::Activity ? "Activity" : "Pause");
        }
    }
}

#endif // QT_NO_DEBUG

// ============================================================================
// Timer implementation
// ============================================================================

Timer::Timer(const Settings &settings, SessionStore& db, QObject *parent)
    : QObject(parent), settings_(settings), timer_(), session_(), mode_(Mode::None),
      was_active_before_autopause_(false), is_locked_(false),
      dialog_open_(false), db_(db),
      checkpoint_interval_msec_(settings.getCheckpointIntervalMsec()),
      last_history_load_skipped_(0), last_history_load_repaired_(0),
      startup_recovered_seconds_(0), startup_recovery_notification_needed_(false),
      day_boundary_watcher_(*this)
{
    // Setup checkpoint timer (disabled if interval is 0)
    if (checkpoint_interval_msec_ > 0) {
        checkpointTimer_.setInterval(checkpoint_interval_msec_);
    }
    checkpointTimer_.setSingleShot(false);
    connect(&checkpointTimer_, &QTimer::timeout, this, &Timer::saveCheckpoint);

    // Log when checkpoints are disabled
    if (checkpoint_interval_msec_ == 0) {
        Logger::Log("[CHECKPOINT] Checkpoints disabled (interval = 0)");
    }

    const std::optional<QDateTime> cleanShutdownMarker = db_.consumeLastCleanShutdownMarker();
    startup_recovered_seconds_ = reconcileOrphanCheckpoints(db_.loadUnfinalizedCheckpoints(), cleanShutdownMarker);
}

Timer::~Timer()
{
    stopTimer(QDateTime::currentDateTime(), StopReason::Shutdown);
}

/**
 * Transitions timer to Activity mode. Behavior depends on current state:
 * - From Pause: Records pause duration, persists it immediately to DB for
 *   crash safety, then switches to Activity
 * - From None: Clears state, optionally adds boot time, starts fresh
 * - From Activity: No-op (logs debug message)
 *
 * @param now  Wall-clock timestamp captured once by the caller. All time
 *             calculations within this method (and its callees) use this
 *             single value, preventing races from thread preemption or
 *             clock skew between successive QDateTime::currentDateTime()
 *             calls.
 */
void Timer::startTimer(const QDateTime& now)
{
    if (mode_ == Mode::Pause) {
        Logger::Log("[DEBUG] Starting Timer from Pause - D=" + QString::number(session_.durations.size()));
        qint64 t_pause = timer_.restart();
        if (t_pause > 0) {
            if (session_.durations.empty() || session_.durations.back().type != DurationType::Pause) {
                addDuration(DurationType::Pause, session_.segment_start_time, now, session_.current_checkpoint_segment_id);
            } else {
                session_.durations.back().endTime = now;
                session_.durations.back().duration = session_.durations.back().startTime.msecsTo(now);
            }
        }
        mode_ = Mode::Activity;
        session_.beginNewSegment(now, settings_);

        // Persist the completed Pause row immediately so it survives a crash.
        // Without this, a crash before the next pauseTimer/stopTimer would lose
        // the Pause entry, making the reloaded timeline show two Activities
        // back-to-back with no gap. The row is written as finalized (is_finalized=1)
        // because the Pause segment is complete the moment the user un-pauses.
        updateDurationsInDB();

        if (checkpoint_interval_msec_ > 0) {
            checkpointTimer_.start(); // Resume periodic checkpoint saving
        }
        day_boundary_watcher_.armScheduledStop(now);
        Logger::Log("[TIMER] > Timer unpaused");
        return;
    }
    if (mode_ == Mode::None) {
        Logger::Log("[DEBUG] Starting Timer from Stopped - D=" + QString::number(session_.durations.size()));

        bool retry_succeeded = true;

        // Try to save any unsaved data from previous failed save attempt.
        // Use unsaved_durations if present to keep retries idempotent.
        const std::deque<TimeDuration>& retry_source = session_.unsaved_durations.empty() ? session_.durations : session_.unsaved_durations;
        if (session_.has_unsaved_data && !retry_source.empty()) {
            Logger::Log("[DB] Retrying save of previously unsaved durations");
            if (appendDurationsChunkToDB(retry_source)) {
                session_.clearUnsaved(settings_);
                Logger::Log("[DB] Previously unsaved durations saved successfully");
            } else {
                retry_succeeded = false;
                Logger::Log("[DB] CRITICAL: Retry save failed - unsaved data retained for another retry");
                emit userWarning("Could not save previous session data. It is kept in memory and will be retried.");
            }
        }

        unsigned int boot_time_sec = settings_.getBootTimeSec();
        bool shouldAddBootTime = false;
        if (boot_time_sec > 0) {
            QDate today = now.date();
            bool hasEntriesInMemory = false;
            for (const auto &d : session_.durations) {
                if (d.endTime.date() == today) {
                    hasEntriesInMemory = true;
                    break;
                }
            }
            // Use tri-state result: only add boot time when the DB positively
            // confirms zero entries for today. When the result is Unknown
            // (history disabled or DB inaccessible), we err on the side of
            // caution and skip boot time to avoid double-counting.
            EntriesForDateResult dbResult = hasEntriesForDate(today);
            shouldAddBootTime = !hasEntriesInMemory && (dbResult == EntriesForDateResult::No);
            Logger::Log(shouldAddBootTime
                        ? "[TIMER] Will add boot time: " + QString::number(boot_time_sec) + " seconds (first session today)"
                        : "[TIMER] Boot time not added - entries already exist for today");
        }
        if (retry_succeeded) {
            session_.resetForNewSession(settings_);
        }
        if (shouldAddBootTime) {
            QDateTime bootStart = now.addMSecs(-static_cast<qint64>(boot_time_sec) * 1000);
            addDuration(DurationType::Activity, bootStart, now);
        }
        timer_.start();
        mode_ = Mode::Activity;
        session_.beginNewSegment(now, settings_);
        if (checkpoint_interval_msec_ > 0) {
            checkpointTimer_.start(); // Start periodic checkpoint saving
        }
        day_boundary_watcher_.armScheduledStop(now);
        Logger::Log("[TIMER] >> Timer started");
        return;
    }
    Logger::Log("[DEBUG] Trying to Start Timer from Mode Activity - D=" + QString::number(session_.durations.size()));
}

void Timer::pauseTimer(const QDateTime& now)
{
    if (mode_ != Mode::Activity) {
        Logger::Log("[DEBUG] Pause ignored; not in Activity");
        return;
    }
    Logger::Log("[DEBUG] Pausing Timer from Activity - D=" + QString::number(session_.durations.size()));
    timer_.restart();
    addDuration(DurationType::Activity, session_.segment_start_time, now, session_.current_checkpoint_segment_id);

    finalizeActivityToPause(now);
    Logger::Log("[TIMER] Timer paused <");
}

/**
 * Retroactively converts the last N minutes of Activity to Pause.
 *
 * Called when PC has been locked longer than the backpause threshold.
 * Splits current elapsed time into two segments:
 *   1. Activity segment: startTime = segment_start_time, endTime = now - backpause
 *   2. Pause segment: startTime = now - backpause, endTime = now
 *
 * This corrects any checkpoint data that was saved during the lock period,
 * since checkpoints are suspended while locked but the time before lock detection
 * may have been saved as Activity.
 */
void Timer::backpauseTimer(const QDateTime& now)
{
    if (mode_ != Mode::Activity) {
        Logger::Log("[DEBUG] Backpause ignored; not in Activity");
        return;
    }
    if (!settings_.isAutopauseEnabled()) {
        Logger::Log("[DEBUG] Autopause disabled; backpause ignored");
        return;
    }
    Logger::Log("[DEBUG] Backpausing Timer from Activity - D=" + QString::number(session_.durations.size()));
    qint64 backpause_msec = settings_.getBackpauseMsec();

    // Bounds check: backpause should be between 1 second and 1 hour
    constexpr qint64 MIN_BACKPAUSE_MS = 1000;      // 1 second minimum
    constexpr qint64 MAX_BACKPAUSE_MS = 3600000;   // 1 hour maximum
    if (backpause_msec < MIN_BACKPAUSE_MS) {
        Logger::Log(QString("[WARNING] Backpause value %1ms below minimum, using %2ms")
            .arg(backpause_msec).arg(MIN_BACKPAUSE_MS));
        backpause_msec = MIN_BACKPAUSE_MS;
    } else if (backpause_msec > MAX_BACKPAUSE_MS) {
        Logger::Log(QString("[WARNING] Backpause value %1ms exceeds maximum, using %2ms")
            .arg(backpause_msec).arg(MAX_BACKPAUSE_MS));
        backpause_msec = MAX_BACKPAUSE_MS;
    }

    timer_.restart();

    // Calculate Activity end time (backpause boundary)
    QDateTime activity_end = now.addMSecs(-backpause_msec);

    // Ensure activity_end is not before segment_start_time
    if (activity_end < session_.segment_start_time) {
        activity_end = session_.segment_start_time;
    }

    // Activity segment: segment_start_time to activity_end
    if (activity_end > session_.segment_start_time) {
        addDuration(DurationType::Activity, session_.segment_start_time, activity_end, session_.current_checkpoint_segment_id);
    }

    // Pause segment: activity_end to now
    QDateTime pause_start = activity_end;
    addDuration(DurationType::Pause, pause_start, now);

    finalizeActivityToPause(now);
    Logger::Log("[TIMER] Timer retroactively paused <");
}

/**
 * Completes the transition from Activity to Pause mode.
 *
 * This is the shared epilogue for both pauseTimer() (explicit pause) and
 * backpauseTimer() (retroactive auto-pause). Both callers are responsible
 * for adding the appropriate duration segments to session_.durations before calling
 * this method.
 *
 * Steps performed (order matters):
 *   1. Set mode to Pause.
 *   2. Record the wall-clock start of the new Pause segment.
 *   3. Sync all in-memory segments to the DB — this finalizes the Activity
 *      segment(s) just added and also cleans up any orphaned segment_ids
 *      left by cleanDurations merges (handled atomically by updateDurationsInDB).
 *   4. Assign a fresh segment_id for the ongoing Pause segment so the next
 *      resume (startTimer from Pause) can track it correctly.
 *   5. Stop the checkpoint timer — no periodic checkpoints during Pause.
 *
 * @param pauseSegmentStart  Wall-clock time at which the new Pause segment
 *                           begins. For pauseTimer this is "now"; for
 *                           backpauseTimer it is also "now" (the retroactive
 *                           Pause segment was already added to session_.durations).
 */
void Timer::finalizeActivityToPause(const QDateTime& pauseSegmentStart)
{
    mode_ = Mode::Pause;
    session_.updateSegmentStartTime(pauseSegmentStart, settings_);

    // Sync to DB: finalizes the Activity segment and cleans up orphaned
    // segment_ids from any cleanDurations merges (T14).
    updateDurationsInDB();

    // Fresh segment_id for the ongoing Pause. This ensures the next
    // checkpoint or save targets a new DB row for the Pause period.
    session_.beginNewSegment(pauseSegmentStart, settings_);

    checkpointTimer_.stop();
}

/**
 * Ends the current timing session.
 *
 * Workflow:
 * 1. Calculates the final segment duration (Activity or Pause).
 * 2. Adds it to the durations list.
 * 3. Stops the periodic checkpoint timer.
 * 4. Persists the Session to the Database using updateDurationsInDB().
 *    (This updates the existing rows rather than creating new ones, preserving IDs).
 */
void Timer::stopTimer(const QDateTime& now, StopReason reason)
{
    if (mode_ == Mode::None) {
        Logger::Log("[DEBUG] Stop ignored; already stopped");
        return;
    }
    if (mode_ == Mode::Pause) {
        Logger::Log("[DEBUG] Stopping from Pause - D=" + QString::number(session_.durations.size()));
        addDuration(DurationType::Pause, session_.segment_start_time, now, session_.current_checkpoint_segment_id);
    } else if (mode_ == Mode::Activity) {
        Logger::Log("[DEBUG] Stopping from Activity - D=" + QString::number(session_.durations.size()));
        addDuration(DurationType::Activity, session_.segment_start_time, now, session_.current_checkpoint_segment_id);
    }
    mode_ = Mode::None;
    session_.clearSegment(settings_);
    checkpointTimer_.stop();
    day_boundary_watcher_.cancel();
    was_active_before_autopause_ = false;
    Logger::Log("[TIMER] Timer stopped <<");

    // Persist current session durations only (not entire history). History dialog does replace explicitly.
    // Use update interface to check for existing entries by start time and update them instead of creating duplicates
    if (updateDurationsInDB()) {
        session_.resetForNewSession(settings_);
        // Use the same `now` for the shutdown marker to maintain a single
        // consistent timestamp across the entire stop operation.
        db_.setLastCleanShutdownMarker(now);
        Logger::Log("[DB] Session durations updated");
    } else {
        session_.markUnsaved(settings_);
        session_.unsaved_durations = session_.durations;
        Logger::Log("[DB] Error updating session durations - data retained for next save attempt");
    }

    emit stopped(reason);
}

void Timer::useTimerViaButton(Button button)
{
    QMutexLocker locker(&mutex_);
#ifndef QT_NO_DEBUG
    StateGuard guard(*this, "useTimerViaButton");
    guard.markTransitioned();
#endif
    const QDateTime now = QDateTime::currentDateTime();
    if (discardCrossMidnightOngoingAndStop(now)) {
        // Ongoing segment crossed midnight — engine is now in None state.
        // Whatever the user asked for, discard it: per the day-boundary
        // policy the timer must remain stopped until the user starts
        // again manually.
#ifndef QT_NO_DEBUG
        checkDurationInvariants();
#endif
        return;
    }
    switch (button) {
        case Button::Start: startTimer(now); break;
        case Button::Pause: pauseTimer(now); break;
        case Button::Stop:  stopTimer(now, StopReason::ButtonStop); break;
    }
#ifndef QT_NO_DEBUG
    checkDurationInvariants();
#endif
}

/**
 * Handles desktop lock/unlock events from LockStateWatcher.
 *
 * Event handling:
 * - Lock: Saves final checkpoint, suspends further checkpoints via is_locked_
 * - LongOngoingLock: Triggers backpause if timer was in Activity mode
 * - Unlock: Resumes checkpoints, restarts timer if it was auto-paused
 *
 * Note: Lock/Unlock events for checkpoint control are always processed.
 * Autopause behavior (LongOngoingLock handling) respects isAutopauseEnabled().
 */
void Timer::useTimerViaLockEvent(LockEvent event)
{
    QMutexLocker locker(&mutex_);
#ifndef QT_NO_DEBUG
    StateGuard guard(*this, "useTimerViaLockEvent");
    guard.markTransitioned();
#endif
    const QDateTime now = QDateTime::currentDateTime();
    if (discardCrossMidnightOngoingAndStop(now)) {
        // Already-forced stopped — ignore the lock event for engine
        // purposes. was_active_before_autopause_ is already cleared inside
        // the helper so a subsequent Unlock does not restart.
#ifndef QT_NO_DEBUG
        checkDurationInvariants();
#endif
        return;
    }
    if (event == LockEvent::Lock) {
        is_locked_ = true;
        if (dialog_open_) {
            Logger::Log("[LOCK] suspended during history edit (Lock)");
            return;
        }
        // Save a checkpoint when lock is detected (only if actively tracking time)
        if (mode_ == Mode::Activity) {
            saveCheckpointInternal(now);
            Logger::Log("[LOCK] Desktop locked - checkpoint saved, further checkpoints suspended");
        } else {
            Logger::Log("[LOCK] Desktop locked - no checkpoint (timer not in Activity mode)");
        }
        return;
    }
    if (event == LockEvent::Unlock) {
        is_locked_ = false;
        if (dialog_open_) {
            // Unlock supersedes any pending LongOngoingLock
            pending_lock_event_ = LockEvent::None;
            Logger::Log("[LOCK] suspended during history edit (Unlock)");
            return;
        }
        Logger::Log("[LOCK] Desktop unlocked - checkpoint saving resumed");
    }
    if (!settings_.isAutopauseEnabled()) {
        Logger::Log("[DEBUG] Autopause disabled; lock event ignored");
        return;
    }
    if (event == LockEvent::LongOngoingLock) {
        if (dialog_open_) {
            pending_lock_event_ = LockEvent::LongOngoingLock;
            Logger::Log("[LOCK] suspended during history edit (LongOngoingLock)");
            return;
        }
        was_active_before_autopause_ = (mode_ == Mode::Activity);
        if (was_active_before_autopause_) {
            backpauseTimer(now);
            emit modeChanged(PauseCause::LockAutopause);
        }
    } else if (event == LockEvent::Unlock) {
        if (was_active_before_autopause_) {
            startTimer(now);
            emit modeChanged(PauseCause::LockResume);
        }
        was_active_before_autopause_ = false;
    }
#ifndef QT_NO_DEBUG
    checkDurationInvariants();
#endif
}

void Timer::onTick(const QDateTime& now)
{
    QMutexLocker locker(&mutex_);
    day_boundary_watcher_.tick(now);
}

qint64 Timer::getActiveTime() const
{
    QMutexLocker locker(&mutex_);
    qint64 sum = 0;
    for (const auto &t : session_.durations)
        if (t.type == DurationType::Activity)
            sum += t.duration;
    if (mode_ == Mode::Activity)
        sum += timer_.elapsed();
    return sum;
}

qint64 Timer::getPauseTime() const
{
    QMutexLocker locker(&mutex_);
    qint64 sum = 0;
    for (const auto &t : session_.durations)
        if (t.type == DurationType::Pause)
            sum += t.duration;
    if (mode_ == Mode::Pause)
        sum += timer_.elapsed();
    return sum;
}

Timeline Timer::snapshot() const
{
    QMutexLocker lock(&mutex_);
    return Timeline(session_.durations, getOngoingDuration());
}

std::deque<TimeDuration> Timer::getCurrentDurations() const
{
    QMutexLocker locker(&mutex_);
    return session_.durations;  // copy under lock
}

void Timer::setDurationType(size_t idx, DurationType type)
{
    QMutexLocker locker(&mutex_);
#ifndef QT_NO_DEBUG
    StateGuard guard(*this, "setDurationType");
    guard.markTransitioned(); // Intentionally mutates durations
#endif
    if (idx < session_.durations.size()) {
        session_.durations[idx].type = type;
        Logger::Log(QString("[TIMER] Duration type changed at index %1 to %2").arg(idx).arg(type == DurationType::Activity ? "Activity" : "Pause"));
    } else {
        Logger::Log(QString("[TIMER] Invalid index %1 for setDurationType (size %2)").arg(idx).arg(session_.durations.size()));
    }
#ifndef QT_NO_DEBUG
    checkDurationInvariants();
#endif
}

/**
 * Atomically replaces the in-memory durations and resets checkpoint tracking.
 *
 * This is the sole external entry point for overwriting session_.durations from outside
 * Timer (e.g., when HistoryDialog saves edits). It couples the two operations
 * that must always happen together:
 *   1. Replace the completed-segment deque.
 *   2. Re-anchor checkpoint tracking so the next checkpoint targets the correct
 *      DB row and wall-clock range for the ongoing segment.
 *
 * Without the coupling, a caller could replace durations but forget to reset
 * checkpoint tracking, causing stale segment IDs or start times to persist — a
 * bug that the compiler cannot catch if the operations are separate public methods.
 *
 * @param newDurations  Completed segments to replace session_.durations with.
 * @param ongoing       The currently running segment (if any). When provided,
 *                      checkpoint tracking (segment ID, start time, DB row) is
 *                      re-anchored to this segment. When std::nullopt, checkpoint
 *                      tracking state is left unchanged (appropriate when the timer
 *                      is stopped and there is no ongoing segment).
 */
void Timer::replaceCurrentDurations(const std::deque<TimeDuration>& newDurations,
                                          const std::optional<TimeDuration>& ongoing)
{
    QMutexLocker locker(&mutex_);
#ifndef QT_NO_DEBUG
    StateGuard guard(*this, "replaceCurrentDurations");
    guard.markTransitioned(); // Intentionally replaces durations
#endif
    session_.durations = newDurations;
    session_.clearUnsaved(settings_);

    if (!ongoing.has_value()) {
#ifndef QT_NO_DEBUG
        checkDurationInvariants();
#endif
        return;
    }

    const TimeDuration& seg = ongoing.value();
    session_.adoptOngoingSegment(seg, settings_);

    if (checkpoint_interval_msec_ == 0 || mode_ != Mode::Activity || seg.type != DurationType::Activity) {
#ifndef QT_NO_DEBUG
        checkDurationInvariants();
#endif
        return;
    }

    if (seg.duration <= 0 || !seg.startTime.isValid() || !seg.endTime.isValid()) {
#ifndef QT_NO_DEBUG
        checkDurationInvariants();
#endif
        return;
    }

    db_.saveCheckpoint(seg.type,
                       seg.duration,
                       seg.startTime,
                       seg.endTime,
                       session_.current_checkpoint_segment_id);
#ifndef QT_NO_DEBUG
    checkDurationInvariants();
#endif
}

void Timer::applyEdits(const Timeline& edited)
{
    replaceCurrentDurations(edited.completed(), edited.ongoing());
}

std::deque<TimeDuration> Timer::getDurationsHistory()
{
    QMutexLocker locker(&mutex_);
    auto loadResult = db_.loadDurations();
    last_history_load_skipped_ = loadResult.skipped;
    last_history_load_repaired_ = loadResult.repaired;
    return loadResult.durations;
}

std::pair<int, int> Timer::getLastHistoryLoadStats() const
{
    QMutexLocker locker(&mutex_);
    return std::make_pair(last_history_load_skipped_, last_history_load_repaired_);
}

std::optional<TimeDuration> Timer::getOngoingDuration() const
{
    QMutexLocker locker(&mutex_);
    if (mode_ == Mode::None) return std::nullopt;
    const QDateTime now = QDateTime::currentDateTime();
    if (!session_.segment_start_time.isValid() || session_.segment_start_time >= now) {
        return std::nullopt;
    }
    if (session_.segment_start_time.date() != now.date()) {
        // Cross-midnight ongoing — about to be force-stopped by the
        // watchdog within ~100 ms. Don't expose this transient state.
        return std::nullopt;
    }
    DurationType type = (mode_ == Mode::Activity) ? DurationType::Activity : DurationType::Pause;
    const QString segmentId = session_.current_checkpoint_segment_id.isEmpty()
        ? TimeDuration::createSegmentId()
        : session_.current_checkpoint_segment_id;
    return TimeDuration::fromTrusted(type, session_.segment_start_time, now, segmentId);
}

qint64 Timer::getStartupRecoveredSeconds() const
{
    QMutexLocker locker(&mutex_);
    return startup_recovered_seconds_;
}

bool Timer::shouldShowStartupRecoveryNotification() const
{
    QMutexLocker locker(&mutex_);
    return startup_recovery_notification_needed_;
}

bool Timer::canMarkCleanShutdown() const
{
    QMutexLocker locker(&mutex_);
    return mode_ == Mode::None && !session_.has_unsaved_data;
}

bool Timer::appendDurationsToDB()
{
    return appendDurationsChunkToDB(session_.durations);
}

bool Timer::appendDurationsChunkToDB(const std::deque<TimeDuration>& durations)
{
    if (durations.empty())
        return true;
    return db_.commitSession(Timeline(durations, std::nullopt));
}

bool Timer::updateDurationsInDB()
{
    if (session_.durations.empty())
        return true;
    return db_.commitSession(Timeline(session_.durations, std::nullopt));
}

bool Timer::replaceAll(const Timeline& history, const Timeline& session)
{
    QMutexLocker locker(&mutex_);
    const bool ok = db_.replaceAll(history, session);
    if (ok) {
        // DB is now authoritative; stale retry cache must not survive past here.
        session_.clearUnsaved(settings_);
    }
    return ok;
}

EntriesForDateResult Timer::hasEntriesForDate(const QDate& date)
{
    return db_.hasEntriesForDate(date);
}

/**
 * Appends a single same-day segment to session_.durations.
 *
 * Cross-midnight inputs are silently discarded (with a [MIDNIGHT] log line).
 * DayBoundaryWatcher's scheduled stop and watchdog prevent cross-midnight
 * inputs from reaching here; this discard is a last-line defence.
 *
 * Zero-duration and negative-duration inputs are also dropped.
 */
void Timer::addDuration(DurationType type,
                              const QDateTime& startTime,
                              const QDateTime& endTime,
                              const QString& segmentId)
{
    auto seg = TimeDuration::create(type, startTime, endTime, segmentId);
    if (!seg.has_value()) {
        Logger::Log(QString("[MIDNIGHT] Discarding cross-midnight segment: %1 → %2")
            .arg(startTime.toString(Qt::ISODateWithMs))
            .arg(endTime.toString(Qt::ISODateWithMs)));
        return;
    }
    const qint64 dur = seg->duration;
    session_.durations.emplace_back(std::move(*seg));
    Logger::Log(QString("[DEBUG] Added duration (%1, %2 ms)")
        .arg(type == DurationType::Activity ? "Activity" : "Pause")
        .arg(dur));
}

bool Timer::isOngoingSegmentCrossMidnight() const
{
    QMutexLocker locker(&mutex_);
    if (mode_ == Mode::None) return false;
    if (!session_.segment_start_time.isValid()) return false;
    return session_.segment_start_time.date() != QDate::currentDate();
}

/**
 * Forces the engine into the stopped state when the ongoing segment has
 * crossed midnight, discarding the in-flight segment.
 *
 * Returns true when it fired (telling the caller to abandon any pending
 * transition). Safe to call multiple times; only the first call does real work.
 *
 * Emits stopped(MidnightWatchdog). The GUI is updated via the stopped() signal
 * connection in MainWin (established in Phase 5).
 */
bool Timer::discardCrossMidnightOngoingAndStop(const QDateTime& now)
{
    if (mode_ == Mode::None) return false;
    if (!session_.segment_start_time.isValid()) return false;
    if (session_.segment_start_time.date() == now.date()) return false;

    Logger::Log(QString("[MIDNIGHT] Cross-midnight ongoing detected "
                        "(segment_start=%1, now=%2) — discarding segment, forcing stop")
        .arg(session_.segment_start_time.toString(Qt::ISODateWithMs))
        .arg(now.toString(Qt::ISODateWithMs)));

    // Best-effort: persist any already-completed (same-day) segments that
    // accumulated before midnight. Only the ONGOING segment is discarded.
    if (!session_.durations.empty()) {
        if (!updateDurationsInDB()) {
            session_.markUnsaved(settings_);
            session_.unsaved_durations = session_.durations;
            Logger::Log("[MIDNIGHT] Could not flush completed segments to DB - retained in unsaved buffer");
        } else {
            session_.resetForNewSession(settings_);
        }
    }

    mode_ = Mode::None;
    session_.clearSegment(settings_);
    checkpointTimer_.stop();
    day_boundary_watcher_.cancel();
    was_active_before_autopause_ = false;
    emit stopped(StopReason::MidnightWatchdog);
    return true;
}

void Timer::saveCheckpoint()
{
    QMutexLocker locker(&mutex_);

    // Don't save checkpoints while desktop is locked
    if (is_locked_) {
        Logger::Log("[CHECKPOINT] Skipped - desktop is locked");
        return;
    }

    // Don't save checkpoints while HistoryDialog is open
    if (dialog_open_) {
        Logger::Log("[CHECKPOINT] Skipped - dialog open");
        return;
    }

    // Capture wall-clock time once for the entire checkpoint operation.
    const QDateTime now = QDateTime::currentDateTime();
    saveCheckpointInternal(now);
}

void Timer::saveCheckpointInternal(const QDateTime& now)
{
    // NOTE: This function assumes the mutex is already held by the caller

    if (checkpoint_interval_msec_ == 0) return;
    if (mode_ != Mode::Activity)        return;
    if (discardCrossMidnightOngoingAndStop(now)) return;

    // Calculate current elapsed time for the ongoing segment
    qint64 elapsed = timer_.elapsed();
    if (elapsed <= 0) {
        return; // No time elapsed yet
    }

    // Always Activity - we return early above if mode_ != Mode::Activity
    DurationType type = DurationType::Activity;

    // Save checkpoint to database using the caller-provided timestamp.
    // Pass session_.segment_start_time and session_.current_checkpoint_segment_id to update the specific row for this segment
    if (session_.current_checkpoint_segment_id.isEmpty()) {
        session_.beginNewSegment(session_.segment_start_time, settings_);
    }
    bool success = db_.saveCheckpoint(type, elapsed, session_.segment_start_time, now, session_.current_checkpoint_segment_id);

    if (success) {
        Logger::Log(QString("[CHECKPOINT] Saved checkpoint - Type: %1, Duration: %2ms, SegmentId: %3")
            .arg(type == DurationType::Activity ? "Activity" : "Pause")
            .arg(elapsed)
            .arg(session_.current_checkpoint_segment_id));
    } else {
        Logger::Log("[CHECKPOINT] Failed to save checkpoint to database");
    }
}

void Timer::pauseCheckpoints()
{
    QMutexLocker locker(&mutex_);
    dialog_open_ = true;
    checkpointTimer_.stop();
    Logger::Log("[CHECKPOINT] Checkpoints paused (dialog open)");
}

void Timer::resumeCheckpoints()
{
    QMutexLocker locker(&mutex_);
    dialog_open_ = false;
    Logger::Log("[CHECKPOINT] Checkpoints resumed (dialog closed)");

    // Replay deferred events that were suspended while dialog was open.
    if (pending_midnight_stop_) {
        pending_midnight_stop_ = false;
        pending_lock_event_ = LockEvent::None;
        Logger::Log("[MIDNIGHT] Replaying deferred midnight stop after dialog close");
        stopTimer(QDateTime::currentDateTime(), StopReason::MidnightScheduled);
    } else if (pending_lock_event_ == LockEvent::LongOngoingLock) {
        pending_lock_event_ = LockEvent::None;
        Logger::Log("[LOCK] Replaying deferred LongOngoingLock after dialog close");
        useTimerViaLockEvent(LockEvent::LongOngoingLock);
    } else {
        pending_lock_event_ = LockEvent::None;
    }
    // Restart checkpoint timer only after deferred events are replayed, so
    // stopTimer/backpauseTimer can set mode_ before we decide whether to arm.
    if (checkpoint_interval_msec_ > 0 && mode_ == Mode::Activity && !is_locked_) {
        checkpointTimer_.start();
    }
}

qint64 Timer::reconcileOrphanCheckpoints(
    const std::deque<OrphanCheckpoint>& orphans,
    const std::optional<QDateTime>& cleanShutdownMarker)
{
    startup_recovery_notification_needed_ = false;

    if (orphans.empty()) {
        return 0;
    }

    const QDateTime now = QDateTime::currentDateTime();
    std::vector<OrphanCheckpoint> toFinalize;
    std::vector<long long> dropIds;

    for (const auto& orphan : orphans) {
        const bool tooShort = orphan.duration < kMinRecoverableOrphanDurationMs;
        const bool stale = orphan.endTime.isValid() && (orphan.endTime.msecsTo(now) > kOrphanStaleAgeMs);

        if (tooShort || stale) {
            dropIds.push_back(orphan.id);
            continue;
        }

        toFinalize.push_back(orphan);
    }

    ReconcileResult reconcile = db_.reconcileUnfinalizedCheckpoints(toFinalize, dropIds);
    if (!reconcile.ok) {
        Logger::Log("[DB] Failed to reconcile orphan checkpoints");
        return 0;
    }

    // The store atomically rejects orphans that overlap an existing finalised row.
    // Only count seconds for rows the store actually finalised.
    const std::vector<long long>& finalizeIds = reconcile.finalized;
    qint64 recoveredSeconds = 0;
    for (const auto& orphan : toFinalize) {
        if (std::find(finalizeIds.begin(), finalizeIds.end(), orphan.id) != finalizeIds.end()) {
            recoveredSeconds += orphan.duration / 1000;
        }
    }

    if (!finalizeIds.empty()) {
        bool showNotification = true;

        if (cleanShutdownMarker.has_value() && cleanShutdownMarker->isValid()) {
            const QDateTime markerUtc = cleanShutdownMarker->toUTC();
            QDateTime oldestFinalizedOrphanEndUtc;
            for (const auto& orphan : orphans) {
                if (std::find(finalizeIds.begin(), finalizeIds.end(), orphan.id) == finalizeIds.end()) {
                    continue;
                }

                const QDateTime orphanEndUtc = orphan.endTime.toUTC();
                if (!oldestFinalizedOrphanEndUtc.isValid() || orphanEndUtc < oldestFinalizedOrphanEndUtc) {
                    oldestFinalizedOrphanEndUtc = orphanEndUtc;
                }
            }

            if (oldestFinalizedOrphanEndUtc.isValid() && markerUtc >= oldestFinalizedOrphanEndUtc) {
                showNotification = false;
            }
        }

        startup_recovery_notification_needed_ = showNotification;
    }

    Logger::Log(QString("[DB] Orphan checkpoint reconciliation finished: finalized=%1, dropped=%2, overlap_rejected=%3, recovered_seconds=%4")
        .arg(finalizeIds.size())
        .arg(dropIds.size())
        .arg(reconcile.dropped.size())
        .arg(recoveredSeconds));

    return recoveredSeconds;
}
