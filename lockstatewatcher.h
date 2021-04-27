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
	const Settings & settings_;
	QElapsedTimer lock_timer_;
	std::deque<bool> lock_state_buffer_;
	const std::deque<bool> buffer_for_lock;
	const std::deque<bool> buffer_for_unlock;

	bool isSessionLocked();
	LockEvent determineLockEvent(bool session_locked);

public:
	explicit LockStateWatcher(const Settings & settings, QWidget *parent = nullptr);

signals:
	void desktopLockEvent(LockEvent event);

public slots:
	void update();
};

#endif // LOCKSTATEWATCHER_H
