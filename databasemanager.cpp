#include "databasemanager.h"
#include <QStandardPaths>
#include <QDir>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include "logger.h"
#include "settings.h"

DatabaseManager::DatabaseManager(const Settings& settings, QObject *parent)
    : history_days_to_keep_(std::max(settings.getHistoryDays(), 0)), QObject(parent), settings_(settings)
{
    db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName("uTimer.sqlite");

    if ((history_days_to_keep_ == 0) && settings.logToFile()) {
        Logger::Log("[DB] History days to keep is set to 0, database will not be used.");
	}
}

DatabaseManager::~DatabaseManager()
{
    lazyClose();
}

bool DatabaseManager::lazyOpen()
{
    if (history_days_to_keep_ == 0) {
        return false;
    }

    if (db.isOpen()) {
        return true;
	}
    if (!db.open()) {
        if (settings_.logToFile())
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

void DatabaseManager::lazyClose() 
{
    if (db.isOpen()) {
        if (db.transaction()) {
            QSqlQuery query;
            if (history_days_to_keep_ == 0) {
                query.prepare("DELETE FROM durations");
            }
            else {
                query.prepare("DELETE FROM durations WHERE end_date < date('now', :days || ' days')");
                query.bindValue(":days", QString::number((-1) * ((int)history_days_to_keep_)));
            }

            if (!query.exec()) {
                db.rollback();
                if (settings_.logToFile())
                    Logger::Log("[DB] Error clearing old durations: " + query.lastError().text());
            }
            else {
                db.commit();
            }
        }
        else {
            if (settings_.logToFile())
                Logger::Log("[DB] Error starting transaction: " + db.lastError().text());
        }
        db.close();
    }
}

bool DatabaseManager::saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode)
{
    if (!lazyOpen()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Could not lazy open DB to save Durations");
        }
        return false;
    }

    if (!db.transaction()) {
        if (settings_.logToFile())
            Logger::Log("[DB] Error starting transaction for Saving: " + db.lastError().text());
        return false;
    }

    if (mode == TransactionMode::Replace) {
        // clear existing entries
        QSqlQuery clearQuery("DELETE FROM durations");
        if (!clearQuery.exec()) {
            db.rollback();
            if (settings_.logToFile())
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
        query.bindValue(":end_time", d.endTime.time().toString("HH:mm:ss.zzz"));
        
        if (!query.exec()) {
            db.rollback();
            if (settings_.logToFile())
                Logger::Log("[DB] Error inserting duration: " + query.lastError().text());
            return false;
        }
    }

    bool ret = db.commit();

    lazyClose();

    return ret;
}

std::deque<TimeDuration> DatabaseManager::loadDurations()
{
    std::deque<TimeDuration> durations;

    if (!lazyOpen()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Could not lazy open DB to load Durations");
        }
        return durations;
    }

    QSqlQuery query("SELECT type, duration, end_date, end_time FROM durations ORDER BY end_date, end_time");
    while (query.next()) {
        DurationType type = static_cast<DurationType>(query.value(0).toInt());
        qint64 duration = query.value(1).toLongLong();
        QDate endDate = QDate::fromString(query.value(2).toString(), Qt::ISODate);
        QTime endTime = QTime::fromString(query.value(3).toString(), "HH:mm:ss.zzz");
        QDateTime endDateTime(endDate, endTime);
        durations.emplace_back(TimeDuration(type, duration, endDateTime));
    }

    lazyClose();
    
    return durations;
}

bool DatabaseManager::hasEntriesForDate(const QDate& date)
{
    if (!lazyOpen()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Could not lazy open DB to check entries for date");
        }
        return false;
    }

    QSqlQuery query;
    query.prepare("SELECT COUNT(*) FROM durations WHERE end_date = :date");
    query.bindValue(":date", date.toString(Qt::ISODate));
    
    bool hasEntries = false;
    if (query.exec() && query.next()) {
        int count = query.value(0).toInt();
        hasEntries = (count > 0);
    } else {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error checking entries for date: " + query.lastError().text());
        }
    }

    lazyClose();
    return hasEntries;
}
