#ifndef LOCKSTATEWATCHER_H
#define LOCKSTATEWATCHER_H

#include <QWidget>
#include <QElapsedTimer>
#include <deque>
#include <memory>
#include <QString>
#include "settings.h"
#include "types.h"

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#endif


class LockStateWatcher : public QWidget
{
	Q_OBJECT

private:
	const Settings & settings_;
	QElapsedTimer lock_timer_;
	std::deque<bool> lock_state_buffer_;
	const std::deque<bool> buffer_for_lock;
	const std::deque<bool> buffer_for_unlock;

#ifdef Q_OS_LINUX
	enum class LinuxLockMethod {
		None,
		SystemdLogind,
		FreedesktopScreenSaver,
		GnomeScreenSaver,
		KdeScreenSaver
	};
	LinuxLockMethod linux_lock_method_;
	bool initializeLinuxLockDetection();
	bool querySystemdLogind();
	bool queryFreedesktopScreenSaver();
	bool queryGnomeScreenSaver();
	bool queryKdeScreenSaver();
#endif

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
