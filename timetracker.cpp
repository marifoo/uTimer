#include "timetracker.h"
#include <QtDebug>
#include <QDateTime>
#include "logger.h"
#include "helpers.h"

TimeTracker::TimeTracker(const Settings &settings, QObject *parent) 
    : QObject(parent), settings_(settings), mode_(Mode::None), 
	was_active_before_autopause_(false), db_(settings, parent), timer_()
{ }

TimeTracker::~TimeTracker()
{
    // Ensure timer is properly stopped and data is saved before destruction
    stopTimer();
}

void TimeTracker::startTimer()
{
	// Resume from pause - capture pause duration and continue activity
	if (mode_ == Mode::Pause) {
		if (settings_.logToFile())
			Logger::Log("[DEBUG] Starting Timer from Pause - D=" + QString::number(durations_.size()));
		qint64 t_pause = timer_.restart();
		QDateTime now = QDateTime::currentDateTime();
		if (t_pause > 0) {
			// If no previous pause entry or last entry is not a pause, create new pause entry
			if (durations_.empty() || durations_.back().type != DurationType::Pause) {
				addDurationWithMidnightSplit(DurationType::Pause, t_pause, now);
			}
			else {
				// Extend existing pause entry if it's already the last entry
				// Note: This path doesn't check for midnight crossing in the extension,
				// which is acceptable since the original entry was already split if needed
				durations_.back().endTime = now;
				durations_.back().duration += t_pause;
			}
		}		
		mode_ = Mode::Activity;
		if (settings_.logToFile())
			Logger::Log("[TIMER] > Timer unpaused");
	}
	// Start fresh timer session
	else if (mode_ == Mode::None) {
		if (settings_.logToFile())
			Logger::Log("[DEBUG] Starting Timer from Stopped - D=" + QString::number(durations_.size()));
		
		// Add boot time as Activity duration if configured and no entries exist for today
		// This helps track computer usage time before the application was started
		unsigned int boot_time_sec = settings_.getBootTimeSec();
		bool shouldAddBootTime = false;
		
		if (boot_time_sec > 0) {
			QDate today = QDate::currentDate();
			bool hasEntriesInMemory = false;
			
			// Check if current in-memory durations contain entries for today
			// This must be done BEFORE clearing durations_
			for (const auto& d : durations_) {
				if (d.endTime.date() == today) {
					hasEntriesInMemory = true;
					break;
				}
			}
			
			// Only add boot time if neither memory nor database has entries for today
			bool hasEntriesInDB = hasEntriesForToday();
			shouldAddBootTime = !hasEntriesInMemory && !hasEntriesInDB;
			
			if (settings_.logToFile()) {
				if (shouldAddBootTime) {
					Logger::Log("[TIMER] Will add boot time: " + QString::number(boot_time_sec) + " seconds (first session today)");
				} else {
					Logger::Log("[TIMER] Boot time not added - entries already exist for today (in memory: " + 
						QString(hasEntriesInMemory ? "yes" : "no") + ", in DB: " + 
						QString(hasEntriesInDB ? "yes" : "no") + ")");
				}
			}
		}
		
		durations_.clear();
		
		// Add boot time if determined necessary
		if (shouldAddBootTime) {
			QDateTime now = QDateTime::currentDateTime();
			qint64 boot_time_msec = static_cast<qint64>(boot_time_sec) * 1000;
			durations_.emplace_back(TimeDuration(DurationType::Activity, boot_time_msec, now));
		}
		
		timer_.start();
		mode_ = Mode::Activity;
		if (settings_.logToFile())
			Logger::Log("[TIMER] >> Timer started");
	}
	else {
		if (settings_.logToFile())
			Logger::Log("[DEBUG] Trying to Start Timer from Mode " + QString((mode_ == Mode::Activity) ? "Activity" : "Unknown") + " - D = " + QString::number(durations_.size()));
	}
}

void TimeTracker::pauseTimer()
{
	// Pause active timer - capture activity duration
	if (mode_ == Mode::Activity) {
		if (settings_.logToFile())
			Logger::Log("[DEBUG] Pausing Timer from Activity - D=" + QString::number(durations_.size()));
		qint64 t_active = timer_.restart();
		QDateTime now = QDateTime::currentDateTime();
		addDurationWithMidnightSplit(DurationType::Activity, t_active, now);
		mode_ = Mode::Pause;
		if (settings_.logToFile())
			Logger::Log("[TIMER] Timer paused <");
	}
	else {
		if (settings_.logToFile())
			Logger::Log("[DEBUG] Trying to Pause Timer from Mode " + (mode_ == Mode::None) ? "Stopped" : ((mode_ == Mode::Pause) ?  "Paused" : "Unknown"));
	}
}

void TimeTracker::backpauseTimer()
{
	// Retroactively pause timer - used for autopause when user becomes inactive
	if (mode_ == Mode::Activity) {
		if (settings_.isAutopauseEnabled()) {
			if (settings_.logToFile())
				Logger::Log("[DEBUG] Backpausing Timer from Activity - D=" + QString::number(durations_.size()));
			qint64 backpause_msec = settings_.getBackpauseMsec();
			QDateTime now = QDateTime::currentDateTime();

			qint64 elapsed_time = timer_.restart();
			qint64 t_active = elapsed_time - backpause_msec;
			
			// Ensure we don't create negative activity time
			// This can happen if the backpause time is longer than the elapsed time
			if (t_active < 0) {
				if (settings_.logToFile()) {
					Logger::Log("[DEBUG] Error: Activity time is smaller than Backpüause Time");
				}
				// If backpause time is longer than elapsed time, treat the entire period as pause
				backpause_msec = elapsed_time;
				t_active = 0;
			}
			
			QDateTime activity_end = now.addMSecs(-backpause_msec);
			
			// Only add activity duration if there was actual activity time
			if (t_active > 0) {
				if (settings_.logToFile()) {
					Logger::Log("[DEBUG] Backpausing: Activity time is greater than Zero");
				}

				if (durations_.empty() || durations_.back().type != DurationType::Activity) {
					addDurationWithMidnightSplit(DurationType::Activity, t_active, activity_end);
				}
				else {
					// Extend existing activity entry if it's already the last entry
					durations_.back().endTime = activity_end;
					durations_.back().duration += t_active;
				}
			}

			// Add the retroactive pause period
			addDurationWithMidnightSplit(DurationType::Pause, backpause_msec, now);

			mode_ = Mode::Pause;
			if (settings_.logToFile())
				Logger::Log("[TIMER] Timer retroactively paused <");
		}
		else {
			if (settings_.logToFile()) {
				Logger::Log("[DEBUG] Autopause is disabled, backpause ignored - D=" + QString::number(durations_.size()));
			}
		}
	}
	else {
		if (settings_.logToFile()) {
			Logger::Log("[DEBUG] Trying to Backpause Timer from Mode " + (mode_ == Mode::None) ? "Stopped" : ((mode_ == Mode::Pause) ? "Paused" : "Unknown"));
		}
	}
}

void TimeTracker::stopTimer()
{
	// Early exit if timer is not running
	if (mode_ == Mode::None) {
		if (settings_.logToFile()) {
			Logger::Log("[DEBUG] Stopping Timer from Stopped is Ignored - D=" + QString::number(durations_.size()));
		}
		return;
	}

	// Stop from pause mode - capture final pause duration
	if (mode_ == Mode::Pause) {
		if (settings_.logToFile()) {
			Logger::Log("[DEBUG] Stopping Timer from Paused - D=" + QString::number(durations_.size()));
		}
		qint64 t_pause = timer_.elapsed();
		QDateTime now = QDateTime::currentDateTime();
		addDurationWithMidnightSplit(DurationType::Pause, t_pause, now);
		mode_ = Mode::None;
		if (settings_.logToFile()) {
			Logger::Log("[TIMER] Timer unpaused < and stopped <<");
			Logger::Log("[TIMER] Total Activity Time was " + convMSecToTimeStr(getActiveTime()) + ", Total Pause Time was " + convMSecToTimeStr(getPauseTime()));
		}
	}
	// Stop from activity mode - capture final activity duration
	else if (mode_ == Mode::Activity) {
		if (settings_.logToFile()) {
			Logger::Log("[DEBUG] Stopping Timer from Activity - D=" + QString::number(durations_.size()));
		}
		qint64 t_active = timer_.elapsed();
		QDateTime now = QDateTime::currentDateTime();
		addDurationWithMidnightSplit(DurationType::Activity, t_active, now);
		mode_ = Mode::None;
		if (settings_.logToFile()) {
			Logger::Log("[TIMER] Timer stopped <<");
			Logger::Log("[TIMER] Total Activity Time was " + convMSecToTimeStr(getActiveTime()) + ", Total Pause Time was " + convMSecToTimeStr(getPauseTime()));
		}
	}

	// Save all durations to database and clear current session
	if (appendDurationsToDB()) {
		durations_.clear();
		if (settings_.logToFile()) {
			Logger::Log("[DB] Database updated");
		}
	}
	else {
		if (settings_.logToFile()) {
			Logger::Log("[DB] Database Update Error");
		}
	}
}

void TimeTracker::useTimerViaButton(Button button) {
	// Handle timer control via GUI buttons
	if (button == Button::Start)
		startTimer();
	else if (button == Button::Pause)
		pauseTimer();
	else if (button == Button::Stop)
		stopTimer();
}

void TimeTracker::useTimerViaLockEvent(LockEvent event) {
	// Handle automatic timer control based on computer lock/unlock events
	if (settings_.isAutopauseEnabled()) {
		if (event == LockEvent::LongOngoingLock) {
			// User has been away for extended period - retroactively pause
			if (mode_ == Mode::Activity) {
				if (settings_.logToFile()) {
					Logger::Log("[DEBUG] Long ongoing lock in Activity detected, backpausing timer");
				}
				was_active_before_autopause_ = true;
				backpauseTimer();
			}
			else {
				if (settings_.logToFile()) {
					Logger::Log("[DEBUG] Long ongoing lock outside of Activity detected, doing nothing");
				}
				was_active_before_autopause_ = false;
			}
		}
		else if (event == LockEvent::Unlock) {
			// User returned - resume if they were active before autopause
			if (was_active_before_autopause_) {
				if (settings_.logToFile()) {
					Logger::Log("[DEBUG] Unlocking and was in Activity before, restarting Timer");
				}
				startTimer();
			}	
			else {
				if (settings_.logToFile()) {
					Logger::Log("[DEBUG] Unlocking but was not in Activity before, doing nothing");
				}
			}
			was_active_before_autopause_ = false;
		}
	}
	else {
		if (settings_.logToFile()) {
			Logger::Log("[DEBUG] Autopause is disabled, lock event ignored");
		}
	}
}

qint64 TimeTracker::getActiveTime() const
{
	// Calculate total activity time from recorded durations
	qint64 sum = 0;
	for (const auto& t : durations_) {
		if (t.type == DurationType::Activity)
			sum += t.duration;
	}
		
	// Add current activity time if timer is running in activity mode
	if (mode_ == Mode::Activity)
		sum += timer_.elapsed();
	return sum;
}

qint64 TimeTracker::getPauseTime() const
{
	// Calculate total pause time from recorded durations
	qint64 sum = 0;
	for (const auto& t : durations_) {
		if (t.type == DurationType::Pause)
		sum += t.duration;
	}

	// Add current pause time if timer is running in pause mode
	if (mode_ == Mode::Pause)
		sum += timer_.elapsed();
	return sum;
}

const std::deque<TimeDuration>& TimeTracker::getCurrentDurations() const 
{
	return durations_;
}

void TimeTracker::setDurationType(size_t idx, DurationType type)
{
	// Change the type of a specific duration entry (Activity/Pause)
	if (idx < durations_.size()) {
		durations_[idx].type = type;
		if (settings_.logToFile()) {
			Logger::Log(QString("[TIMER] Duration type changed at index %1 to %2")
				.arg(idx).arg(type == DurationType::Activity ? "Activity" : "Pause"));
		}
	}
	else if (settings_.logToFile()) {
		Logger::Log(QString("[TIMER] Warning: Attempted to set duration type at invalid index %1 (size: %2)")
			.arg(idx).arg(durations_.size()));
	}
}

void TimeTracker::setCurrentDurations(const std::deque<TimeDuration>& newDurations)
{
    // Replace current session durations with new data (used by history editor)
    durations_.clear();
    durations_ = newDurations;
}

std::deque<TimeDuration> TimeTracker::getDurationsHistory()
{
	// Load historical duration data from database
	return db_.loadDurations();
}

bool TimeTracker::appendDurationsToDB()
{ 
	// Save current session durations to database (add to existing data)
	size_t original_size = durations_.size();
	cleanDurations(&durations_);  // Remove duplicates and merge adjacent entries
	if (settings_.logToFile() && (original_size != durations_.size())) {
		Logger::Log(QString("[DB] Cleaned durations in DB: original size %1, cleaned size %2 (A)")
			.arg(original_size).arg(durations_.size()));
	}
	return db_.saveDurations(durations_, TransactionMode::Append);
}

bool TimeTracker::replaceDurationsInDB(std::deque<TimeDuration> durations)
{
	// Replace all database durations with provided data (used by history editor)
	size_t original_size = durations.size();
	cleanDurations(&durations);  // Remove duplicates and merge adjacent entries
	if (settings_.logToFile() && (original_size != durations.size())) {
		Logger::Log(QString("[DB] Cleaned durations in DB: original size %1, cleaned size %2 (R)")
			.arg(original_size).arg(durations.size()));
	}
	return db_.saveDurations(durations, TransactionMode::Replace);
}

bool TimeTracker::hasEntriesForToday()
{
	// Check if there are already time entries for today's date
	return db_.hasEntriesForDate(QDate::currentDate());
}

void TimeTracker::addDurationWithMidnightSplit(DurationType type, qint64 duration, const QDateTime& endTime)
{
	// Ignore non-positive durations to avoid zero-length entries
	if (duration <= 0) {
		if (settings_.logToFile()) {
			Logger::Log("[DEBUG] Ignoring non-positive duration in addDurationWithMidnightSplit");
		}
		return;
	}

	// Calculate start time of this duration
	QDateTime startTime = endTime.addMSecs(-duration);
	
	// Check if we crossed midnight
	if (startTime.date() == endTime.date()) {
		// No midnight crossing - add duration normally
		durations_.emplace_back(TimeDuration(type, duration, endTime));
		
		if (settings_.logToFile()) {
			Logger::Log(QString("{DEBUG] No crossing detected - single duration added (type: %1, duration: %2ms)")
				.arg(type == DurationType::Activity ? "Activity" : "Pause")
				.arg(duration));
		}
	}
	else {
		// Midnight was crossed - need to split the duration
		if (settings_.logToFile()) {
			Logger::Log(QString("{DEBUG] Crossing detected! Start: %1, End: %2")
				.arg(startTime.toString("yyyy-MM-dd HH:mm:ss"))
				.arg(endTime.toString("yyyy-MM-dd HH:mm:ss")));
		}
		
		// Calculate end of the start date (23:59:59.999)
		QDateTime endOfStartDay(startTime.date(), QTime(23, 59, 59, 999));
		
		// Calculate duration until midnight
		qint64 durationBeforeMidnight = startTime.msecsTo(endOfStartDay) + 1; // +1 to include the last millisecond
		
		// Calculate duration after midnight
		qint64 durationAfterMidnight = duration - durationBeforeMidnight;
		
		// Validate split durations
		if (durationBeforeMidnight < 0 || durationAfterMidnight < 0) {
			if (settings_.logToFile()) {
				Logger::Log(QString("{DEBUG] Error: Invalid split calculation - before: %1ms, after: %2ms")
					.arg(durationBeforeMidnight).arg(durationAfterMidnight));
			}
			// Fallback: add as single entry
			durations_.emplace_back(TimeDuration(type, duration, endTime));
			return;
		}
		
		bool addedDay1 = false;
		// Add entry ending at 23:59:59.999 of the previous day if positive
		if (durationBeforeMidnight > 0) {
			durations_.emplace_back(TimeDuration(type, durationBeforeMidnight, endOfStartDay));
			addedDay1 = true;
			if (settings_.logToFile()) {
				Logger::Log(QString("{DEBUG] Created Day 1 entry: %1ms ending at %2")
					.arg(durationBeforeMidnight)
					.arg(endOfStartDay.toString("yyyy-MM-dd HH:mm:ss.zzz")));
			}
		}
		
		// Save the previous day's data to database immediately only if Day 1 was added
		if (addedDay1) {
			if (appendDurationsToDB()) {
				durations_.clear();
				if (settings_.logToFile()) {
					Logger::Log("{DEBUG] Day 1 data saved to DB and cleared from memory");
				}
			}
			else {
				if (settings_.logToFile()) {
					Logger::Log("{DEBUG] Error: Failed to save Day 1 data to DB");
				}
			}
		}
		
		// Calculate start of the new day (00:00:00.000)
		QDateTime startOfNewDay(endTime.date(), QTime(0, 0, 0, 0));
		
		// Add entry for the new day starting at 00:00:00.000 if positive
		if (durationAfterMidnight > 0) {
			durations_.emplace_back(TimeDuration(type, durationAfterMidnight, endTime));
			if (settings_.logToFile()) {
				Logger::Log(QString("{DEBUG] Created Day 2 entry: %1ms starting at %2, ending at %3")
					.arg(durationAfterMidnight)
					.arg(startOfNewDay.toString("yyyy-MM-dd HH:mm:ss.zzz"))
					.arg(endTime.toString("yyyy-MM-dd HH:mm:ss.zzz")));
				Logger::Log("{DEBUG] Split completed successfully");
			}
		} else {
			if (settings_.logToFile()) {
				Logger::Log("{DEBUG] Day 2 entry has non-positive duration, not adding");
			}
		}
	}
}

