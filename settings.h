#ifndef SETTINGS_H
#define SETTINGS_H

#include <QtGlobal>
#include <QSettings>

class Settings
{
private:
	int backpause_min_;
	bool autopause_enabled_;
	bool autostart_timing_;
	bool start_minimized_;
	bool start_pinned_to_top_;
	bool warning_nopause_;
	bool warning_activity_;
	int warning_nopause_min_;
	int pause_for_warning_nopause_min_;
	int warning_activity_min_;
	QSettings sfile_;
	qint64 convMinToMsec(const int minutes) const;

public:
	Settings(const QString filename);
	bool isAutopauseEnabled() const;
	bool isAutostartTimingEnabled() const;
	bool isMinimizedStartEnabled() const;
	bool isPinnedStartEnabled() const;
	bool showNoPauseWarning() const;
	bool showTooMuchActivityWarning() const;
	qint64 getBackpauseMsec() const;
	qint64 getPauseTimeForWarnTimeNoPauseMsec() const;
	qint64 getWarnTimeNoPauseMsec() const;
	qint64 getWarnTimeActivityMsec() const;
	void setAutopauseState(const bool autopause_enabled);
	void setPinToTopState(const bool pin2top_enabled);
};

#endif // SETTINGS_H
