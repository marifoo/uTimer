#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QFile>

class Settings;

class Logger
{
	static const Settings* s_settings_;
	QFile *logfile_;
	const static int max_lines_ = 1024;

	Logger();
	void log(const QString & text);

public:
	/// Must be called once at startup before any Log() call reaches a call-site.
	/// Until registered, all Log() calls are silently dropped.
	static void registerSettings(const Settings& s);
	static void Log(const QString & text);
	/// Returns the absolute path to the log file (applicationDir/uTimer.log).
	/// This is a pure path computation — does not check if the file exists.
	static QString logFilePath();
	~Logger();
};

#endif // LOGGER_H
