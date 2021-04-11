#ifndef LOCKSTATEWATCHER_H
#define LOCKSTATEWATCHER_H

#include <QWidget>
#include <Windows.h>
#include <QElapsedTimer>
#include <deque>
#include <memory>
#include <QString>
#include "settings.h"
#include "types.h"


class LockStateWatcher : public QWidget
{
	Q_OBJECT
private:
	enum class Event {None, LockOrUnlock};

	const Settings & settings_;
	QElapsedTimer lock_timer_;
	std::deque<Event> lock_events_;
	const decltype(lock_events_)::size_type buffer_size_with_100ms_timing_ = 9;
	const decltype(lock_events_)::difference_type buffer_threshold = 5;

	Event getEvent() const;
	LockEvent determineLockEvent(const Event &e);

public:
	explicit LockStateWatcher(const Settings & settings, QWidget *parent = nullptr);

signals:
	void desktopLockEvent(LockEvent event);

public slots:
	void update();
};

#endif // LOCKSTATEWATCHER_H
