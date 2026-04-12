#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QFile>

class Logger
{
	QFile *logfile_;
	const static int max_lines_ = 1024;

	Logger();
	void log(const QString & text);

public:
	static void Log(const QString & text);
	/// Returns the absolute path to the log file (applicationDir/uTimer.log).
	/// This is a pure path computation — does not check if the file exists.
	static QString logFilePath();
	~Logger();
};

#endif // LOGGER_H
