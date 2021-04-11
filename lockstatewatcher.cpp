#include "lockstatewatcher.h"
#include <QDebug>
#include <QDateTime>
#include <algorithm>
#include "logger.h"

LockStateWatcher::LockStateWatcher(const Settings &settings, QWidget *parent) : QWidget(parent), settings_(settings)
{
	lock_timer_.invalidate();

	lock_events_.assign(buffer_size_with_100ms_timing_, Event::None);
}

LockStateWatcher::Event LockStateWatcher::getEvent() const
{
	HDESK desktop = OpenDesktop(TEXT("Default"), 0, false, DESKTOP_SWITCHDESKTOP);
	if (desktop) {
		if (SwitchDesktop(desktop))	{
			CloseDesktop(desktop);
			return Event::None;
		}
		else {
			CloseDesktop(desktop);
			if (settings_.logToFile())
				Logger::Log("LockOrUnlock_1 detected");
			return Event::LockOrUnlock;
		}
	}
	else {
		if (settings_.logToFile())
			Logger::Log("LockOrUnlock_2 detected");
		return Event::LockOrUnlock;
	}
}

LockEvent LockStateWatcher::determineLockEvent(const Event &e)
{
	const auto lock_events_in_buffer = std::count(lock_events_.begin(), lock_events_.end(), Event::LockOrUnlock);
	LockEvent ret;

	if ((e == Event::None) && (lock_events_.back() == Event::LockOrUnlock) && (lock_events_.front() == Event::None) && (lock_events_in_buffer < buffer_threshold))
		ret = LockEvent::Lock;
	else if ((e == Event::None) && (lock_events_.back() == Event::LockOrUnlock) && (lock_events_.front() == Event::LockOrUnlock) && (lock_events_in_buffer > buffer_threshold))
		ret = LockEvent::Unlock;
	else if (lock_timer_.isValid() && (lock_timer_.elapsed() >= settings_.getBackpauseMsec()))
		ret = LockEvent::LongOngoingLock;
	else
		ret = LockEvent::None;

	if ((settings_.logToFile()) && (ret != LockEvent::None))
		Logger::Log(QString(": LockEvent ") + (((int)ret==1) ? "Unlock" : (((int)ret==2) ? "Lock" : "LongLock")) + " determined");

	return ret;
}

void LockStateWatcher::update()
{
	const Event e = getEvent();
	const LockEvent le = determineLockEvent(e);

	if (le == LockEvent::Lock) {
		lock_timer_.start();
	}
	else if ((le == LockEvent::Unlock) || (le == LockEvent::LongOngoingLock)) {
		lock_timer_.invalidate();
		if (settings_.isAutopauseEnabled())
			emit desktopLockEvent(le);
	}

	lock_events_.push_back(e);
	lock_events_.pop_front();
}
