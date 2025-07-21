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
	~Logger();
};

#endif // LOGGER_H
