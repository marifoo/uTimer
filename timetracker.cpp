/**
 * TimeTracker - Core timing engine managing Activity/Pause/None states.
 *
 * Architecture:
 * - State machine with three modes: Activity (timing work), Pause (timing break), None (stopped)
 * - Completed segments stored in session_.durations deque; ongoing segment tracked by timer_.elapsed()
 * - Checkpoints save ongoing Activity segments to DB every 5 minutes for crash recovery
 * - All public methods are thread-safe via QRecursiveMutex
 * - Per-session mutable state is grouped in SessionState (session_) with explicit
 *   transition methods that log old→new values, replacing the old pattern of
 *   scattered raw-field mutations
 *
 * Checkpoint behavior:
 * - Only saved during Activity mode (not Pause or Stopped)
 * - Suspended while PC is locked (is_locked_) or HistoryDialog is open (checkpoints_paused_)
 * - Uses session_.current_checkpoint_segment_id to update same DB row instead of creating new rows
 *
 * Backpause behavior:
 * - When PC locked for longer than threshold, retroactively converts last N minutes to Pause
 * - Triggered by LockEvent::LongOngoingLock from LockStateWatcher
 */

#include "timetracker.h"
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
// SessionState transition methods
// ============================================================================

void SessionState::beginNewSegment(const QDateTime& startTime, const Settings& settings)
{
    QString oldId = current_checkpoint_segment_id;
    QDateTime oldStart = segment_start_time;
    current_checkpoint_segment_id = TimeDuration::createSegmentId();
    segment_start_time = startTime;
    if (settings.logToFile()) {
        Logger::Log(QString("[STATE] beginNewSegment: segId '%1' -> '%2', start %3 -> %4")
            .arg(oldId, current_checkpoint_segment_id,
                 oldStart.toString(Qt::ISODate), startTime.toString(Qt::ISODate)));
    }
}

void SessionState::clearSegment(const Settings& settings)
{
    QString oldId = current_checkpoint_segment_id;
    QDateTime oldStart = segment_start_time;
    current_checkpoint_segment_id.clear();
    segment_start_time = QDateTime();
    if (settings.logToFile()) {
        Logger::Log(QString("[STATE] clearSegment: segId '%1' -> (empty), start %2 -> (invalid)")
            .arg(oldId, oldStart.toString(Qt::ISODate)));
    }
}

void SessionState::updateSegmentStartTime(const QDateTime& newStart, const Settings& settings)
{
    QDateTime oldStart = segment_start_time;
    segment_start_time = newStart;
    if (settings.logToFile()) {
        Logger::Log(QString("[STATE] updateSegmentStartTime: %1 -> %2")
            .arg(oldStart.toString(Qt::ISODate), newStart.toString(Qt::ISODate)));
    }
}

void SessionState::markUnsaved(const Settings& settings)
{
    if (!has_unsaved_data) {
        has_unsaved_data = true;
        if (settings.logToFile()) {
            Logger::Log("[STATE] markUnsaved: false -> true");
        }
    }
}

void SessionState::clearUnsaved(const Settings& settings)
{
    bool old = has_unsaved_data;
    has_unsaved_data = false;
    unsaved_durations.clear();
    if (old && settings.logToFile()) {
        Logger::Log("[STATE] clearUnsaved: true -> false");
    }
}

void SessionState::resetForNewSession(const Settings& settings)
{
    size_t oldSize = durations.size();
    bool oldUnsaved = has_unsaved_data;
    durations.clear();
    has_unsaved_data = false;
    unsaved_durations.clear();
    if (settings.logToFile()) {
        Logger::Log(QString("[STATE] resetForNewSession: durations %1 -> 0, unsaved %2 -> false")
            .arg(oldSize).arg(oldUnsaved ? "true" : "false"));
    }
}

void SessionState::adoptOngoingSegment(const TimeDuration& ongoing, const Settings& settings)
{
    QString oldId = current_checkpoint_segment_id;
    QDateTime oldStart = segment_start_time;
    current_checkpoint_segment_id = ongoing.segment_id.isEmpty()
        ? TimeDuration::createSegmentId()
        : ongoing.segment_id;
    segment_start_time = ongoing.startTime;
    if (settings.logToFile()) {
        Logger::Log(QString("[STATE] adoptOngoingSegment: segId '%1' -> '%2', start %3 -> %4")
            .arg(oldId, current_checkpoint_segment_id,
                 oldStart.toString(Qt::ISODate), ongoing.startTime.toString(Qt::ISODate)));
    }
}

// ============================================================================
// Debug-build state invariant checking
// ============================================================================

#ifndef QT_NO_DEBUG

TimeTracker::StateSnapshot TimeTracker::takeStateSnapshot() const
{
    return {
        session_.durations.size(),
        session_.current_checkpoint_segment_id,
        session_.segment_start_time,
        session_.has_unsaved_data
    };
}

TimeTracker::StateGuard::StateGuard(const TimeTracker& tracker, const char* methodName)
    : tracker_(tracker), method_(methodName), entry_(tracker.takeStateSnapshot())
{
}

TimeTracker::StateGuard::~StateGuard()
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

void TimeTracker::StateGuard::markTransitioned()
{
    transitioned_ = true;
}

#endif // QT_NO_DEBUG

// ============================================================================
// TimeTracker implementation
// ============================================================================

TimeTracker::TimeTracker(const Settings &settings, QObject *parent)
    : QObject(parent), settings_(settings), timer_(), session_(), mode_(Mode::None),
      was_active_before_autopause_(false), is_locked_(false),
      checkpoints_paused_(false), db_(settings, parent),
      checkpoint_interval_msec_(settings.getCheckpointIntervalMsec()),
      last_history_load_skipped_(0), last_history_load_repaired_(0),
      startup_recovered_seconds_(0), startup_recovery_notification_needed_(false)
{
    // Setup checkpoint timer (disabled if interval is 0)
    if (checkpoint_interval_msec_ > 0) {
        checkpointTimer_.setInterval(checkpoint_interval_msec_);
    }
    checkpointTimer_.setSingleShot(false);
    connect(&checkpointTimer_, &QTimer::timeout, this, &TimeTracker::saveCheckpoint);

    // Log when checkpoints are disabled
    if (checkpoint_interval_msec_ == 0 && settings_.logToFile()) {
        Logger::Log("[CHECKPOINT] Checkpoints disabled (interval = 0)");
    }

    const std::optional<QDateTime> cleanShutdownMarker = db_.consumeLastCleanShutdownMarker();
    startup_recovered_seconds_ = reconcileOrphanCheckpoints(db_.loadUnfinalizedCheckpoints(), cleanShutdownMarker);
}

TimeTracker::~TimeTracker()
{
    stopTimer(QDateTime::currentDateTime());
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
void TimeTracker::startTimer(const QDateTime& now)
{
    if (mode_ == Mode::Pause) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Starting Timer from Pause - D=" + QString::number(session_.durations.size()));
        }
        qint64 t_pause = timer_.restart();
        if (t_pause > 0) {
            if (session_.durations.empty() || session_.durations.back().type != DurationType::Pause) {
                addDurationWithMidnightSplit(DurationType::Pause, session_.segment_start_time, now, session_.current_checkpoint_segment_id);
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
        if (settings_.logToFile()) {
            Logger::Log("[TIMER] > Timer unpaused");
        }
        return;
    }
    if (mode_ == Mode::None) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Starting Timer from Stopped - D=" + QString::number(session_.durations.size()));
        }

        bool retry_succeeded = true;

        // Try to save any unsaved data from previous failed save attempt.
        // Use unsaved_durations if present to keep retries idempotent.
        const std::deque<TimeDuration>& retry_source = session_.unsaved_durations.empty() ? session_.durations : session_.unsaved_durations;
        if (session_.has_unsaved_data && !retry_source.empty()) {
            if (settings_.logToFile()) {
                Logger::Log("[DB] Retrying save of previously unsaved durations");
            }
            if (appendDurationsChunkToDB(retry_source)) {
                session_.clearUnsaved(settings_);
                if (settings_.logToFile()) {
                    Logger::Log("[DB] Previously unsaved durations saved successfully");
                }
            } else {
                retry_succeeded = false;
                if (settings_.logToFile()) {
                    Logger::Log("[DB] CRITICAL: Retry save failed - unsaved data retained for another retry");
                }
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
            if (settings_.logToFile()) {
                Logger::Log(shouldAddBootTime
                            ? "[TIMER] Will add boot time: " + QString::number(boot_time_sec) + " seconds (first session today)"
                            : "[TIMER] Boot time not added - entries already exist for today");
            }
        }
        if (retry_succeeded) {
            session_.resetForNewSession(settings_);
        }
        if (shouldAddBootTime) {
            QDateTime bootStart = now.addMSecs(-static_cast<qint64>(boot_time_sec) * 1000);
            addDurationWithMidnightSplit(DurationType::Activity, bootStart, now);
        }
        timer_.start();
        mode_ = Mode::Activity;
        session_.beginNewSegment(now, settings_);
        if (checkpoint_interval_msec_ > 0) {
            checkpointTimer_.start(); // Start periodic checkpoint saving
        }
        if (settings_.logToFile()) {
            Logger::Log("[TIMER] >> Timer started");
        }
        return;
    }
    if (settings_.logToFile()) {
        Logger::Log("[DEBUG] Trying to Start Timer from Mode Activity - D=" + QString::number(session_.durations.size()));
    }
}

void TimeTracker::pauseTimer(const QDateTime& now)
{
    if (mode_ != Mode::Activity) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Pause ignored; not in Activity");
        }
        return;
    }
    if (settings_.logToFile()) {
        Logger::Log("[DEBUG] Pausing Timer from Activity - D=" + QString::number(session_.durations.size()));
    }
    timer_.restart();
    addDurationWithMidnightSplit(DurationType::Activity, session_.segment_start_time, now, session_.current_checkpoint_segment_id);

    finalizeActivityToPause(now);
    if (settings_.logToFile()) {
        Logger::Log("[TIMER] Timer paused <");
    }
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
void TimeTracker::backpauseTimer(const QDateTime& now)
{
    if (mode_ != Mode::Activity) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Backpause ignored; not in Activity");
        }
        return;
    }
    if (!settings_.isAutopauseEnabled()) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Autopause disabled; backpause ignored");
        }
        return;
    }
    if (settings_.logToFile()) {
        Logger::Log("[DEBUG] Backpausing Timer from Activity - D=" + QString::number(session_.durations.size()));
    }
    qint64 backpause_msec = settings_.getBackpauseMsec();

    // Bounds check: backpause should be between 1 second and 1 hour
    constexpr qint64 MIN_BACKPAUSE_MS = 1000;      // 1 second minimum
    constexpr qint64 MAX_BACKPAUSE_MS = 3600000;   // 1 hour maximum
    if (backpause_msec < MIN_BACKPAUSE_MS) {
        if (settings_.logToFile()) {
            Logger::Log(QString("[WARNING] Backpause value %1ms below minimum, using %2ms")
                .arg(backpause_msec).arg(MIN_BACKPAUSE_MS));
        }
        backpause_msec = MIN_BACKPAUSE_MS;
    } else if (backpause_msec > MAX_BACKPAUSE_MS) {
        if (settings_.logToFile()) {
            Logger::Log(QString("[WARNING] Backpause value %1ms exceeds maximum, using %2ms")
                .arg(backpause_msec).arg(MAX_BACKPAUSE_MS));
        }
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
        addDurationWithMidnightSplit(DurationType::Activity, session_.segment_start_time, activity_end, session_.current_checkpoint_segment_id);
    }

    // Pause segment: activity_end to now
    QDateTime pause_start = activity_end;
    addDurationWithMidnightSplit(DurationType::Pause, pause_start, now);

    finalizeActivityToPause(now);
    if (settings_.logToFile()) {
        Logger::Log("[TIMER] Timer retroactively paused <");
    }
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
void TimeTracker::finalizeActivityToPause(const QDateTime& pauseSegmentStart)
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
void TimeTracker::stopTimer(const QDateTime& now)
{
    if (mode_ == Mode::None) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Stop ignored; already stopped");
        }
        return;
    }
    if (mode_ == Mode::Pause) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Stopping from Pause - D=" + QString::number(session_.durations.size()));
        }
        addDurationWithMidnightSplit(DurationType::Pause, session_.segment_start_time, now, session_.current_checkpoint_segment_id);
    } else if (mode_ == Mode::Activity) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Stopping from Activity - D=" + QString::number(session_.durations.size()));
        }
        addDurationWithMidnightSplit(DurationType::Activity, session_.segment_start_time, now, session_.current_checkpoint_segment_id);
    }
    mode_ = Mode::None;
    session_.clearSegment(settings_);
    checkpointTimer_.stop(); // Stop periodic checkpoint saving when timer is stopped
    if (settings_.logToFile()) {
        Logger::Log("[TIMER] Timer stopped <<");
    }

    // Persist current session durations only (not entire history). History dialog does replace explicitly.
    // Use update interface to check for existing entries by start time and update them instead of creating duplicates
    if (updateDurationsInDB()) {
        session_.resetForNewSession(settings_);
        // Use the same `now` for the shutdown marker to maintain a single
        // consistent timestamp across the entire stop operation.
        db_.setLastCleanShutdownMarker(now);
        if (settings_.logToFile()) {
            Logger::Log("[DB] Session durations updated");
        }
    } else {
        session_.markUnsaved(settings_);
        session_.unsaved_durations = session_.durations;
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error updating session durations - data retained for next save attempt");
        }
    }
}

void TimeTracker::useTimerViaButton(Button button)
{
    QMutexLocker locker(&mutex_);
#ifndef QT_NO_DEBUG
    StateGuard guard(*this, "useTimerViaButton");
    guard.markTransitioned(); // All branches intentionally mutate state
#endif
    // Capture wall-clock time once for the entire operation. All helpers
    // receive this single timestamp, preventing races from preemption or
    // clock skew between successive QDateTime::currentDateTime() calls.
    const QDateTime now = QDateTime::currentDateTime();
    switch (button) {
        case Button::Start: startTimer(now); break;
        case Button::Pause: pauseTimer(now); break;
        case Button::Stop:  stopTimer(now);  break;
    }
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
void TimeTracker::useTimerViaLockEvent(LockEvent event)
{
    QMutexLocker locker(&mutex_);
#ifndef QT_NO_DEBUG
    StateGuard guard(*this, "useTimerViaLockEvent");
    guard.markTransitioned(); // Lock events may trigger backpause/resume
#endif
    // Capture wall-clock time once for the entire operation. All helpers
    // receive this single timestamp, preventing races from preemption or
    // clock skew between successive QDateTime::currentDateTime() calls.
    const QDateTime now = QDateTime::currentDateTime();
    if (event == LockEvent::Lock) {
        is_locked_ = true;
        // Save a checkpoint when lock is detected (only if actively tracking time)
        if (mode_ == Mode::Activity) {
            saveCheckpointInternal(now);
            if (settings_.logToFile()) {
                Logger::Log("[LOCK] Desktop locked - checkpoint saved, further checkpoints suspended");
            }
        } else if (settings_.logToFile()) {
            Logger::Log("[LOCK] Desktop locked - no checkpoint (timer not in Activity mode)");
        }
        return;
    }
    if (event == LockEvent::Unlock) {
        is_locked_ = false;
        if (settings_.logToFile()) {
            Logger::Log("[LOCK] Desktop unlocked - checkpoint saving resumed");
        }
    }
    if (!settings_.isAutopauseEnabled()) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Autopause disabled; lock event ignored");
        }
        return;
    }
    if (event == LockEvent::LongOngoingLock) {
        was_active_before_autopause_ = (mode_ == Mode::Activity);
        if (was_active_before_autopause_) {
            backpauseTimer(now);
        }
    } else if (event == LockEvent::Unlock) {
        if (was_active_before_autopause_) {
            startTimer(now);
        }
        was_active_before_autopause_ = false;
    }
}

qint64 TimeTracker::getActiveTime() const
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

qint64 TimeTracker::getPauseTime() const
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

const std::deque<TimeDuration>& TimeTracker::getCurrentDurations() const
{
    QMutexLocker locker(&mutex_);
    return session_.durations;
}

void TimeTracker::setDurationType(size_t idx, DurationType type)
{
    QMutexLocker locker(&mutex_);
#ifndef QT_NO_DEBUG
    StateGuard guard(*this, "setDurationType");
    guard.markTransitioned(); // Intentionally mutates durations
#endif
    if (idx < session_.durations.size()) {
        session_.durations[idx].type = type;
        if (settings_.logToFile()) {
            Logger::Log(QString("[TIMER] Duration type changed at index %1 to %2").arg(idx).arg(type == DurationType::Activity ? "Activity" : "Pause"));
        }
    } else if (settings_.logToFile()) {
        Logger::Log(QString("[TIMER] Invalid index %1 for setDurationType (size %2)").arg(idx).arg(session_.durations.size()));
    }
}

/**
 * Atomically replaces the in-memory durations and resets checkpoint tracking.
 *
 * This is the sole external entry point for overwriting session_.durations from outside
 * TimeTracker (e.g., when HistoryDialog saves edits). It couples the two operations
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
void TimeTracker::replaceCurrentDurations(const std::deque<TimeDuration>& newDurations,
                                          const std::optional<TimeDuration>& ongoing)
{
    QMutexLocker locker(&mutex_);
#ifndef QT_NO_DEBUG
    StateGuard guard(*this, "replaceCurrentDurations");
    guard.markTransitioned(); // Intentionally replaces durations
#endif
    session_.durations = newDurations;

    if (!ongoing.has_value()) {
        return;
    }

    const TimeDuration& seg = ongoing.value();
    session_.adoptOngoingSegment(seg, settings_);

    if (checkpoint_interval_msec_ == 0 || mode_ != Mode::Activity || seg.type != DurationType::Activity) {
        return;
    }

    if (seg.duration <= 0 || !seg.startTime.isValid() || !seg.endTime.isValid()) {
        return;
    }

    db_.saveCheckpoint(seg.type,
                       seg.duration,
                       seg.startTime,
                       seg.endTime,
                       session_.current_checkpoint_segment_id);
}

std::deque<TimeDuration> TimeTracker::getDurationsHistory()
{
    QMutexLocker locker(&mutex_);
    auto loadResult = db_.loadDurations();
    last_history_load_skipped_ = loadResult.skipped;
    last_history_load_repaired_ = loadResult.repaired;
    return loadResult.durations;
}

std::pair<int, int> TimeTracker::getLastHistoryLoadStats() const
{
    QMutexLocker locker(&mutex_);
    return std::make_pair(last_history_load_skipped_, last_history_load_repaired_);
}

std::optional<TimeDuration> TimeTracker::getOngoingDuration() const
{
    QMutexLocker locker(&mutex_);
    if (mode_ == Mode::None) {
        return std::nullopt;
    }
    QDateTime now = QDateTime::currentDateTime();
    if (!session_.segment_start_time.isValid() || session_.segment_start_time >= now) {
        return std::nullopt;
    }
    DurationType type = (mode_ == Mode::Activity) ? DurationType::Activity : DurationType::Pause;
    const QString segmentId = session_.current_checkpoint_segment_id.isEmpty()
        ? TimeDuration::createSegmentId()
        : session_.current_checkpoint_segment_id;
    return TimeDuration(type, session_.segment_start_time, now, segmentId);
}

qint64 TimeTracker::getStartupRecoveredSeconds() const
{
    QMutexLocker locker(&mutex_);
    return startup_recovered_seconds_;
}

bool TimeTracker::shouldShowStartupRecoveryNotification() const
{
    QMutexLocker locker(&mutex_);
    return startup_recovery_notification_needed_;
}

bool TimeTracker::markCleanShutdown()
{
    return db_.setLastCleanShutdownMarker(QDateTime::currentDateTime());
}

bool TimeTracker::canMarkCleanShutdown() const
{
    QMutexLocker locker(&mutex_);
    return mode_ == Mode::None && !session_.has_unsaved_data;
}

bool TimeTracker::appendDurationsToDB()
{
    return appendDurationsChunkToDB(session_.durations);
}

bool TimeTracker::appendDurationsChunkToDB(const std::deque<TimeDuration>& durations)
{
    if (durations.empty())
        return true;
    auto temp = durations;
    size_t original = temp.size();
    auto removedIds = cleanDurations(&temp);
    if (settings_.logToFile() && original != temp.size()) {
        Logger::Log(QString("[DB] Cleaned session durations: %1 -> %2").arg(original).arg(temp.size()));
    }
    
    return db_.saveDurations(temp, TransactionMode::Append, removedIds);
}

bool TimeTracker::updateDurationsInDB()
{
    if (session_.durations.empty())
        return true;
    auto temp = session_.durations;
    size_t original = temp.size();
    auto removedIds = cleanDurations(&temp);
    if (settings_.logToFile() && original != temp.size()) {
        Logger::Log(QString("[DB] Cleaned session durations for update: %1 -> %2").arg(original).arg(temp.size()));
    }
    
    // Use the separate update interface that matches existing entries by segment_id
    return db_.updateDurationsById(temp, removedIds);
}

bool TimeTracker::replaceDurationsInDB(std::deque<TimeDuration> historyDurations,
                                       std::deque<TimeDuration> currentSessionDurations)
{
    size_t originalHistory = historyDurations.size();
    // Return value ignored: replaceDurationsInDB wipes the entire table,
    // so orphaned segment_ids are implicitly cleaned up by the DELETE FROM.
    cleanDurations(&historyDurations);
    if (settings_.logToFile() && originalHistory != historyDurations.size()) {
        Logger::Log(QString("[DB] Cleaned history durations for replace: %1 -> %2")
            .arg(originalHistory)
            .arg(historyDurations.size()));
    }

    size_t originalCurrent = currentSessionDurations.size();
    // Return value ignored: same reasoning as above.
    cleanDurations(&currentSessionDurations);
    if (settings_.logToFile() && originalCurrent != currentSessionDurations.size()) {
        Logger::Log(QString("[DB] Cleaned current-session durations for replace: %1 -> %2")
            .arg(originalCurrent)
            .arg(currentSessionDurations.size()));
    }

    return db_.replaceDurationsInDB(historyDurations, currentSessionDurations);
}

EntriesForDateResult TimeTracker::hasEntriesForDate(const QDate& date)
{
    return db_.hasEntriesForDate(date);
}

/**
 * Pure computation: splits a duration at midnight boundaries without mutating
 * any TimeTracker state. Returns a MidnightSplitResult that the caller can
 * apply explicitly via applyMidnightSplit().
 *
 * If the duration spans midnight, the result contains:
 * - previous_day_entries: segments ending at 23:59:59.999 (for immediate DB save)
 * - new_entries: segments starting at 00:00:00.000 (for current-day in-memory)
 * - updated_segment_start_time: start of the new day
 *
 * If no midnight crossing, new_entries contains a single segment and
 * previous_day_entries is empty.
 */
MidnightSplitResult TimeTracker::computeMidnightSplit(DurationType type, const QDateTime& startTime, const QDateTime& endTime, const QString& segmentId) const
{
    MidnightSplitResult result;

    qint64 duration = startTime.msecsTo(endTime);
    if (duration <= 0) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Ignoring non-positive duration");
        }
        return result;
    }

    // Check if midnight was crossed (this should rarely happen due to auto-stop/restart)
    if (startTime.date() != endTime.date()) {
        result.crossed_midnight = true;

        if (settings_.logToFile()) {
            Logger::Log(QString("[WARNING] Unexpected midnight crossing detected - duration spans %1 to %2")
                .arg(startTime.toString(Qt::ISODate))
                .arg(endTime.toString(Qt::ISODate)));
        }

        // Fallback: Split at midnight boundary
        QDateTime endOfStartDay(startTime.date(), QTime(23, 59, 59, 999));
        QDateTime startOfNewDay(endTime.date(), QTime(0, 0, 0, 0));

        if (startTime < endOfStartDay) {
            result.previous_day_entries.emplace_back(TimeDuration(type, startTime, endOfStartDay, segmentId));
            if (settings_.logToFile()) {
                Logger::Log(QString("[DEBUG] Split: before-midnight segment (%1, %2ms)")
                    .arg(type == DurationType::Activity ? "Activity" : "Pause")
                    .arg(startTime.msecsTo(endOfStartDay)));
            }
        }

        if (startOfNewDay < endTime) {
            result.new_entries.emplace_back(TimeDuration(type, startOfNewDay, endTime));
            result.updated_segment_start_time = startOfNewDay;
            if (settings_.logToFile()) {
                Logger::Log(QString("[DEBUG] Split: after-midnight segment (%1, %2ms)")
                    .arg(type == DurationType::Activity ? "Activity" : "Pause")
                    .arg(startOfNewDay.msecsTo(endTime)));
            }
        }
        return result;
    }

    // Normal case: duration does not cross midnight
    result.new_entries.emplace_back(TimeDuration(type, startTime, endTime, segmentId));
    if (settings_.logToFile()) {
        Logger::Log(QString("[DEBUG] Added duration (%1, %2ms)")
            .arg(type == DurationType::Activity ? "Activity" : "Pause")
            .arg(duration));
    }
    return result;
}

/**
 * Applies a MidnightSplitResult to the session state.
 *
 * For midnight-crossing splits:
 * 1. Appends previous_day_entries to session_.durations
 * 2. Saves previous day to DB
 * 3. Clears durations for the new day
 * 4. Appends new_entries (after-midnight segments)
 * 5. Updates segment_start_time if needed
 *
 * For normal (no midnight crossing) splits:
 * 1. Simply appends new_entries to session_.durations
 *
 * Memory allocation failures trigger emergency saves to prevent data loss.
 */
void TimeTracker::applyMidnightSplit(const MidnightSplitResult& result)
{
    if (result.crossed_midnight) {
        // Append before-midnight entries
        for (const auto& entry : result.previous_day_entries) {
            try {
                session_.durations.emplace_back(entry);
            } catch (const std::bad_alloc&) {
                if (settings_.logToFile()) {
                    Logger::Log("[CRITICAL] Memory allocation failed in applyMidnightSplit (before midnight) - attempting emergency save");
                }
                appendDurationsToDB();
                session_.markUnsaved(settings_);
                return;
            }
        }

        // Save the previous day's data
        if (appendDurationsToDB()) {
            session_.resetForNewSession(settings_);
            if (settings_.logToFile()) {
                Logger::Log("[DB] Previous day saved to DB (fallback midnight handling)");
            }
        } else {
            session_.markUnsaved(settings_);
            session_.unsaved_durations = session_.durations;
            if (settings_.logToFile()) {
                Logger::Log("[DB] CRITICAL: Failed to save previous day during midnight crossing - data retained in memory for next save attempt");
            }
            // Data remains in session_.durations and will be saved on next stopTimer() call
        }

        // Append after-midnight entries
        for (const auto& entry : result.new_entries) {
            try {
                session_.durations.emplace_back(entry);
            } catch (const std::bad_alloc&) {
                if (settings_.logToFile()) {
                    Logger::Log("[CRITICAL] Memory allocation failed in applyMidnightSplit (after midnight) - data may be incomplete");
                }
                session_.markUnsaved(settings_);
                return;
            }
        }

        // Update segment_start_time to start of new day
        if (result.updated_segment_start_time.has_value()) {
            session_.updateSegmentStartTime(result.updated_segment_start_time.value(), settings_);
        }
    } else {
        // Normal case: no midnight crossing, just append new entries
        for (const auto& entry : result.new_entries) {
            try {
                session_.durations.emplace_back(entry);
            } catch (const std::bad_alloc&) {
                if (settings_.logToFile()) {
                    Logger::Log("[CRITICAL] Memory allocation failed in applyMidnightSplit - attempting emergency save");
                }
                appendDurationsToDB();
                session_.markUnsaved(settings_);
                return;
            }
        }
    }
}

/**
 * Adds a duration segment to the in-memory deque, handling midnight boundaries.
 *
 * This is a thin wrapper that calls the pure computeMidnightSplit() to determine
 * what entries to create, then applies the result via applyMidnightSplit().
 * This separation makes the midnight logic testable independently of state mutation.
 */
void TimeTracker::addDurationWithMidnightSplit(DurationType type, const QDateTime& startTime, const QDateTime& endTime, const QString& segmentId)
{
    MidnightSplitResult result = computeMidnightSplit(type, startTime, endTime, segmentId);
    applyMidnightSplit(result);
}

void TimeTracker::saveCheckpoint()
{
    QMutexLocker locker(&mutex_);

    // Don't save checkpoints while desktop is locked
    if (is_locked_) {
        if (settings_.logToFile()) {
            Logger::Log("[CHECKPOINT] Skipped - desktop is locked");
        }
        return;
    }

    // Don't save checkpoints while paused (e.g., HistoryDialog is open)
    if (checkpoints_paused_) {
        if (settings_.logToFile()) {
            Logger::Log("[CHECKPOINT] Skipped - checkpoints paused");
        }
        return;
    }

    // Capture wall-clock time once for the entire checkpoint operation.
    const QDateTime now = QDateTime::currentDateTime();
    saveCheckpointInternal(now);
}

void TimeTracker::saveCheckpointInternal(const QDateTime& now)
{
    // NOTE: This function assumes the mutex is already held by the caller

    // Don't save checkpoints if feature is disabled
    if (checkpoint_interval_msec_ == 0) {
        return;
    }

    // Only save checkpoint if timer is in Activity mode (not during Pause or Stopped)
    if (mode_ != Mode::Activity) {
        return;
    }

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
        if (settings_.logToFile()) {
            Logger::Log(QString("[CHECKPOINT] Saved checkpoint - Type: %1, Duration: %2ms, SegmentId: %3")
                .arg(type == DurationType::Activity ? "Activity" : "Pause")
                .arg(elapsed)
                .arg(session_.current_checkpoint_segment_id));
        }
    } else if (settings_.logToFile()) {
        Logger::Log("[CHECKPOINT] Failed to save checkpoint to database");
    }
}

void TimeTracker::pauseCheckpoints()
{
    QMutexLocker locker(&mutex_);
    checkpoints_paused_ = true;
    checkpointTimer_.stop();
    if (settings_.logToFile()) {
        Logger::Log("[CHECKPOINT] Checkpoints paused");
    }
}

void TimeTracker::resumeCheckpoints()
{
    QMutexLocker locker(&mutex_);
    checkpoints_paused_ = false;
    if (checkpoint_interval_msec_ > 0 && mode_ == Mode::Activity && !is_locked_) {
        checkpointTimer_.start();
    }
    if (settings_.logToFile()) {
        Logger::Log("[CHECKPOINT] Checkpoints resumed");
    }
}

bool TimeTracker::checkDatabaseSchema()
{
    return db_.checkSchemaOnStartup();
}

void TimeTracker::flushDatabaseToDisc()
{
    db_.flushToDisc();
}

qint64 TimeTracker::reconcileOrphanCheckpoints(
    const std::deque<DatabaseManager::OrphanCheckpoint>& orphans,
    const std::optional<QDateTime>& cleanShutdownMarker)
{
    startup_recovery_notification_needed_ = false;

    if (orphans.empty()) {
        return 0;
    }

    const QDateTime now = QDateTime::currentDateTime();
    std::vector<long long> finalizeIds;
    std::vector<long long> dropIds;
    qint64 recoveredSeconds = 0;

    for (const auto& orphan : orphans) {
        const bool tooShort = orphan.duration < kMinRecoverableOrphanDurationMs;
        const bool stale = orphan.endTime.isValid() && (orphan.endTime.msecsTo(now) > kOrphanStaleAgeMs);

        if (tooShort || stale) {
            dropIds.push_back(orphan.id);
            continue;
        }

        finalizeIds.push_back(orphan.id);
        recoveredSeconds += orphan.duration / 1000;
    }

    if (!db_.reconcileUnfinalizedCheckpoints(finalizeIds, dropIds)) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Failed to reconcile orphan checkpoints");
        }
        return 0;
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

    if (settings_.logToFile()) {
        Logger::Log(QString("[DB] Orphan checkpoint reconciliation finished: finalized=%1, dropped=%2, recovered_seconds=%3")
            .arg(finalizeIds.size())
            .arg(dropIds.size())
            .arg(recoveredSeconds));
    }

    return recoveredSeconds;
}
