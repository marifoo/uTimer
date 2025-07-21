#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QObject>
#include <QSqlDatabase>
#include <QDateTime>
#include <deque>
#include "types.h"

class DatabaseManager : public QObject
{
    Q_OBJECT
public:
    explicit DatabaseManager(int history_days_to_keep, QObject *parent = nullptr);
    ~DatabaseManager();

    bool saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode);
    std::deque<TimeDuration> loadDurations();

private:
    QSqlDatabase db;
	uint history_days_to_keep_;
    bool lazyOpen();
    void lazyClose();
};

#endif // DATABASEMANAGER_H