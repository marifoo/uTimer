#ifndef LOCKSTATEWATCHER_H
#define LOCKSTATEWATCHER_H

#include <QObject>
#include <QElapsedTimer>
#include <deque>
#include <memory>
#include <QString>
#include "settings.h"
#include "types.h"

#ifdef Q_OS_WIN
#include <Windows.h>
#endif


class LockStateWatcher : public QObject
{
	Q_OBJECT

private:
	const Settings & settings_;
	QElapsedTimer lock_timer_;
	std::deque<bool> lock_state_buffer_;
	const std::deque<bool> buffer_for_lock;
	const std::deque<bool> buffer_for_unlock;

#ifdef Q_OS_LINUX
	bool initializeLinuxLockDetection();
	bool querySystemdLogind();
	bool queryScreenSaverDBus(const QString &service, const QString &path, const QString &interface);
#endif

	bool isSessionLocked();
	LockEvent determineLockEvent(bool session_locked);

public:
	explicit LockStateWatcher(const Settings & settings, QObject *parent = nullptr);

signals:
	void desktopLockEvent(LockEvent event);

public slots:
	void update();
};

#endif // LOCKSTATEWATCHER_H
