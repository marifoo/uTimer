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
    explicit DatabaseManager(QObject *parent = nullptr);
    ~DatabaseManager();

    bool saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode);
    std::deque<TimeDuration> loadDurations();

private:
    QSqlDatabase db;

    bool lazyInit();
};

#endif // DATABASEMANAGER_H