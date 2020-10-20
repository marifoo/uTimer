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
	QSettings sfile_;

public:
	Settings(QString filename);
	qint64 getBackpauseMsec();
	bool isAutopauseEnabled();
	bool isAutostartTimingEnabled();
	bool isMinimizedStartEnabled();
	void setAutopauseState(bool autopause_enabled);
};

#endif // SETTINGS_H
