#include "lockstatewatcher.h"
#include <QDebug>
#include <QDateTime>
#include <algorithm>
#include <WtsApi32.h>
#include "logger.h"

LockStateWatcher::LockStateWatcher(const Settings &settings, QWidget *parent)
	: QWidget(parent),
		settings_(settings),
		lock_state_buffer_{ false, false, false, false, false},
		buffer_for_lock{ false, false, true, true, true},
		buffer_for_unlock{ true, true, false, false, false}
{
	lock_timer_.invalidate();
}

bool LockStateWatcher::isSessionLocked()
{
	// https://stackoverflow.com/questions/29326685/c-check-if-computer-is-locked/43055326#43055326

	typedef BOOL( PASCAL * WTSQuerySessionInformation )( HANDLE hServer, DWORD SessionId, WTS_INFO_CLASS WTSInfoClass, LPTSTR* ppBuffer, DWORD* pBytesReturned );
	typedef void ( PASCAL * WTSFreeMemory )( PVOID pMemory );

	WTSINFOEXW * pInfo = NULL;
	WTS_INFO_CLASS wtsic = WTSSessionInfoEx;
	bool bRet = false;
	LPTSTR ppBuffer = NULL;
	DWORD dwBytesReturned = 0;
	LONG dwFlags = 0;
	WTSQuerySessionInformation pWTSQuerySessionInformation = NULL;
	WTSFreeMemory pWTSFreeMemory = NULL;

	HMODULE hLib = LoadLibraryW( L"wtsapi32.dll" );
	if( !hLib )
	{
			return false;
	}
	pWTSQuerySessionInformation = (WTSQuerySessionInformation) GetProcAddress( hLib, "WTSQuerySessionInformationW" );
	if( pWTSQuerySessionInformation )
	{
			pWTSFreeMemory = (WTSFreeMemory) GetProcAddress( hLib, "WTSFreeMemory" );
			if( pWTSFreeMemory != NULL )
			{
					DWORD dwSessionID = WTSGetActiveConsoleSessionId();
					if( pWTSQuerySessionInformation( WTS_CURRENT_SERVER_HANDLE, dwSessionID, wtsic, &ppBuffer, &dwBytesReturned ) )
					{
							if( dwBytesReturned > 0 )
							{
									pInfo = (WTSINFOEXW*) ppBuffer;
									if( pInfo->Level == 1 )
									{
											dwFlags = pInfo->Data.WTSInfoExLevel1.SessionFlags;
									}
									if( dwFlags == WTS_SESSIONSTATE_LOCK )
									{
											bRet = true;
									}
							}
							pWTSFreeMemory( ppBuffer );
							ppBuffer = NULL;
					}
			}
	}
	if( hLib != NULL )
	{
			FreeLibrary( hLib );
	}
	return bRet;
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
			Logger::Log("[LOCK] Current Lock Duration=" + QString::number(lock_timer_.elapsed()));
		lock_timer_.invalidate();
		if (settings_.logToFile())
			Logger::Log("[LOCK] Unlock determined <<");
		if (settings_.isAutopauseEnabled())
			emit desktopLockEvent(LockEvent::Unlock);
	}

	if (lock_timer_.isValid() && (lock_timer_.elapsed() >= settings_.getBackpauseMsec())) {
		if (settings_.logToFile() && lock_timer_.isValid())
			Logger::Log("[LOCK] Current Lock Duration=" + QString::number(lock_timer_.elapsed()));
		lock_timer_.invalidate();
		if (settings_.logToFile())
			Logger::Log("[LOCK] Ongoing Lock is long enough to be counted as a Pause");
		if (settings_.isAutopauseEnabled())
			emit desktopLockEvent(LockEvent::LongOngoingLock);
	}
}
