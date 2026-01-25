#include "testcommon.h"

namespace TestCommon {

TimeDuration mk(DurationType type, qint64 startMs, qint64 endMs)
{
    QDateTime start = QDateTime::fromMSecsSinceEpoch(startMs, Qt::UTC);
    QDateTime end = QDateTime::fromMSecsSinceEpoch(endMs, Qt::UTC);
    return TimeDuration(type, start, end);
}

QString createSettingsFile(const QString& dirPath, int historyDays)
{
    QString settingsPath = QDir(dirPath).filePath("user-settings.ini");
    QSettings seed(settingsPath, QSettings::IniFormat);
    seed.setValue("uTimer/history_days_to_keep", historyDays);
    seed.setValue("uTimer/debug_log_to_file", false);
    seed.sync();
    return settingsPath;
}

qint64 sumDurations(const std::deque<TimeDuration>& d, DurationType type)
{
    qint64 total = 0;
    for (const auto& t : d) {
        if (t.type == type) total += t.duration;
    }
    return total;
}

} // namespace TestCommon
