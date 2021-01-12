#ifndef SETTINGS_H
#define SETTINGS_H

#include <QtGlobal>
#include <QSettings>

class Settings
{
private:
	int backpause_min_;
	int autopause_enabled_;
	int autostart_timing_;
	int start_minimized_;
	int start_pinned_to_top_;
	int warning_nopause_;
	int warning_nopause_min_;
	int pause_for_warning_nopause_min_;
	int warning_activity_;
	int warning_activity_min_;
	QSettings sfile_;

public:
	Settings(QString filename);
	qint64 getBackpauseMsec();
	bool isAutopauseEnabled();
	bool isAutostartTimingEnabled();
	bool isMinimizedStartEnabled();
	bool isPinnedStartEnabled();
	bool showNoPauseWarning();
	bool showTooMuchActivityWarning();
	qint64 getPauseTimeForWarnTimeNoPauseMsec();
	qint64 getWarnTimeNoPauseMsec();
	qint64 getWarnTimeActivityMsec();
	void setAutopauseState(bool autopause_enabled);
	void setPinToTopState(bool pin2top_enabled);
};

#endif // SETTINGS_H
