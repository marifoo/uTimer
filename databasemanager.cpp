#include "databasemanager.h"
#include <QStandardPaths>
#include <QDir>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include "logger.h"

DatabaseManager::DatabaseManager(int history_days_to_keep, QObject *parent)
    : history_days_to_keep_(history_days_to_keep), QObject(parent)
{
    db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName("uTimer.sqlite");
}

DatabaseManager::~DatabaseManager()
{
    if (db.isOpen()) {
        if (db.transaction()) {
            QSqlQuery query;
            query.prepare("DELETE FROM durations WHERE end_date < date('now', :days || ' days')");
            query.bindValue(":days", QString::number(-history_days_to_keep_));
            if (!query.exec()) {
                db.rollback();
                Logger::Log("[DB] Error clearing old durations: " + query.lastError().text());
            } else {
                db.commit();
            }
        } else {
            Logger::Log("[DB] Error starting transaction: " + db.lastError().text());
        }
        db.close();
    }
}

bool DatabaseManager::lazyInit()
{
    if (db.isOpen()) {
        return true;
	}
    if (!db.open()) {
        Logger::Log("[DB] Error opening database: " + db.lastError().text());
        return false;
    }

    QSqlQuery query;
    return query.exec(
        "CREATE TABLE IF NOT EXISTS durations ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT," 
        "type INTEGER NOT NULL," 
        "duration INTEGER NOT NULL," 
        "end_date DATE NOT NULL," 
        "end_time TIME NOT NULL"
        ")"
    );
}

bool DatabaseManager::saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode)
{
    if (!lazyInit()) {
        return false;
    }

    if (!db.transaction()) {
        Logger::Log("[DB] Error starting transaction for Saving: " + db.lastError().text());
        return false;
    }

    if (mode == TransactionMode::Replace) {
        // clear existing entries
        QSqlQuery clearQuery("DELETE FROM durations");
        if (!clearQuery.exec()) {
            db.rollback();
            Logger::Log("[DB] Error clearing durations table: " + clearQuery.lastError().text());
            return false;
        }
	}

    QSqlQuery query;
    query.prepare("INSERT INTO durations (type, duration, end_date, end_time) "
                  "VALUES (:type, :duration, :end_date, :end_time)");

    for (const auto& d : durations) {
        query.bindValue(":type", int(d.type));
        query.bindValue(":duration", qint64(d.duration));
        query.bindValue(":end_date", d.endTime.date().toString(Qt::ISODate));
        query.bindValue(":end_time", d.endTime.time().toString(Qt::ISODate));
        
        if (!query.exec()) {
            db.rollback();
            Logger::Log("[DB] Error inserting duration: " + query.lastError().text());
            return false;
        }
    }

    return db.commit();
}

std::deque<TimeDuration> DatabaseManager::loadDurations()
{
    std::deque<TimeDuration> durations;

    if (!lazyInit()) {
        return durations;
    }

    QSqlQuery query("SELECT type, duration, end_date, end_time FROM durations ORDER BY end_date, end_time");
    while (query.next()) {
        DurationType type = static_cast<DurationType>(query.value(0).toInt());
        qint64 duration = query.value(1).toLongLong();
        QDate endDate = QDate::fromString(query.value(2).toString(), Qt::ISODate);
        QTime endTime = QTime::fromString(query.value(3).toString(), Qt::ISODate);
        QDateTime endDateTime(endDate, endTime);
        durations.emplace_back(TimeDuration(type, duration, endDateTime));
    }
    
    return durations;
}
