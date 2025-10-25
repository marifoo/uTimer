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
				durations_.emplace_back(TimeDuration(DurationType::Pause, t_pause, now));
			}
			else {
				// Extend existing pause entry if it's already the last entry
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
		durations_.clear();
		
		// Add boot time as Activity duration if configured and no entries exist for today
		// This helps track computer usage time before the application was started
		unsigned int boot_time_sec = settings_.getBootTimeSec();
		if (boot_time_sec > 0 && !hasEntriesForToday()) {
			QDateTime now = QDateTime::currentDateTime();
			qint64 boot_time_msec = static_cast<qint64>(boot_time_sec) * 1000;
			durations_.emplace_back(TimeDuration(DurationType::Activity, boot_time_msec, now));
			if (settings_.logToFile())
				Logger::Log("[TIMER] Added boot time: " + QString::number(boot_time_sec) + " seconds (first session today)");
		}
		else if (boot_time_sec > 0 && settings_.logToFile()) {
			Logger::Log("[TIMER] Boot time not added - entries already exist for today");
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
		durations_.emplace_back(TimeDuration(DurationType::Activity, t_active, QDateTime::currentDateTime()));
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
					durations_.emplace_back(TimeDuration(DurationType::Activity, t_active, activity_end));
				}
				else {
					// Extend existing activity entry if it's already the last entry
					durations_.back().endTime = activity_end;
					durations_.back().duration += t_active;
				}
			}

			// Add the retroactive pause period
			durations_.emplace_back(TimeDuration(DurationType::Pause, backpause_msec, now));

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
		durations_.emplace_back(TimeDuration(DurationType::Pause, t_pause, QDateTime::currentDateTime()));
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
		durations_.emplace_back(TimeDuration(DurationType::Activity, t_active, QDateTime::currentDateTime()));
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
	// cleanDurations(&durations_);  // Remove duplicates and merge adjacent entries
	return db_.saveDurations(durations_, TransactionMode::Append);
}

bool TimeTracker::replaceDurationsInDB(std::deque<TimeDuration> durations)
{
	// Replace all database durations with provided data (used by history editor)
	cleanDurations(&durations);  // Remove duplicates and merge adjacent entries
	return db_.saveDurations(durations, TransactionMode::Replace);
}

bool TimeTracker::hasEntriesForToday()
{
	// Check if there are already time entries for today's date
	return db_.hasEntriesForDate(QDate::currentDate());
}

static void cleanDurations(std::deque<TimeDuration>* pDurations)
{
	// Clean up duration entries by removing near-duplicates and merging adjacent entries of the same type
	// This prevents database bloat from frequent timer operations
	auto& durations = *pDurations;
	if (durations.size() < 2) {
		return;
	}
	
	for (auto it = durations.begin() + 1; it != durations.end(); ) {
		auto prevIt = std::prev(it);

		// Merge consecutive durations of the same type that are close in time
		if (prevIt->type == it->type) {
			const qint64 prev_start = prevIt->endTime.toMSecsSinceEpoch() - prevIt->duration;
			const qint64 it_start = it->endTime.toMSecsSinceEpoch() - it->duration;
			const qint64 prev_end = prevIt->endTime.toMSecsSinceEpoch();
			const qint64 it_end = it->endTime.toMSecsSinceEpoch();

			const qint64 diff_end = prev_end - it_end;
			const qint64 diff_dur = prevIt->duration - it->duration;
			const qint64 gap = it_start - prev_end;

			// Remove near-duplicate entries (within 50ms difference)
			if (std::abs(diff_end) < 50 && std::abs(diff_dur) < 50) {
				it = durations.erase(it);
				continue;
			}
			
			// Current entry starts before previous (shorter) entry -> delete
			if (it_start < prev_start && prev_end <= it_end) {
				prevIt->endTime = it->endTime;
				prevIt->duration = it->duration;
				it = durations.erase(it);
				continue;
			}

			// Current entry starts before previous (longer) entry -> join
			if (it_start < prev_start && it_end < prev_end && it_start < prev_end) {
				prevIt->duration = prev_end - it_start;
				it = durations.erase(it);
				continue;
			}

			// Current entry start overlaps into previous entry -> join
			if (prev_start <= it_start && it_start <= prev_end && prev_end <= it_end) {
				prevIt->endTime = it->endTime;
				prevIt->duration += it_end - prev_start;
				it = durations.erase(it);
				continue;
			}

			// Current entry start is subset of previous entry -> delete
			if (prev_start <= it_start && it_start <= prev_end && it_end <= prev_end) {
				it = durations.erase(it);
				continue;
			}

			// Merge adjacent (but disjoint) entries of same type with small gaps (less than 500ms)
			if (gap > 0 && gap < 500) {
				prevIt->duration += it->duration + gap;
				prevIt->endTime = it->endTime;
				it = durations.erase(it);
				continue;
			}
		}
		++it;
	}
}
