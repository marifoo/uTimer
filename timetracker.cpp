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
    stopTimer();
}

void TimeTracker::startTimer()
{
	if (mode_ == Mode::Pause) {
		qint64 t_pause = timer_.restart();
		QDateTime now = QDateTime::currentDateTime();
		if (t_pause > 0) {
			if (durations_.empty() || durations_.back().type != DurationType::Pause) {
				durations_.emplace_back(TimeDuration(DurationType::Pause, t_pause, now));
			}
			else {
				durations_.back().endTime = now;
				durations_.back().duration += t_pause;
			}
		}		
		mode_ = Mode::Activity;
		if (settings_.logToFile())
			Logger::Log("[TIMER] > Timer unpaused");
	}
	else if (mode_ == Mode::None) {
		durations_.clear();
		
		// Add boot time as Activity duration if configured and no entries exist for today
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
}

void TimeTracker::pauseTimer()
{
	if (mode_ == Mode::Activity) {
		qint64 t_active = timer_.restart();
		durations_.emplace_back(TimeDuration(DurationType::Activity, t_active, QDateTime::currentDateTime()));
		mode_ = Mode::Pause;
		if (settings_.logToFile())
			Logger::Log("[TIMER] Timer paused <");
	}
}

void TimeTracker::backpauseTimer()
{
	if (mode_ == Mode::Activity) {
		if (settings_.isAutopauseEnabled()) {
			qint64 backpause_msec = settings_.getBackpauseMsec();
			QDateTime now = QDateTime::currentDateTime();

			qint64 t_active = timer_.restart() - backpause_msec;
			QDateTime activiy_end = now.addMSecs(-backpause_msec);
			if (t_active > 0) {
				if (durations_.empty() || durations_.back().type != DurationType::Activity) {
					durations_.emplace_back(TimeDuration(DurationType::Activity, t_active, activiy_end));
				}
				else {
					durations_.back().endTime = activiy_end;
					durations_.back().duration += t_active;
				}
			}

			durations_.emplace_back(TimeDuration(DurationType::Pause, backpause_msec, now));

			mode_ = Mode::Pause;
			if (settings_.logToFile())
				Logger::Log("[TIMER] Timer retroactively paused <");
		}
	}
}

void TimeTracker::stopTimer()
{
	if (mode_ == Mode::None) {
		return;
	}

	if (mode_ == Mode::Pause) {
		qint64 t_pause = timer_.elapsed();
		durations_.emplace_back(TimeDuration(DurationType::Pause, t_pause, QDateTime::currentDateTime()));
		mode_ = Mode::None;
		if (settings_.logToFile()) {
			Logger::Log("[TIMER] Timer unpaused < and stopped <<");
			Logger::Log("[TIMER] Total Activity Time was " + convMSecToTimeStr(getActiveTime()) + ", Total Pause Time was " + convMSecToTimeStr(getPauseTime()));
		}
	}
	else if (mode_ == Mode::Activity) {
		qint64 t_active = timer_.elapsed();
		durations_.emplace_back(TimeDuration(DurationType::Activity, t_active, QDateTime::currentDateTime()));
		mode_ = Mode::None;
		if (settings_.logToFile()) {
			Logger::Log("[TIMER] Timer stopped <<");
			Logger::Log("[TIMER] Total Activity Time was " + convMSecToTimeStr(getActiveTime()) + ", Total Pause Time was " + convMSecToTimeStr(getPauseTime()));
		}
	}

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
	if (button == Button::Start)
		startTimer();
	else if (button == Button::Pause)
		pauseTimer();
	else if (button == Button::Stop)
		stopTimer();
}

void TimeTracker::useTimerViaLockEvent(LockEvent event) {
	if (settings_.isAutopauseEnabled()) {
		if (event == LockEvent::LongOngoingLock) {
				if (mode_ == Mode::Activity) {
					was_active_before_autopause_ = true;
					backpauseTimer();
				}
				else {
					was_active_before_autopause_ = false;
				}
			}
		else if (event == LockEvent::Unlock) {
			if (was_active_before_autopause_)
				startTimer();
			was_active_before_autopause_ = false;
		}
	}
}

qint64 TimeTracker::getActiveTime() const
{
	qint64 sum = 0;
	for (const auto& t : durations_) {
		if (t.type == DurationType::Activity)
			sum += t.duration;
	}
		
	if (mode_ == Mode::Activity)
		sum += timer_.elapsed();
	return sum;
}

qint64 TimeTracker::getPauseTime() const
{
	qint64 sum = 0;
	for (const auto& t : durations_) {
		if (t.type == DurationType::Pause)
			sum += t.duration;
	}

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
	if (idx < durations_.size()) {
		durations_[idx].type = type;
	}
}

void TimeTracker::setCurrentDurations(const std::vector<TimeDuration>& newDurations)
{
    durations_.clear();
    durations_.insert(durations_.end(), newDurations.begin(), newDurations.end());
}

std::deque<TimeDuration> TimeTracker::getDurationsHistory()
{
	return db_.loadDurations();
}

bool TimeTracker::appendDurationsToDB()
{ 
	cleanDurations(&durations_);
	return db_.saveDurations(durations_, TransactionMode::Append);
}

bool TimeTracker::replaceDurationsInDB(std::deque<TimeDuration> durations)
{
	cleanDurations(&durations);
	return db_.saveDurations(durations, TransactionMode::Replace);
}

bool TimeTracker::hasEntriesForToday()
{
	return db_.hasEntriesForDate(QDate::currentDate());
}

static void cleanDurations(std::deque<TimeDuration>* pDurations)
{
	/*auto& durations = *pDurations;
	if (durations.size() < 2) {
		return;
	}
	for (auto it = durations.begin() + 1; it != durations.end(); ) {
		auto prevIt = std::prev(it);

		if (prevIt->type == it->type) {
			qint64 endTimeDiff = std::abs(prevIt->endTime.toMSecsSinceEpoch() - it->endTime.toMSecsSinceEpoch());
			qint64 DurDiff = std::abs(prevIt->duration - it->duration);
			qint64 startEndDiff = std::abs(prevIt->endTime.toMSecsSinceEpoch() - (it->endTime.toMSecsSinceEpoch() - it->duration));

			if (endTimeDiff < 50 && DurDiff < 50) {
				it = durations.erase(it);
				continue;
			}
			else if (startEndDiff < 500) {
				prevIt->duration += it->duration;
				it = durations.erase(it);
				continue;
			}
		}
		++it;
	}*/
}
