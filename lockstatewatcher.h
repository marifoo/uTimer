#ifndef LOCKSTATEWATCHER_H
#define LOCKSTATEWATCHER_H

#include <QObject>
#include <QElapsedTimer>
#include <deque>
#include <optional>
#include <memory>
#include <QString>
#include "settings.h"
#include "types.h"

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

/// Tri-state result of a lock-state query.
/// Unknown is returned when the query fails; the debounce buffer is not updated.
enum class LockQueryResult { Locked, Unlocked, Unknown };

class LockStateWatcher : public QObject
{
	Q_OBJECT

private:
	const Settings & settings_;
	QElapsedTimer lock_timer_;
	std::deque<LockQueryResult> lock_state_buffer_;
	const std::deque<LockQueryResult> buffer_for_lock;
	const std::deque<LockQueryResult> buffer_for_unlock;

#ifdef Q_OS_LINUX
	enum class LinuxLockMethod {
		SystemdLogind,
		FreedesktopScreenSaver,
		GnomeScreenSaver,
		KdeScreenSaver
	};
	std::optional<LinuxLockMethod> linux_lock_method_; // set once in initializeLinuxLockDetection()
	bool initializeLinuxLockDetection();
	LockQueryResult querySystemdLogind();
	LockQueryResult queryScreenSaverDBus(const QString &service, const QString &path, const QString &interface);
#endif

	LockQueryResult isSessionLocked();
	LockEvent determineLockEvent(LockQueryResult query_result);
	bool shouldEmitLongLock(LockQueryResult latest, qint64 elapsedMs) const;

public:
	explicit LockStateWatcher(const Settings & settings, QObject *parent = nullptr);

#ifndef QT_NO_DEBUG
    // ---- Debug-build test probes ----
    /// Exposes the debounce logic for direct unit testing.
    LockEvent determineLockEvent_dbg(LockQueryResult result) { return determineLockEvent(result); }
    /// Exposes the long-lock maturation check for direct unit testing.
    bool shouldEmitLongLock_dbg(LockQueryResult latest, qint64 elapsedMs) const { return shouldEmitLongLock(latest, elapsedMs); }
#endif

signals:
	void desktopLockEvent(LockEvent event);

public slots:
	void update();
};

#endif // LOCKSTATEWATCHER_H
