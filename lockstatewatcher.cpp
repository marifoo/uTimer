/**
 * LockStateWatcher - Cross-platform desktop lock detection.
 *
 * Polls the desktop lock state every 100ms (driven by main.cpp timer) and emits
 * lock events when state transitions are detected.
 *
 * Platform implementations:
 * - Windows: WtsApi32.dll WTSQuerySessionInformation for session lock state
 * - Linux: DBus queries with fallback chain (systemd-logind → Freedesktop → GNOME → KDE)
 *
 * Debouncing:
 * - Uses a 5-sample buffer to filter transient state changes
 * - Lock requires pattern: [false, false, true, true, true]
 * - Unlock requires pattern: [true, true, false, false, false]
 *
 * Events emitted:
 * - LockEvent::Lock - Desktop just locked
 * - LockEvent::Unlock - Desktop just unlocked
 * - LockEvent::LongOngoingLock - Lock duration exceeded backpause threshold
 */

#include "lockstatewatcher.h"
#include <QDebug>
#include <QDateTime>
#include <algorithm>
#include "logger.h"

#ifdef Q_OS_WIN
#include <WtsApi32.h>

// RAII wrapper for Windows library handle to prevent leaks
struct ScopedLibrary {
    HMODULE handle;
    explicit ScopedLibrary(LPCWSTR name) : handle(LoadLibraryW(name)) {}
    ~ScopedLibrary() { if (handle) FreeLibrary(handle); }
    explicit operator bool() const { return handle != nullptr; }
    ScopedLibrary(const ScopedLibrary&) = delete;
    ScopedLibrary& operator=(const ScopedLibrary&) = delete;
};
#endif

#ifdef Q_OS_LINUX
#include <QDBusMessage>
#include <QDBusVariant>
#endif

LockStateWatcher::LockStateWatcher(const Settings &settings, QWidget *parent)
	: QWidget(parent),
	settings_(settings),
	buffer_for_lock{ false, false, true, true, true }, // fixed pattern for lock detection
	buffer_for_unlock{ true, true, false, false, false } // fixed pattern for unlock detection
#ifdef Q_OS_LINUX
	, linux_lock_method_(LinuxLockMethod::None)
#endif
{
	lock_state_buffer_ = { false, false, false, false, false};
	lock_timer_.invalidate();

#ifdef Q_OS_LINUX
	initializeLinuxLockDetection();
#endif

	if (settings_.logToFile())
		Logger::Log("[LOCK] LockStateWatcher initialized, BufferSize = " + QString::number(lock_state_buffer_.size()));
}

/**
 * Platform-agnostic check for the current session lock state.
 *
 * Windows Implementation:
 * Uses WTSQuerySessionInformation from WtsApi32.dll.
 * Note: The DLL is loaded dynamically at runtime (using LoadLibrary/GetProcAddress)
 * rather than linking statically. This ensures the application remains portable
 * and runs even if the WTS API behaves differently on some Windows versions,
 * though WtsApi32 is standard since XP.
 *
 * Linux Implementation:
 * Delegates to the specific DBus method identified during initialization
 * (Systemd, GNOME, KDE, etc.).
 */
bool LockStateWatcher::isSessionLocked()
{
#ifdef Q_OS_WIN
	// Using RAII wrapper for automatic library cleanup
	typedef BOOL(PASCAL *WTSQuerySessionInformation)(HANDLE hServer, DWORD SessionId, WTS_INFO_CLASS WTSInfoClass, LPTSTR* ppBuffer, DWORD* pBytesReturned);
	typedef void(PASCAL *WTSFreeMemory)(PVOID pMemory);

	ScopedLibrary hLib(L"wtsapi32.dll");
	if (!hLib) {
		return false;
	}

	auto pWTSQuerySessionInformation = reinterpret_cast<WTSQuerySessionInformation>(
		GetProcAddress(hLib.handle, "WTSQuerySessionInformationW"));
	if (!pWTSQuerySessionInformation) {
		return false;
	}

	auto pWTSFreeMemory = reinterpret_cast<WTSFreeMemory>(
		GetProcAddress(hLib.handle, "WTSFreeMemory"));
	if (!pWTSFreeMemory) {
		return false;  // Don't proceed without cleanup capability
	}

	LPTSTR ppBuffer = nullptr;
	DWORD dwBytesReturned = 0;
	DWORD dwSessionID = WTSGetActiveConsoleSessionId();
	bool isLocked = false;

	if (pWTSQuerySessionInformation(WTS_CURRENT_SERVER_HANDLE, dwSessionID,
	                                WTSSessionInfoEx, &ppBuffer, &dwBytesReturned)) {
		if (dwBytesReturned > 0 && ppBuffer) {
			auto pInfo = reinterpret_cast<WTSINFOEXW*>(ppBuffer);
			if (pInfo->Level == 1) {
				isLocked = (pInfo->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK);
			}
		}
		pWTSFreeMemory(ppBuffer);
	}

	return isLocked;
#elif defined(Q_OS_LINUX)
	switch (linux_lock_method_) {
		case LinuxLockMethod::SystemdLogind:
			return querySystemdLogind();
		case LinuxLockMethod::FreedesktopScreenSaver:
			return queryFreedesktopScreenSaver();
		case LinuxLockMethod::GnomeScreenSaver:
			return queryGnomeScreenSaver();
		case LinuxLockMethod::KdeScreenSaver:
			return queryKdeScreenSaver();
		case LinuxLockMethod::None:
		default:
			return false;
	}
#else
	// Unsupported platform
	return false;
#endif
}

LockEvent LockStateWatcher::determineLockEvent(bool session_locked)
{
	lock_state_buffer_.push_back(session_locked);
	lock_state_buffer_.pop_front();

	if (lock_state_buffer_ == buffer_for_lock)
		return LockEvent::Lock;
	else if (lock_state_buffer_ == buffer_for_unlock)
		return LockEvent::Unlock;
	else
		return LockEvent::None;
}

/**
 * Main polling method, called every 100ms by the application timer.
 *
 * Workflow:
 * 1. Query current lock state via platform-specific isSessionLocked()
 * 2. Feed state into debounce buffer via determineLockEvent()
 * 3. On Lock: Start lock_timer_, emit Lock event
 * 4. On Unlock: Emit Unlock event (only if autopause enabled)
 * 5. While locked: Check if lock_timer_ exceeded threshold → emit LongOngoingLock
 *
 * The lock_timer_ is invalidated after emitting LongOngoingLock to prevent
 * repeated emissions during the same lock session.
 */
void LockStateWatcher::update()
{
	const LockEvent lock_event = determineLockEvent(isSessionLocked());

	if (lock_event == LockEvent::Lock) {
		if (settings_.logToFile())
			Logger::Log("[LOCK] >> Lock determined");
		lock_timer_.start();
		emit desktopLockEvent(LockEvent::Lock);
	}
	else if (lock_event == LockEvent::Unlock) {
		if (settings_.logToFile() && lock_timer_.isValid())
			Logger::Log("[LOCK] Current Lock Duration = " + QString::number(lock_timer_.elapsed()) + "ms");
		lock_timer_.invalidate();
		if (settings_.logToFile())
			Logger::Log("[LOCK] Unlock determined <<");
		emit desktopLockEvent(LockEvent::Unlock);
	}

	if (lock_timer_.isValid() && (lock_timer_.elapsed() >= settings_.getBackpauseMsec())) {
		if (settings_.logToFile() && lock_timer_.isValid())
			Logger::Log("[LOCK] Current Lock Duration = " + QString::number(lock_timer_.elapsed()) + "ms");
		lock_timer_.invalidate();
		if (settings_.logToFile())
			Logger::Log("[LOCK] Ongoing Lock is long enough to be counted as a Pause");
		if (settings_.isAutopauseEnabled())
			emit desktopLockEvent(LockEvent::LongOngoingLock);
	}
}

#ifdef Q_OS_LINUX

/**
 * Probes available DBus services to determine the best lock detection method.
 *
 * Priority Order:
 * 1. systemd-logind: The modern standard. Most reliable on recent distros.
 * 2. freedesktop.ScreenSaver: The standard interface implemented by many DEs (XFCE, MATE, etc.).
 * 3. GNOME ScreenSaver: Legacy specific interface.
 * 4. KDE ScreenSaver: Legacy specific interface.
 *
 * The first service found to be valid is selected and cached in linux_lock_method_.
 */
bool LockStateWatcher::initializeLinuxLockDetection()
{
	// Check services in order of preference

	// 1. systemd-logind (most reliable on systemd systems)
	QDBusInterface logindCheck(
		"org.freedesktop.login1",
		"/org/freedesktop/login1",
		"org.freedesktop.login1.Manager",
		QDBusConnection::systemBus()
	);
	if (logindCheck.isValid()) {
		linux_lock_method_ = LinuxLockMethod::SystemdLogind;
		if (settings_.logToFile())
			Logger::Log("[LOCK] Using systemd-logind for lock detection");
		return true;
	}

	// 2. Freedesktop ScreenSaver
	QDBusInterface fdoCheck(
		"org.freedesktop.ScreenSaver",
		"/org/freedesktop/ScreenSaver",
		"org.freedesktop.ScreenSaver",
		QDBusConnection::sessionBus()
	);
	if (fdoCheck.isValid()) {
		linux_lock_method_ = LinuxLockMethod::FreedesktopScreenSaver;
		if (settings_.logToFile())
			Logger::Log("[LOCK] Using freedesktop.ScreenSaver for lock detection");
		return true;
	}

	// 3. GNOME ScreenSaver
	QDBusInterface gnomeCheck(
		"org.gnome.ScreenSaver",
		"/org/gnome/ScreenSaver",
		"org.gnome.ScreenSaver",
		QDBusConnection::sessionBus()
	);
	if (gnomeCheck.isValid()) {
		linux_lock_method_ = LinuxLockMethod::GnomeScreenSaver;
		if (settings_.logToFile())
			Logger::Log("[LOCK] Using GNOME ScreenSaver for lock detection");
		return true;
	}

	// 4. KDE ScreenSaver
	QDBusInterface kdeCheck(
		"org.kde.screensaver",
		"/ScreenSaver",
		"org.kde.screensaver",
		QDBusConnection::sessionBus()
	);
	if (kdeCheck.isValid()) {
		linux_lock_method_ = LinuxLockMethod::KdeScreenSaver;
		if (settings_.logToFile())
			Logger::Log("[LOCK] Using KDE ScreenSaver for lock detection");
		return true;
	}

	linux_lock_method_ = LinuxLockMethod::None;
	if (settings_.logToFile())
		Logger::Log("[LOCK] WARNING: No lock detection method available on this Linux system");
	return false;
}

bool LockStateWatcher::querySystemdLogind()
{
	QDBusMessage msg = QDBusMessage::createMethodCall(
		"org.freedesktop.login1",
		"/org/freedesktop/login1/session/auto",
		"org.freedesktop.DBus.Properties",
		"Get"
	);
	msg << "org.freedesktop.login1.Session" << "LockedHint";

	// Use short timeout (100ms) to avoid blocking GUI - local DBus calls should be fast
	QDBusMessage reply = QDBusConnection::systemBus().call(msg, QDBus::Block, 100);

	if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
		QVariant v = reply.arguments().first();
		if (v.canConvert<QDBusVariant>()) {
			return v.value<QDBusVariant>().variant().toBool();
		}
	}
	return false;
}

bool LockStateWatcher::queryFreedesktopScreenSaver()
{
	QDBusMessage msg = QDBusMessage::createMethodCall(
		"org.freedesktop.ScreenSaver",
		"/org/freedesktop/ScreenSaver",
		"org.freedesktop.ScreenSaver",
		"GetActive"
	);

	// Use short timeout (100ms) to avoid blocking GUI
	QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 100);

	if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
		return reply.arguments().first().toBool();
	}
	return false;
}

bool LockStateWatcher::queryGnomeScreenSaver()
{
	QDBusMessage msg = QDBusMessage::createMethodCall(
		"org.gnome.ScreenSaver",
		"/org/gnome/ScreenSaver",
		"org.gnome.ScreenSaver",
		"GetActive"
	);

	// Use short timeout (100ms) to avoid blocking GUI
	QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 100);

	if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
		return reply.arguments().first().toBool();
	}
	return false;
}

bool LockStateWatcher::queryKdeScreenSaver()
{
	QDBusMessage msg = QDBusMessage::createMethodCall(
		"org.kde.screensaver",
		"/ScreenSaver",
		"org.kde.screensaver",
		"GetActive"
	);

	// Use short timeout (100ms) to avoid blocking GUI
	QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 100);

	if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
		return reply.arguments().first().toBool();
	}
	return false;
}

#endif // Q_OS_LINUX
