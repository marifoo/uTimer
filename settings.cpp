#include "settings.h"

Settings::Settings(QString filename) : sfile_(filename, QSettings::IniFormat)
{
	autostart_timing_ = qBound(0, sfile_.value("uTimer/press_start_button_on_app_start", 1).toInt(), 1);
	autopause_enabled_ = qBound(0, sfile_.value("uTimer/autopause_enabled", 0).toInt(), 1);
	backpause_min_ = qBound(0, sfile_.value("uTimer/autopause_threshold_minutes", 12).toInt(), 200);
	start_minimized_ = qBound(0, sfile_.value("uTimer/start_minimized_to_tray", 0).toInt(), 1);

	sfile_.setValue("uTimer/press_start_button_on_app_start", autostart_timing_);
	sfile_.setValue("uTimer/autopause_enabled", autopause_enabled_);
	sfile_.setValue("uTimer/autopause_threshold_minutes", backpause_min_);
	sfile_.setValue("uTimer/start_minimized_to_tray", start_minimized_);
}

qint64 Settings::getBackpauseMsec()
{
	return (static_cast<qint64>(backpause_min_) * 60000);
}

bool Settings::isAutopauseEnabled()
{
	return (autopause_enabled_ == 1);
}

bool Settings::isAutostartTimingEnabled()
{
	return (autostart_timing_ == 1);
}

bool Settings::isMinimizedStartEnabled()
{
	return (start_minimized_ == 1);
}

void Settings::setAutopauseState(bool autopause_enabled)
{
	backpause_min_ = qBound(0, sfile_.value("uTimer/autopause_threshold_minutes").toInt(), 200);
	autopause_enabled_ = autopause_enabled;
	sfile_.setValue("uTimer/autopause_enabled", autopause_enabled_);
}
