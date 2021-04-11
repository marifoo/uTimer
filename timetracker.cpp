#include "timetracker.h"
#include <QtDebug>
#include <QDateTime>
#include "logger.h"

TimeTracker::TimeTracker(const Settings &settings, QObject *parent) : QObject(parent), settings_(settings)
{
	mode_ = Mode::None;
}

void TimeTracker::startTimer()
{
	if (mode_ == Mode::Pause) {
		qint64 t_pause = timer_.restart();
		pauses_.push_back(t_pause);
		mode_ = Mode::Activity;
		if (settings_.logToFile())
			Logger::Log("[TIMER] > Timer unpaused");
	}
	else if (mode_ == Mode::None) {
		activities_.clear();
		pauses_.clear();
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
		activities_.push_back(t_active);
		mode_ = Mode::Pause;
		if (settings_.logToFile())
			Logger::Log("[TIMER] Timer paused <");
	}
}

void TimeTracker::backpauseTimer()
{
	if (mode_ == Mode::Activity) {
		qint64 backpause_msec = settings_.getBackpauseMsec();
		pauses_.push_back(backpause_msec);
		activities_.push_back(-backpause_msec);
		pauseTimer();
		if (settings_.logToFile())
			Logger::Log("[TIMER] Timer retroactively added Pause");
	}
}

void TimeTracker::stopTimer()
{
	if (mode_ == Mode::Pause) {
		qint64 t_pause = timer_.elapsed();
		pauses_.push_back(t_pause);
		mode_ = Mode::None;
		if (settings_.logToFile())
			Logger::Log("[TIMER] Timer unpaused < and stopped <<");
	}
	else if (mode_ == Mode::Activity) {
		qint64 t_active = timer_.elapsed();
		activities_.push_back(t_active);
		mode_ = Mode::None;
		if (settings_.logToFile())
			Logger::Log("[TIMER] Timer stopped <<");
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
	if (event == LockEvent::Unlock)
		startTimer();
	else if (event == LockEvent::LongOngoingLock)
		backpauseTimer();
}

void TimeTracker::sendTimes()
{
	emit sendAllTimes(getActiveTime(), getPauseTime());
}

qint64 TimeTracker::getActiveTime() const
{
	qint64 sum = 0;
	for(auto & t : activities_)
		sum += t;
	if (mode_ == Mode::Activity)
		sum += timer_.elapsed();
	return sum;
}

qint64 TimeTracker::getPauseTime() const
{
	qint64 sum = 0;
	for(auto & t : pauses_)
		sum += t;
	if (mode_ == Mode::Pause)
		sum += timer_.elapsed();
	return sum;
}


