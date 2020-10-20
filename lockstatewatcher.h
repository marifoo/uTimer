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
	enum class Event {None, LockEvent};

	std::unique_ptr<QElapsedTimer> lock_timer_;
	std::deque<Event> lock_events_;
	const int thres_lock_ = 4; // magic number
	Settings & settings_;

	Event getLockEvent();

public:
	explicit LockStateWatcher(Settings & settings, QWidget *parent = nullptr);

signals:
	void desktopLockEvent(LockEvent event);

public slots:
	void update();
};

#endif // LOCKSTATEWATCHER_H
