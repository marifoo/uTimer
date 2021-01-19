#include "lockstatewatcher.h"
#include <QDebug>
#include <QDateTime>
#include <algorithm>

LockStateWatcher::LockStateWatcher(const Settings &settings, QWidget *parent) : QWidget(parent), settings_(settings)
{
	lock_timer_ = std::make_unique<QElapsedTimer>();
	lock_timer_->invalidate();

	for (int i=0; i<(thres_lock_+1); ++i) {
		lock_events_.push_back(Event::None);
	}
}

LockStateWatcher::Event LockStateWatcher::getLockEvent() const
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
	return Event::LockEvent;
}

void LockStateWatcher::update()
{
	const Event e = getLockEvent();
	if ((e == Event::None) && (lock_events_.back() == Event::LockEvent)) {
		if (lock_events_.front() == Event::None) {
			lock_timer_->start();
		}
		else {
			lock_timer_->invalidate();
			if (settings_.isAutopauseEnabled())
				emit desktopLockEvent(LockEvent::Unlock);
		}
	}	

	if (lock_timer_->isValid() && (lock_timer_->elapsed() >= settings_.getBackpauseMsec())) {
		lock_timer_->invalidate();
		if (settings_.isAutopauseEnabled())
			emit desktopLockEvent(LockEvent::LongLock);
	}

	lock_events_.push_back(e);
	lock_events_.pop_front();
}
