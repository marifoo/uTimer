#include "settings.h"

Settings::Settings(QString filename) : sfile_(filename, QSettings::IniFormat)
{
	sfile_.setIniCodec("UTF-8");

	autostart_timing_ = qBound(0, sfile_.value("uTimer/press_start_button_on_app_start", 1).toInt(), 1);
	autopause_enabled_ = qBound(0, sfile_.value("uTimer/autopause_enabled", 0).toInt(), 1);
	backpause_min_ = qBound(0, sfile_.value("uTimer/autopause_threshold_minutes", 12).toInt(), 200);
	start_minimized_ = qBound(0, sfile_.value("uTimer/start_minimized_to_tray", 0).toInt(), 1);
	start_pinned_to_top_ = qBound(0, sfile_.value("uTimer/start_pinned_to_top", 0).toInt(), 1);
	pin_when_paused_ = qBound(0, sfile_.value("uTimer/pin_to_top_when_paused", 0).toInt(), 1);
	warning_nopause_min_ = qBound(0, sfile_.value("uTimer/too_much_activity_without_pause_warning_min", 0).toInt(), 1000);
	pause_for_warning_nopause_min_ = 30;
	warning_activity_min_ = qBound(0, sfile_.value("uTimer/too_much_activity_warning_min", 0).toInt(), 1000);

	sfile_.setValue("uTimer/press_start_button_on_app_start", autostart_timing_);
	sfile_.setValue("uTimer/autopause_enabled", autopause_enabled_);
	sfile_.setValue("uTimer/autopause_threshold_minutes", backpause_min_);
	sfile_.setValue("uTimer/start_minimized_to_tray", start_minimized_);
	sfile_.setValue("uTimer/start_pinned_to_top", start_pinned_to_top_);
	sfile_.setValue("uTimer/pin_to_top_when_paused", pin_when_paused_);
	sfile_.setValue("uTimer/too_much_activity_without_pause_warning_min", warning_nopause_min_);
	sfile_.setValue("uTimer/too_much_activity_warning_min", warning_activity_min_);
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

bool Settings::isPinnedStartEnabled()
{
	return (start_pinned_to_top_ == 1);
}

bool Settings::pinToTopWhenPaused()
{
	return (pin_when_paused_ == 1);
}

qint64 Settings::getPauseTimeForWarnTimeNoPauseMsec()
{
	return (static_cast<qint64>(pause_for_warning_nopause_min_) * 60000);;
}

qint64 Settings::getWarnTimeNoPauseMsec()
{
	return (static_cast<qint64>(warning_nopause_min_) * 60000);;
}

qint64 Settings::getWarnTimeActivityMsec()
{
	return (static_cast<qint64>(warning_activity_min_) * 60000);;
}

void Settings::setAutopauseState(bool autopause_enabled)
{
	backpause_min_ = qBound(0, sfile_.value("uTimer/autopause_threshold_minutes", 12).toInt(), 200);
	autopause_enabled_ = autopause_enabled ? 1 : 0;
	sfile_.setValue("uTimer/autopause_enabled", autopause_enabled_);
}

void Settings::setPinToTopState(bool pin2top_enabled)
{
	start_pinned_to_top_ = pin2top_enabled ? 1 : 0;
	sfile_.setValue("uTimer/start_pinned_to_top", start_pinned_to_top_);
}
