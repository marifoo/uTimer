#include "logger.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>

Logger::Logger()
{
    logfile_ = new QFile();
    logfile_->setFileName(QDir(QCoreApplication::applicationDirPath()).filePath("uTimer.log"));
    logfile_->open(QIODevice::ReadWrite | QIODevice::Append | QIODevice::Text);
    log("uTimer Startup >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");
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
    if (logfile_ != nullptr)
        out << msg;
    out.flush();
    
    // Also flush to disk for maximum safety during shutdown
    // Note: This makes every log call slower (~10-100x) due to disk I/O,
    // but ensures logs are never lost even during hard shutdowns
    logfile_->flush();
}

Logger::~Logger()
{
    log("uTimer Shutdown <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");

    if (logfile_ != nullptr) {
        logfile_->flush();
        logfile_->seek(0);
        QTextStream in(logfile_);
        in.setCodec("UTF-8");
        QStringList lines;
        while (!in.atEnd()) {
            lines << in.readLine();
        }
        if (lines.size() > max_lines_) {
            lines = lines.mid(lines.size() - max_lines_);
            logfile_->resize(0);
            logfile_->seek(0);
            QTextStream out2(logfile_);
            out2.setCodec("UTF-8");
            for (const QString& l : lines) {
                out2 << l << "\n";
            }
            out2.flush();
            logfile_->flush();  // Ensure rotated log is written to disk
        }
        logfile_->close();
    }
}

