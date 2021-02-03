#include "lockstatewatcher.h"
#include <QDebug>
#include <QDateTime>
#include <algorithm>

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
		}
	}
	return Event::LockOrUnlock;
}

LockEvent LockStateWatcher::determineLockEvent(const Event &e)
{
	if ((e == Event::None) && (lock_events_.back() == Event::LockOrUnlock) && (lock_events_.front() == Event::None))
		return LockEvent::Lock;
	else if ((e == Event::None) && (lock_events_.back() == Event::LockOrUnlock) && (lock_events_.front() == Event::LockOrUnlock))
		return LockEvent::Unlock;
	else if (lock_timer_.isValid() && (lock_timer_.elapsed() >= settings_.getBackpauseMsec()))
		return LockEvent::LongOngoingLock;
	else
		return LockEvent::None;
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
