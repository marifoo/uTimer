#include "lockstatewatcher.h"
#include <QDebug>
#include <QDateTime>
#include <algorithm>
#include "logger.h"

#ifdef Q_OS_WIN
#include <WtsApi32.h>
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

bool LockStateWatcher::isSessionLocked()
{
#ifdef Q_OS_WIN
	// taken from https://stackoverflow.com/questions/29326685/c-check-if-computer-is-locked/43055326#43055326
	typedef BOOL( PASCAL * WTSQuerySessionInformation )( HANDLE hServer, DWORD SessionId, WTS_INFO_CLASS WTSInfoClass, LPTSTR* ppBuffer, DWORD* pBytesReturned );
	typedef void ( PASCAL * WTSFreeMemory )( PVOID pMemory );

	WTSINFOEXW * pInfo = nullptr;
	WTS_INFO_CLASS wtsic = WTSSessionInfoEx;
	bool bRet = false;
	LPTSTR ppBuffer = nullptr;
	DWORD dwBytesReturned = 0;
	LONG dwFlags = 0;
	WTSQuerySessionInformation pWTSQuerySessionInformation = nullptr;
	WTSFreeMemory pWTSFreeMemory = nullptr;

	HMODULE hLib = LoadLibraryW(L"wtsapi32.dll");
	if (!hLib) {
		return false;
	}

	pWTSQuerySessionInformation = reinterpret_cast<WTSQuerySessionInformation>(GetProcAddress(hLib, "WTSQuerySessionInformationW"));
	if (pWTSQuerySessionInformation) {
		pWTSFreeMemory = reinterpret_cast<WTSFreeMemory>(GetProcAddress(hLib, "WTSFreeMemory"));
		if (pWTSFreeMemory != nullptr) {
			DWORD dwSessionID = WTSGetActiveConsoleSessionId();
			if (pWTSQuerySessionInformation( WTS_CURRENT_SERVER_HANDLE, dwSessionID, wtsic, &ppBuffer, &dwBytesReturned)) {
				if (dwBytesReturned > 0) {
					pInfo = reinterpret_cast<WTSINFOEXW*>(ppBuffer);
					if (pInfo->Level == 1) {
						dwFlags = pInfo->Data.WTSInfoExLevel1.SessionFlags;
					}
					if (dwFlags == WTS_SESSIONSTATE_LOCK) {
						bRet = true;
					}
				}
				pWTSFreeMemory(ppBuffer);
				ppBuffer = nullptr;
			}
		}
	}
	if (hLib != nullptr) {
		FreeLibrary(hLib);
	}
	return bRet;
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

void LockStateWatcher::update()
{	
	const LockEvent lock_event = determineLockEvent(isSessionLocked());

	if (lock_event == LockEvent::Lock) {
		if (settings_.logToFile())
			Logger::Log("[LOCK] >> Lock determined");
		lock_timer_.start();
	}
	else if (lock_event == LockEvent::Unlock) {
		if (settings_.logToFile() && lock_timer_.isValid())
			Logger::Log("[LOCK] Current Lock Duration = " + QString::number(lock_timer_.elapsed()) + "ms");
		lock_timer_.invalidate();
		if (settings_.logToFile())
			Logger::Log("[LOCK] Unlock determined <<");
		if (settings_.isAutopauseEnabled())
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

	QDBusMessage reply = QDBusConnection::systemBus().call(msg, QDBus::Block, 1000);

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

	QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 1000);

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

	QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 1000);

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

	QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 1000);

	if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
		return reply.arguments().first().toBool();
	}
	return false;
}

#endif // Q_OS_LINUX
