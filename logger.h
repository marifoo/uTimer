#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QFile>

class Logger
{
	QFile *logfile_;

	Logger();
	void log(const QString & text);

public:
	static void Log(const QString & text);
	~Logger();
};

#endif // LOGGER_H
