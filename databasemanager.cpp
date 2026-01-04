#include "databasemanager.h"
#include <QStandardPaths>
#include <QDir>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QFile>
#include <QTextStream>
#include "logger.h"
#include "settings.h"

DatabaseManager::DatabaseManager(const Settings& settings, QObject *parent)
    : history_days_to_keep_(std::max(settings.getHistoryDays(), 0)), QObject(parent), settings_(settings)
{
    // Use unique connection name to avoid conflicts with multiple instances
    QString connectionName = QString("uTimer_connection_%1").arg(reinterpret_cast<quintptr>(this));
    db = QSqlDatabase::addDatabase("QSQLITE", connectionName);
    db.setDatabaseName("uTimer.sqlite");

    // Log when database is disabled (history_days_to_keep_ = 0 means no history storage)
    if ((history_days_to_keep_ == 0) && settings.logToFile()) {
        Logger::Log("[DB] History days to keep is set to 0, database will not be used.");
	}
}

DatabaseManager::~DatabaseManager()
{
    lazyClose();
    
    // Remove the database connection to prevent Qt warnings
    if (QSqlDatabase::contains(db.connectionName())) {
        QSqlDatabase::removeDatabase(db.connectionName());
    }
}

bool DatabaseManager::lazyOpen()
{
    // Don't open database if history storage is disabled
    if (history_days_to_keep_ == 0) {
        return false;
    }

    // Return true if already open
    if (db.isOpen()) {
        return true;
	}
    
    if (!db.open()) {
        if (settings_.logToFile())
            Logger::Log("[DB] Error opening database: " + db.lastError().text());
        return false;
    }

    // Create the durations table if it doesn't exist
    QSqlQuery query_new(db);
    bool tableCreated = query_new.exec(
        "CREATE TABLE IF NOT EXISTS durations ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT," 
        "type INTEGER NOT NULL," // 0 = Activity, 1 = Pause (DurationType enum)
        "duration INTEGER NOT NULL," // Duration in milliseconds
        "end_date DATE NOT NULL," // Date when the duration ended
        "end_time TIME NOT NULL" // Time when the duration ended (with milliseconds)
        ")"
    );
    
    if (!tableCreated) {
        if (settings_.logToFile())
            Logger::Log("[DB] Error creating table: " + query_new.lastError().text());
        db.close();
        return false;
    }

    // Create index for date-based queries (cleanup and hasEntriesForDate)
    QSqlQuery indexQuery(db);
    if (!indexQuery.exec("CREATE INDEX IF NOT EXISTS idx_end_date ON durations(end_date)")) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Warning: Failed to create index: " + indexQuery.lastError().text());
        }
        // Non-fatal: continue even if index creation fails
    }

    // Cleanup old entries
    if (db.transaction()) {
        QSqlQuery query(db);
        bool querySuccessful = false;
        
        query.prepare("DELETE FROM durations WHERE end_date < date('now', '-' || :days || ' days')");
        query.bindValue(":days", static_cast<int>(history_days_to_keep_));
        querySuccessful = query.exec();

        if (!querySuccessful) {
            db.rollback();
            if (settings_.logToFile())
                Logger::Log("[DB] Error clearing old durations: " + query.lastError().text());
            db.close();
            return false;
        }
        else {
            if (!db.commit()) {
                if (settings_.logToFile())
                    Logger::Log("[DB] Error committing cleanup transaction: " + db.lastError().text());
            }
        }
    }
    else {
        if (settings_.logToFile())
            Logger::Log("[DB] Error starting cleanup transaction: " + db.lastError().text());
    }
    
    return true;
}

void DatabaseManager::lazyClose() 
{
    if (db.isOpen()) {
        db.close();
    }
}

bool DatabaseManager::createBackup(const std::deque<TimeDuration>& durations, TransactionMode mode)
{
    // Don't create backup if database doesn't exist
    if (!QFile::exists(db.databaseName())) {
        return true; // No file to backup, not an error
    }

    // Close database before copying
    bool wasOpen = db.isOpen();
    if (wasOpen) {
        db.close();
    }

    // Generate backup filename with ISO timestamp
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-ddTHH-mm-ss");
    QString backupName = QString("%1.%2.backup").arg(db.databaseName()).arg(timestamp);
    QString durationsFileName = QString("%1.%2.durations.txt").arg(db.databaseName()).arg(timestamp);

    // Copy the database file
    bool success = QFile::copy(db.databaseName(), backupName);
    
    if (!success) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error: Failed to create backup of database");
        }
    } else if (settings_.logToFile()) {
        Logger::Log("[DB] Created database backup: " + backupName);
    }

    // Write durations to text file
    QFile durationsFile(durationsFileName);
    if (durationsFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&durationsFile);
        out << "Transaction Mode: " << (mode == TransactionMode::Replace ? "Replace" : "Append") << "\n";
        out << "Total Durations: " << durations.size() << "\n";
        out << "Timestamp: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
        out << "----------------------------------------\n";
        
        for (const auto& d : durations) {
            out << "Type: " << (d.type == DurationType::Activity ? "Activity" : "Pause") << " | ";
            out << "Duration: " << d.duration << "ms | ";
            out << "End Date: " << d.endTime.date().toString(Qt::ISODate) << " | ";
            out << "End Time: " << d.endTime.time().toString("HH:mm:ss.zzz") << "\n";
        }
        
        durationsFile.close();
        
        if (settings_.logToFile()) {
            Logger::Log("[DB] Created durations log: " + durationsFileName);
        }
    } else if (settings_.logToFile()) {
        Logger::Log("[DB] Warning: Could not create durations log file");
    }

    // Reopen database if it was open before
    if (wasOpen) {
        if (!db.open()) {
            if (settings_.logToFile()) {
                Logger::Log("[DB] CRITICAL: Failed to reopen database after backup: " + db.lastError().text());
            }
            return false;  // Signal backup failure since DB is now unusable
        }
    }

    return success;
}

bool DatabaseManager::saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode)
{
    // Create backup before any write operation
    if (!createBackup(durations, mode)) {
        if (settings_.logToFile()) {
            QString modeStr = (mode == TransactionMode::Replace) ? "REPLACE" : "APPEND";
            Logger::Log("[DB] Warning: Backup failed before " + modeStr + " operation - proceeding without backup");
        }
    }

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

    // Replace mode: clear existing entries before inserting new ones
    if (mode == TransactionMode::Replace) {
        QSqlQuery clearQuery(db);
        if (!clearQuery.exec("DELETE FROM durations")) {
            db.rollback();
            if (settings_.logToFile())
                Logger::Log("[DB] Error clearing durations table: " + clearQuery.lastError().text());
            return false;
        }
	}

    // Prepare insert statement for batch insertion
    QSqlQuery query(db);
    query.prepare("INSERT INTO durations (type, duration, end_date, end_time) "
                  "VALUES (:type, :duration, :end_date, :end_time)");

    // Insert each duration entry
    for (const auto& d : durations) {
        query.bindValue(":type", static_cast<int>(d.type));
        query.bindValue(":duration", d.duration);
        query.bindValue(":end_date", d.endTime.date().toString(Qt::ISODate));
        query.bindValue(":end_time", d.endTime.time().toString("HH:mm:ss.zzz"));
        
        if (!query.exec()) {
            db.rollback();
            if (settings_.logToFile())
                Logger::Log("[DB] Error inserting duration: " + query.lastError().text());
            return false;
        }
    }

    bool commitSuccessful = db.commit();
    if (!commitSuccessful && settings_.logToFile()) {
        Logger::Log("[DB] Error committing save transaction: " + db.lastError().text());
    }

    lazyClose();

    return commitSuccessful;
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

    // Load all durations ordered by end date and time
    QSqlQuery query("SELECT type, duration, end_date, end_time FROM durations ORDER BY end_date, end_time", db);
    if (!query.exec()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error executing load query: " + query.lastError().text());
        }
        lazyClose();
        return durations;
    }
    
    // Parse each row and reconstruct TimeDuration objects
    while (query.next()) {
        int typeInt = query.value(0).toInt();
        qint64 duration = query.value(1).toLongLong();
        QDate endDate = QDate::fromString(query.value(2).toString(), Qt::ISODate);
        QTime endTime = QTime::fromString(query.value(3).toString(), "HH:mm:ss.zzz");
        QDateTime endDateTime(endDate, endTime);

        // Validate type enum range
        if (typeInt != static_cast<int>(DurationType::Activity) &&
            typeInt != static_cast<int>(DurationType::Pause)) {
            if (settings_.logToFile()) {
                Logger::Log(QString("[DB] Warning: Invalid type value %1, skipping entry").arg(typeInt));
            }
            continue;
        }

        DurationType type = static_cast<DurationType>(typeInt);

        // Validate parsed data before adding
        if (endDate.isValid() && endTime.isValid() && duration >= 0) {
            durations.emplace_back(TimeDuration(type, duration, endDateTime));
        } else if (settings_.logToFile()) {
            Logger::Log(QString("[DB] Warning: Skipped invalid duration entry - Date: %1, Time: %2, Duration: %3")
                .arg(endDate.toString()).arg(endTime.toString()).arg(duration));
        }
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

    // Check if any entries exist for the specified date
    QSqlQuery query(db);
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
