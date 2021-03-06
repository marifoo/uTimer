#include "settings.h"
#include "helpers.h"
#include "logger.h"

Settings::Settings(const QString filename) : sfile_(filename, QSettings::IniFormat)
{
	sfile_.setIniCodec("UTF-8");
	readSettingsFile();
	sfile_.clear();
	writeSettingsFile();
	sfile_.sync();
}

void Settings::readSettingsFile()
{
	autostart_timing_ = sfile_.value("uTimer/press_start_button_on_app_start", true).toBool();
	autopause_enabled_ = sfile_.value("uTimer/autopause_enabled", true).toBool();
	backpause_min_ = qBound(0, sfile_.value("uTimer/autopause_threshold_minutes", 15).toInt(), 99);
	start_minimized_ = sfile_.value("uTimer/start_minimized_to_tray", false).toBool();
	start_pinned_to_top_ = sfile_.value("uTimer/start_pinned_to_top", false).toBool();
	warning_nopause_ = sfile_.value("uTimer/show_warning_when_not_30min_pause_after_6h_activity", false).toBool();
	warning_nopause_min_ = 6*60;
	pause_for_warning_nopause_min_ = 30;
	warning_activity_ = sfile_.value("uTimer/show_warning_after_9h45min_activity", false).toBool();
	warning_activity_min_ = 9*60+45;
	log_to_file_ = sfile_.value("uTimer/debug_log_to_file", true).toBool();
}

void Settings::writeSettingsFile()
{
	sfile_.setValue("uTimer/press_start_button_on_app_start", autostart_timing_);
	sfile_.setValue("uTimer/autopause_enabled", autopause_enabled_);
	sfile_.setValue("uTimer/autopause_threshold_minutes", backpause_min_);
	sfile_.setValue("uTimer/start_minimized_to_tray", start_minimized_);
	sfile_.setValue("uTimer/start_pinned_to_top", start_pinned_to_top_);
	sfile_.setValue("uTimer/show_warning_when_not_30min_pause_after_6h_activity", warning_nopause_);
	sfile_.setValue("uTimer/show_warning_after_9h45min_activity", warning_activity_);
	sfile_.setValue("uTimer/debug_log_to_file", log_to_file_);

	if (log_to_file_)
		Logger::Log("Current Autopause Settings are: Enabled = " + QString::number(autopause_enabled_) + "; Minutes = " + QString::number(backpause_min_));
}

bool Settings::isAutopauseEnabled() const
{
	return autopause_enabled_;
}

bool Settings::isAutostartTimingEnabled() const
{
	return autostart_timing_;
}

bool Settings::isMinimizedStartEnabled() const
{
	return start_minimized_;
}

bool Settings::isPinnedStartEnabled() const
{
	return start_pinned_to_top_;
}

bool Settings::showNoPauseWarning() const
{
	return warning_nopause_;
}

bool Settings::showTooMuchActivityWarning() const
{
	return warning_activity_;
}

bool Settings::logToFile() const
{
	return log_to_file_;
}

QString Settings::getBackpauseMin() const
{
	return QString::number(backpause_min_);
}

qint64 Settings::getBackpauseMsec() const
{
	return convMinToMsec(backpause_min_);
}

qint64 Settings::getPauseTimeForWarnTimeNoPauseMsec() const
{
	return convMinToMsec(pause_for_warning_nopause_min_);
}

qint64 Settings::getWarnTimeNoPauseMsec() const
{
	return convMinToMsec(warning_nopause_min_);
}

qint64 Settings::getWarnTimeActivityMsec() const
{
	return convMinToMsec(warning_activity_min_);
}

void Settings::setAutopauseState(const bool autopause_enabled)
{
	sfile_.sync();
	readSettingsFile();
	autopause_enabled_ = autopause_enabled;
	writeSettingsFile();
}

void Settings::setPinToTopState(const bool pin2top_enabled)
{
	sfile_.sync();
	start_pinned_to_top_ = pin2top_enabled;
	writeSettingsFile();
}
