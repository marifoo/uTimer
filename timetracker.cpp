#include "timetracker.h"
#include <QtDebug>
#include <QDateTime>
#include <QMutexLocker>
#include <new>  // for std::bad_alloc
#include "logger.h"
#include "helpers.h"

TimeTracker::TimeTracker(const Settings &settings, QObject *parent)
    : QObject(parent), settings_(settings), timer_(), mode_(Mode::None),
      was_active_before_autopause_(false), has_unsaved_data_(false), db_(settings, parent)
{ }

TimeTracker::~TimeTracker()
{
    stopTimer();
}

void TimeTracker::startTimer()
{
    if (mode_ == Mode::Pause) {
        if (settings_.logToFile()) {
            Logger::Log("[DEBUG] Starting Timer from Pause - D=" + QString::number(durations_.size()));
        }
        qint64 t_pause = timer_.restart();
        QDateTime now = QDateTime::currentDateTime();
        if (t_pause > 0) {
            if (durations_.empty() || durations_.back().type != DurationType::Pause) {
                addDurationWithMidnightSplit(DurationType::Pause, t_pause, now);
            } else {
                durations_.back().endTime = now;
                durations_.back().duration += t_pause;
            }
        }
        mode_ = Mode::Activity;
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
    qint64 t_active = timer_.restart();
    QDateTime now = QDateTime::currentDateTime();
    addDurationWithMidnightSplit(DurationType::Activity, t_active, now);
    mode_ = Mode::Pause;
    if (settings_.logToFile()) {
        Logger::Log("[TIMER] Timer paused <");
    }
}

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
    if (settings_.logToFile()) {
        Logger::Log("[TIMER] Timer retroactively paused <");
    }
}

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
    if (settings_.logToFile()) {
        Logger::Log("[TIMER] Timer stopped <<");
    }

    // Persist current session durations only (not entire history). History dialog does replace explicitly.
    if (appendDurationsToDB()) {
        durations_.clear();
        has_unsaved_data_ = false;
        if (settings_.logToFile()) {
            Logger::Log("[DB] Session durations appended");
        }
    } else {
        has_unsaved_data_ = true;  // Mark for retry on next start
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error appending session durations - data retained for next save attempt");
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

void TimeTracker::useTimerViaLockEvent(LockEvent event)
{
    QMutexLocker locker(&mutex_);
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

