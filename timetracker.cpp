#include "timetracker.h"
#include <QtDebug>
#include <QDateTime>
#include "logger.h"
#include "helpers.h"

TimeTracker::TimeTracker(const Settings &settings, QObject *parent) 
    : QObject(parent), settings_(settings), mode_(Mode::None), 
      was_active_before_autopause_(false)
{
	;
}

TimeTracker::~TimeTracker()
{
    stopTimer();
}

void TimeTracker::startTimer()
{
	if (mode_ == Mode::Pause) {
		qint64 t_pause = timer_.restart();
		if (durations_.empty() || durations_.back().type != DurationType::Pause) {
			durations_.emplace_back(TimeDuration(DurationType::Pause, t_pause, QDateTime::currentDateTime()));
		}
		else {
			durations_.back().endTime = QDateTime::currentDateTime();
			durations_.back().duration += t_pause;
		}
		mode_ = Mode::Activity;
		if (settings_.logToFile())
			Logger::Log("[TIMER] > Timer unpaused");
	}
	else if (mode_ == Mode::None) {
		durations_.clear();
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
			durations_.emplace_back(TimeDuration(DurationType::Activity, t_active, activiy_end));

			durations_.emplace_back(TimeDuration(DurationType::Pause, backpause_msec, now));

			mode_ = Mode::Pause;
			if (settings_.logToFile())
				Logger::Log("[TIMER] Timer retroactively paused <");
		}
	}
}

void TimeTracker::stopTimer()
{
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

	if (!db_.saveDurations(durations_, TransactionMode::Append)) {
		if (settings_.logToFile()) {
			Logger::Log("[TIMER] Database not updated");
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

std::deque<TimeDuration> TimeTracker::getDurationsHistory()
{
	return db_.loadDurations();
}

bool TimeTracker::appendDurationsToDB()
{
	return db_.saveDurations(this->durations_, TransactionMode::Append);
}

bool TimeTracker::replaceDurationsInDB(const std::deque<TimeDuration> &durations)
{
	return db_.saveDurations(durations, TransactionMode::Replace);
}


