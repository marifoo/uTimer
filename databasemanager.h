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
    bool updateOrAppendCheckpoint(DurationType type, qint64 duration, const QDateTime& endTime);
    bool updateDurationsByStartTime(const std::deque<TimeDuration>& durations);

private:
    QSqlDatabase db;
    uint history_days_to_keep_;
    const Settings& settings_;
    bool lazyOpen();
    void lazyClose();
    bool createBackup(const std::deque<TimeDuration>& durations, TransactionMode mode);
    bool updateDurationByStartTime(DurationType type, qint64 duration, const QDateTime& endTime, int& existingId);
};

#endif // DATABASEMANAGER_H