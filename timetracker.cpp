/**
 * TimeTracker - Core timing engine managing Activity/Pause/None states.
 *
 * Architecture:
 * - State machine with three modes: Activity (timing work), Pause (timing break), None (stopped)
 * - Completed segments stored in durations_ deque; ongoing segment tracked by timer_.elapsed()
 * - Checkpoints save ongoing Activity segments to DB every 5 minutes for crash recovery
 * - All public methods are thread-safe via QRecursiveMutex
 *
 * Checkpoint behavior:
 * - Only saved during Activity mode (not Pause or Stopped)
 * - Suspended while PC is locked (is_locked_) or HistoryDialog is open (checkpoints_paused_)
 * - Uses current_checkpoint_id_ to update same DB row instead of creating new rows
 *
 * Backpause behavior:
 * - When PC locked for longer than threshold, retroactively converts last N minutes to Pause
 * - Triggered by LockEvent::LongOngoingLock from LockStateWatcher
 */

#include "timetracker.h"
#include <QtDebug>
#include <QDateTime>
#include <QMutexLocker>
#include <new>  // for std::bad_alloc
#include "logger.h"
#include "helpers.h"

TimeTracker::TimeTracker(const Settings &settings, QObject *parent)
    : QObject(parent), settings_(settings), timer_(), mode_(Mode::None),
      was_active_before_autopause_(false), has_unsaved_data_(false), is_locked_(false),
      checkpoints_paused_(false), db_(settings, parent), current_checkpoint_id_(-1)
{
    // Setup checkpoint timer to fire every 5 minutes
    checkpointTimer_.setInterval(5 * 60 * 1000); // 5 minutes in milliseconds
    checkpointTimer_.setSingleShot(false); // Repeat every 5 minutes
    connect(&checkpointTimer_, &QTimer::timeout, this, &TimeTracker::saveCheckpoint);
}

TimeTracker::~TimeTracker()
{
    stopTimer();
}

/**
 * Transitions timer to Activity mode. Behavior depends on current state:
 * - From Pause: Records pause duration, switches to Activity
 * - From None: Clears state, optionally adds boot time, starts fresh
 * - From Activity: No-op (logs debug message)
 */
void TimeTracker::startTimer()
{
    if (mode_ == Mode::Pause) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Starting Timer from Pause - D=" + QString::number(durations_.size()));
        }
        // Capture timestamp before restart to minimize precision loss
        QDateTime now = QDateTime::currentDateTime();
        qint64 t_pause = timer_.restart();
        if (t_pause > 0) {
            if (durations_.empty() || durations_.back().type != DurationType::Pause) {
                addDurationWithMidnightSplit(DurationType::Pause, t_pause, now);
            } else {
                durations_.back().endTime = now;
                durations_.back().duration += t_pause;
            }
        }
        mode_ = Mode::Activity;
        current_checkpoint_id_ = -1; // Reset checkpoint ID for new segment
        checkpointTimer_.start(); // Resume periodic checkpoint saving
        if (settings_.logToFile()) {
            Logger::Log("[TIMER] > Timer unpaused");
        }
        return;
    }
    if (mode_ == Mode::None) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Starting Timer from Stopped - D=" + QString::number(durations_.size()));
        }

        // Try to save any unsaved data from previous failed save attempt
        if (has_unsaved_data_ && !durations_.empty()) {
            if (settings_.logToFile()) {
                Logger::Log("[DB] Retrying save of previously unsaved durations");
            }
            if (appendDurationsToDB()) {
                durations_.clear();
                has_unsaved_data_ = false;
                if (settings_.logToFile()) {
                    Logger::Log("[DB] Previously unsaved durations saved successfully");
                }
            } else if (settings_.logToFile()) {
                Logger::Log("[DB] CRITICAL: Retry save failed - data will be lost");
                // Continue anyway to avoid blocking the user indefinitely
            }
        }

        unsigned int boot_time_sec = settings_.getBootTimeSec();
        bool shouldAddBootTime = false;
        if (boot_time_sec > 0) {
            QDate today = QDate::currentDate();
            bool hasEntriesInMemory = false;
            for (const auto &d : durations_) {
                if (d.endTime.date() == today) {
                    hasEntriesInMemory = true;
                    break;
                }
            }
            bool hasEntriesInDB = hasEntriesForToday();
            shouldAddBootTime = !hasEntriesInMemory && !hasEntriesInDB;
            if (settings_.logToFile()) {
                Logger::Log(shouldAddBootTime
                            ? "[TIMER] Will add boot time: " + QString::number(boot_time_sec) + " seconds (first session today)"
                            : "[TIMER] Boot time not added - entries already exist for today");
            }
        }
        durations_.clear();
        has_unsaved_data_ = false;
        if (shouldAddBootTime) {
            QDateTime now = QDateTime::currentDateTime();
            try {
                durations_.emplace_back(TimeDuration(DurationType::Activity, static_cast<qint64>(boot_time_sec) * 1000, now));
            } catch (const std::bad_alloc&) {
                if (settings_.logToFile()) {
                    Logger::Log("[CRITICAL] Memory allocation failed adding boot time - continuing without boot time");
                }
                // Continue without boot time rather than failing to start
            }
        }
        timer_.start();
        mode_ = Mode::Activity;
        current_checkpoint_id_ = -1; // Reset checkpoint ID for new segment
        checkpointTimer_.start(); // Start periodic checkpoint saving
        if (settings_.logToFile()) {
            Logger::Log("[TIMER] >> Timer started");
        }
        return;
    }
    if (settings_.logToFile()) {
        Logger::Log("[DEBUG] Trying to Start Timer from Mode Activity - D=" + QString::number(durations_.size()));
    }
}

void TimeTracker::pauseTimer()
{
    if (mode_ != Mode::Activity) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Pause ignored; not in Activity");
        }
        return;
    }
    if (settings_.logToFile()) {
        Logger::Log("[DEBUG] Pausing Timer from Activity - D=" + QString::number(durations_.size()));
    }
    // Capture timestamp before restart to minimize precision loss
    QDateTime now = QDateTime::currentDateTime();
    qint64 t_active = timer_.restart();
    addDurationWithMidnightSplit(DurationType::Activity, t_active, now);
    mode_ = Mode::Pause;

    // Sync checkpoint to DB before resetting ID (ensures Activity duration is finalized)
    updateDurationsInDB();

    current_checkpoint_id_ = -1; // Reset checkpoint ID for new segment (pause)
    checkpointTimer_.start(); // Continue checkpoint saving during pause
    if (settings_.logToFile()) {
        Logger::Log("[TIMER] Timer paused <");
    }
}

/**
 * Retroactively converts the last N minutes of Activity to Pause.
 *
 * Called when PC has been locked longer than the backpause threshold.
 * Splits current elapsed time into two segments:
 *   1. Activity segment: (elapsed - threshold) milliseconds, ending at (now - threshold)
 *   2. Pause segment: threshold milliseconds, ending at now
 *
 * This corrects any checkpoint data that was saved during the lock period,
 * since checkpoints are suspended while locked but the time before lock detection
 * may have been saved as Activity.
 */
void TimeTracker::backpauseTimer()
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
        Logger::Log("[DEBUG] Backpausing Timer from Activity - D=" + QString::number(durations_.size()));
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

    // Capture timestamp before restart to minimize precision loss
    QDateTime now = QDateTime::currentDateTime();
    qint64 elapsed = timer_.restart();
    qint64 t_active = elapsed - backpause_msec;
    if (t_active < 0) {
        backpause_msec = elapsed;
        t_active = 0;
    }
    QDateTime activity_end = now.addMSecs(-backpause_msec);
    if (t_active > 0) {
        addDurationWithMidnightSplit(DurationType::Activity, t_active, activity_end);
    }
    addDurationWithMidnightSplit(DurationType::Pause, backpause_msec, now);
    mode_ = Mode::Pause;
    current_checkpoint_id_ = -1; // Reset checkpoint ID for new segment (pause)

    // Immediately sync to DB to correct the Activity checkpoint we just truncated
    updateDurationsInDB(); 
    
    checkpointTimer_.start(); // Continue checkpoint saving during pause
    if (settings_.logToFile()) {
        Logger::Log("[TIMER] Timer retroactively paused <");
    }
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
void TimeTracker::stopTimer()
{
    if (mode_ == Mode::None) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Stop ignored; already stopped");
        }
        return;
    }
    QDateTime now = QDateTime::currentDateTime();
    if (mode_ == Mode::Pause) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Stopping from Pause - D=" + QString::number(durations_.size()));
        }
        qint64 t_pause = timer_.elapsed();
        addDurationWithMidnightSplit(DurationType::Pause, t_pause, now);
    } else if (mode_ == Mode::Activity) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Stopping from Activity - D=" + QString::number(durations_.size()));
        }
        qint64 t_active = timer_.elapsed();
        addDurationWithMidnightSplit(DurationType::Activity, t_active, now);
    }
    mode_ = Mode::None;
    current_checkpoint_id_ = -1; // Reset ID
    checkpointTimer_.stop(); // Stop periodic checkpoint saving when timer is stopped
    if (settings_.logToFile()) {
        Logger::Log("[TIMER] Timer stopped <<");
    }

    // Persist current session durations only (not entire history). History dialog does replace explicitly.
    // Use update interface to check for existing entries by start time and update them instead of creating duplicates
    if (updateDurationsInDB()) {
        durations_.clear();
        has_unsaved_data_ = false;
        if (settings_.logToFile()) {
            Logger::Log("[DB] Session durations updated");
        }
    } else {
        has_unsaved_data_ = true;  // Mark for retry on next start
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error updating session durations - data retained for next save attempt");
        }
    }
}

void TimeTracker::useTimerViaButton(Button button)
{
    QMutexLocker locker(&mutex_);
    switch (button) {
        case Button::Start: startTimer(); break;
        case Button::Pause: pauseTimer(); break;
        case Button::Stop:  stopTimer();  break;
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
    if (event == LockEvent::Lock) {
        is_locked_ = true;
        // Save a checkpoint when lock is detected (only if actively tracking time)
        if (mode_ == Mode::Activity) {
            saveCheckpointInternal();
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
            backpauseTimer();
        }
    } else if (event == LockEvent::Unlock) {
        if (was_active_before_autopause_) {
            startTimer();
        }
        was_active_before_autopause_ = false;
    }
}

qint64 TimeTracker::getActiveTime() const
{
    QMutexLocker locker(&mutex_);
    qint64 sum = 0;
    for (const auto &t : durations_)
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
    for (const auto &t : durations_)
        if (t.type == DurationType::Pause)
            sum += t.duration;
    if (mode_ == Mode::Pause)
        sum += timer_.elapsed();
    return sum;
}

const std::deque<TimeDuration>& TimeTracker::getCurrentDurations() const
{
    QMutexLocker locker(&mutex_);
    return durations_;
}

void TimeTracker::setDurationType(size_t idx, DurationType type)
{
    QMutexLocker locker(&mutex_);
    if (idx < durations_.size()) {
        durations_[idx].type = type;
        if (settings_.logToFile()) {
            Logger::Log(QString("[TIMER] Duration type changed at index %1 to %2").arg(idx).arg(type == DurationType::Activity ? "Activity" : "Pause"));
        }
    } else if (settings_.logToFile()) {
        Logger::Log(QString("[TIMER] Invalid index %1 for setDurationType (size %2)").arg(idx).arg(durations_.size()));
    }
}

void TimeTracker::setCurrentDurations(const std::deque<TimeDuration>& newDurations)
{
    QMutexLocker locker(&mutex_);
    durations_ = newDurations;
}

std::deque<TimeDuration> TimeTracker::getDurationsHistory()
{
    QMutexLocker locker(&mutex_);
    return db_.loadDurations();
}

bool TimeTracker::appendDurationsToDB()
{
    if (durations_.empty())
        return true;
    auto temp = durations_;
    size_t original = temp.size();
    cleanDurations(&temp);
    if (settings_.logToFile() && original != temp.size()) {
        Logger::Log(QString("[DB] Cleaned session durations: %1 -> %2").arg(original).arg(temp.size()));
    }
    
    return db_.saveDurations(temp, TransactionMode::Append);
}

bool TimeTracker::updateDurationsInDB()
{
    if (durations_.empty())
        return true;
    auto temp = durations_;
    size_t original = temp.size();
    cleanDurations(&temp);
    if (settings_.logToFile() && original != temp.size()) {
        Logger::Log(QString("[DB] Cleaned session durations for update: %1 -> %2").arg(original).arg(temp.size()));
    }
    
    // Use the separate update interface that checks for existing entries by start time
    return db_.updateDurationsByStartTime(temp);
}

bool TimeTracker::replaceDurationsInDB(std::deque<TimeDuration> durations)
{
    size_t original = durations.size();
    cleanDurations(&durations);
    if (settings_.logToFile() && original != durations.size()) {
        Logger::Log(QString("[DB] Cleaned durations for replace: %1 -> %2").arg(original).arg(durations.size()));
    }
    return db_.saveDurations(durations, TransactionMode::Replace);
}

bool TimeTracker::hasEntriesForToday()
{
    return db_.hasEntriesForDate(QDate::currentDate());
}

/**
 * Adds a duration segment to the in-memory deque, handling midnight boundaries.
 *
 * If a duration spans midnight, it is split into two segments:
 * - One ending at 23:59:59.999 (saved to DB immediately as previous day)
 * - One starting at 00:00:00.000 (kept in memory for current day)
 *
 * This ensures each day's data is self-contained and can be saved independently.
 * Memory allocation failures trigger emergency saves to prevent data loss.
 */
void TimeTracker::addDurationWithMidnightSplit(DurationType type, qint64 duration, const QDateTime& endTime)
{
    if (duration <= 0) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Ignoring non-positive duration");
        }
        return;
    }

    QDateTime startTime = endTime.addMSecs(-duration);

    // Check if midnight was crossed (this should rarely happen due to auto-stop/restart)
    if (startTime.date() != endTime.date()) {
        if (settings_.logToFile()) {
            Logger::Log(QString("[WARNING] Unexpected midnight crossing detected - duration spans %1 to %2")
                .arg(startTime.toString(Qt::ISODate))
                .arg(endTime.toString(Qt::ISODate)));
        }

        // Fallback: Split at midnight boundary
        QDateTime endOfStartDay(startTime.date(), QTime(23, 59, 59, 999));
        QDateTime startOfNewDay(endTime.date(), QTime(0, 0, 0, 0));

        qint64 beforeMidnight = startTime.msecsTo(endOfStartDay) + 1;
        qint64 afterMidnight = startOfNewDay.msecsTo(endTime);

        if (beforeMidnight > 0) {
            try {
                durations_.emplace_back(TimeDuration(type, beforeMidnight, endOfStartDay));
            } catch (const std::bad_alloc&) {
                if (settings_.logToFile()) {
                    Logger::Log("[CRITICAL] Memory allocation failed in addDurationWithMidnightSplit (before midnight) - attempting emergency save");
                }
                appendDurationsToDB();  // Emergency save attempt
                has_unsaved_data_ = true;
                return;
            }
            if (settings_.logToFile()) {
                Logger::Log(QString("[DEBUG] Added duration before midnight (%1, %2ms)")
                    .arg(type == DurationType::Activity ? "Activity" : "Pause")
                    .arg(beforeMidnight));
            }
        }

        // Save the previous day's data
        if (appendDurationsToDB()) {
            durations_.clear();
            has_unsaved_data_ = false;
            if (settings_.logToFile()) {
                Logger::Log("[DB] Previous day saved to DB (fallback midnight handling)");
            }
        } else {
            has_unsaved_data_ = true;
            if (settings_.logToFile()) {
                Logger::Log("[DB] CRITICAL: Failed to save previous day during midnight crossing - data retained in memory for next save attempt");
            }
            // Data remains in durations_ and will be saved on next stopTimer() call
        }

        if (afterMidnight > 0) {
            try {
                durations_.emplace_back(TimeDuration(type, afterMidnight, endTime));
            } catch (const std::bad_alloc&) {
                if (settings_.logToFile()) {
                    Logger::Log("[CRITICAL] Memory allocation failed in addDurationWithMidnightSplit (after midnight) - data may be incomplete");
                }
                has_unsaved_data_ = true;
                return;
            }
            if (settings_.logToFile()) {
                Logger::Log(QString("[DEBUG] Added duration after midnight (%1, %2ms)")
                    .arg(type == DurationType::Activity ? "Activity" : "Pause")
                    .arg(afterMidnight));
            }
        }
        return;
    }

    // Normal case: duration does not cross midnight
    try {
        durations_.emplace_back(TimeDuration(type, duration, endTime));
    } catch (const std::bad_alloc&) {
        if (settings_.logToFile()) {
            Logger::Log("[CRITICAL] Memory allocation failed in addDurationWithMidnightSplit - attempting emergency save");
        }
        appendDurationsToDB();  // Emergency save attempt
        has_unsaved_data_ = true;
        return;
    }
    if (settings_.logToFile()) {
        Logger::Log(QString("[DEBUG] Added duration (%1, %2ms)")
            .arg(type == DurationType::Activity ? "Activity" : "Pause")
            .arg(duration));
    }
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

    saveCheckpointInternal();
}

void TimeTracker::saveCheckpointInternal()
{
    // NOTE: This function assumes the mutex is already held by the caller

    // Only save checkpoint if timer is in Activity mode (not during Pause or Stopped)
    if (mode_ != Mode::Activity) {
        return;
    }

    // Calculate current elapsed time for the ongoing segment
    qint64 elapsed = timer_.elapsed();
    if (elapsed <= 0) {
        return; // No time elapsed yet
    }

    // Determine duration type based on current mode
    DurationType type = (mode_ == Mode::Activity) ? DurationType::Activity : DurationType::Pause;

    // Get current end time
    QDateTime now = QDateTime::currentDateTime();

    // Save checkpoint to database
    // Pass current_checkpoint_id_ to allow updating the specific row for this segment
    bool success = db_.saveCheckpoint(type, elapsed, now, current_checkpoint_id_);

    if (success) {
        Logger::Log(QString("[CHECKPOINT] Saved checkpoint - Type: %1, Duration: %2ms, ID: %3")
            .arg(type == DurationType::Activity ? "Activity" : "Pause")
            .arg(elapsed)
            .arg(current_checkpoint_id_));
    } else if (settings_.logToFile()) {
        Logger::Log("[CHECKPOINT] Failed to save checkpoint to database");
    }
}

void TimeTracker::pauseCheckpoints()
{
    QMutexLocker locker(&mutex_);
    checkpoints_paused_ = true;
    if (settings_.logToFile()) {
        Logger::Log("[CHECKPOINT] Checkpoints paused");
    }
}

void TimeTracker::resumeCheckpoints()
{
    QMutexLocker locker(&mutex_);
    checkpoints_paused_ = false;
    if (settings_.logToFile()) {
        Logger::Log("[CHECKPOINT] Checkpoints resumed");
    }
}
