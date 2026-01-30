#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QObject>
#include <QSqlDatabase>
#include <QDateTime>
#include <deque>
#include "types.h"
#include "settings.h"

class DatabaseManager : public QObject
{
    Q_OBJECT
public:
    explicit DatabaseManager(const Settings& settings, QObject *parent = nullptr);
    ~DatabaseManager();

    bool saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode);
    std::deque<TimeDuration> loadDurations();
    bool hasEntriesForDate(const QDate& date);
    bool saveCheckpoint(DurationType type, qint64 duration, const QDateTime& startTime, const QDateTime& endTime, long long& checkpointId);
    bool updateDurationsByStartTime(const std::deque<TimeDuration>& durations);
    bool checkSchemaOnStartup(); // Returns true if schema is valid, false if outdated
    void flushToDisc(); // Force pending writes to disk (for shutdown safety)

private:
    QSqlDatabase db;
    uint history_days_to_keep_;
    const Settings& settings_;
    bool lazyOpen();
    void lazyClose();
    bool validateSchema();
    bool createBackup(const std::deque<TimeDuration>& durations, TransactionMode mode);
};

#endif // DATABASEMANAGER_H