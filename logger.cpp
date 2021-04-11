#include "logger.h"
#include <QDateTime>
#include <QTextStream>

Logger::Logger()
{
	logfile_ = new QFile();
	logfile_->setFileName("utimer.log");
	logfile_->open(QIODevice::ReadWrite | QIODevice::Truncate | QIODevice::Text);
	log("uTimer Startup <3");
}

void Logger::Log(const QString &text)
{
	static Logger L;
	L.log(text);
}

void Logger::log(const QString &text)
{
	const QString msg = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz: ") + text + "\n";
	QTextStream out(logfile_);
	out.setCodec("UTF-8");
	if (logfile_ != 0)
		out << msg;
}

Logger::~Logger()
{
	log("uTimer Shutdown");
	if (logfile_ != 0)
		logfile_->close();
}

