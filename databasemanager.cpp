/**
 * DatabaseManager - SQLite persistence for time duration data.
 *
 * Connection pattern:
 * - Lazy-open: Database opened on first operation, closed after each operation
 * - This prevents file locking issues and allows external backup tools to work
 *
 * Save methods:
 * - saveDurations(): Full save with backup creation, used for stopTimer/HistoryDialog
 * - saveCheckpoint(): Lightweight update without backup, used for periodic checkpoints
 * - updateDurationsByStartTime(): Smart upsert matching entries by calculated start time
 *
 * Backup strategy:
 * - saveDurations() creates timestamped .backup file + .durations.txt log before writes
 * - saveCheckpoint() skips backup for performance (called every 5 minutes)
 *
 * Configuration:
 * - history_days_to_keep_ = 0 disables database entirely (all methods return early)
 * - Old entries automatically purged on lazyOpen() based on retention setting
 */

#include "databasemanager.h"
#include <QCoreApplication>
#include <QDir>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QFile>
#include <QTextStream>
#include "logger.h"
#include "settings.h"

DatabaseManager::DatabaseManager(const Settings& settings, QObject *parent)
    : QObject(parent), history_days_to_keep_(std::max(settings.getHistoryDays(), 0)), settings_(settings)
{
    // Use unique connection name to avoid conflicts with multiple instances
    QString connectionName = QString("uTimer_connection_%1").arg(reinterpret_cast<quintptr>(this));
    db = QSqlDatabase::addDatabase("QSQLITE", connectionName);
    
    // Use executable directory for portability
    db.setDatabaseName(QDir(QCoreApplication::applicationDirPath()).filePath("uTimer.sqlite"));

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

/**
 * Initializes the database connection and ensures schema integrity.
 *
 * Tasks:
 * 1. Opens the SQLite file (creates it if missing).
 * 2. Creates the 'durations' table if it doesn't exist.
 * 3. Creates necessary indices for performance.
 * 4. Performs auto-maintenance: Deletes entries older than history_days_to_keep_.
 *
 * Returns true if the database is ready for use.
 */
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

/**
 * Creates a safety copy of the database before critical write operations.
 *
 * Why:
 * SQLite databases can occasionally become corrupted during write operations,
 * especially if the system crashes (power loss) during a transaction.
 * Since this app tracks user history, data loss is unacceptable.
 *
 * Strategy:
 * - Copies the .sqlite file to a timestamped .backup file.
 * - Writes a human-readable text dump (.durations.txt) for verification.
 * - This runs before 'saveDurations' (major save) but not 'saveCheckpoint' (frequent).
 */
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

/**
 * Persists a batch of duration entries to the database.
 *
 * Modes:
 * - Append: Adds new entries to the end (standard usage).
 * - Replace: Deletes ALL existing entries and writes the new set (used by HistoryDialog).
 *
 * Safety:
 * - Wraps the operation in a SQL transaction.
 * - Creates a file-level backup before starting.
 * - Rolls back the transaction on any error.
 */
bool DatabaseManager::saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode)
{
    // If history storage is disabled, treat as success (no-op)
    if (history_days_to_keep_ == 0) {
        return true;
    }

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
        lazyClose();
        return false;
    }

    // Replace mode: clear existing entries before inserting new ones
    if (mode == TransactionMode::Replace) {
        QSqlQuery clearQuery(db);
        if (!clearQuery.exec("DELETE FROM durations")) {
            db.rollback();
            if (settings_.logToFile())
                Logger::Log("[DB] Error clearing durations table: " + clearQuery.lastError().text());
            lazyClose();
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
            lazyClose();
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

/**
 * Retrieves the entire history of durations from the database.
 *
 * - Returns entries sorted by End Date/Time (chronological).
 * - Validates each row:
 *   - Type must be valid enum (Activity/Pause).
 *   - Duration must be non-negative.
 *   - Corrupted/Invalid rows are skipped and logged.
 */
std::deque<TimeDuration> DatabaseManager::loadDurations()
{
    std::deque<TimeDuration> durations;

    if (!lazyOpen()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Could not lazy open DB to load Durations");
        }
        return durations;
    }

    // Start read transaction for consistent snapshot
    if (!db.transaction()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error starting read transaction: " + db.lastError().text());
        }
        lazyClose();
        return durations;
    }

    // Load all durations ordered by end date and time
    QSqlQuery query(db);
    query.prepare("SELECT type, duration, end_date, end_time FROM durations ORDER BY end_date, end_time");
    if (!query.exec()) {
        db.rollback();
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

    db.commit();  // Commit read transaction
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

/**
 * Saves or updates a checkpoint entry for crash recovery.
 *
 * Unlike saveDurations(), this method:
 * - Does NOT create backups (for performance - called every 5 minutes)
 * - Uses checkpointId to update existing row instead of inserting new one
 * - Sets checkpointId to the new row ID on first call (when checkpointId == -1)
 *
 * The caller (TimeTracker) resets checkpointId to -1 on mode changes,
 * causing the next checkpoint to create a new row for the new segment.
 */
bool DatabaseManager::saveCheckpoint(DurationType type, qint64 duration, const QDateTime& endTime, long long& checkpointId)
{
    // If history storage is disabled, treat as success (no-op)
    if (history_days_to_keep_ == 0) {
        return true;
    }

    if (!lazyOpen()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Could not lazy open DB to save checkpoint");
        }
        return false;
    }

    QString endDateStr = endTime.date().toString(Qt::ISODate);
    QString endTimeStr = endTime.time().toString("HH:mm:ss.zzz");

    if (!db.transaction()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error starting transaction for checkpoint: " + db.lastError().text());
        }
        lazyClose();
        return false;
    }

    bool success = false;
    QSqlQuery query(db);

    if (checkpointId != -1) {
        // Update existing entry using known ID
        query.prepare(
            "UPDATE durations SET duration = :duration, end_date = :end_date, end_time = :end_time "
            "WHERE id = :id"
        );
        query.bindValue(":duration", duration);
        query.bindValue(":end_date", endDateStr);
        query.bindValue(":end_time", endTimeStr);
        query.bindValue(":id", checkpointId);

        if (query.exec()) {
            if (query.numRowsAffected() > 0) {
                success = db.commit();
                if (!success && settings_.logToFile()) {
                    Logger::Log("[DB] Error committing checkpoint update: " + db.lastError().text());
                }
            } else {
                // Row was deleted (retention cleanup or manual edit). Insert a new checkpoint.
                QSqlQuery insertQuery(db);
                insertQuery.prepare(
                    "INSERT INTO durations (type, duration, end_date, end_time) "
                    "VALUES (:type, :duration, :end_date, :end_time)"
                );
                insertQuery.bindValue(":type", static_cast<int>(type));
                insertQuery.bindValue(":duration", duration);
                insertQuery.bindValue(":end_date", endDateStr);
                insertQuery.bindValue(":end_time", endTimeStr);

                if (insertQuery.exec()) {
                    checkpointId = insertQuery.lastInsertId().toLongLong();
                    success = db.commit();
                    if (!success && settings_.logToFile()) {
                        Logger::Log("[DB] Error committing checkpoint insert after missing row: " + db.lastError().text());
                    }
                } else {
                    db.rollback();
                    if (settings_.logToFile()) {
                        Logger::Log("[DB] Error inserting checkpoint after missing row: " + insertQuery.lastError().text());
                    }
                    lazyClose();
                    return false;
                }
            }
        } else {
            db.rollback();
            if (settings_.logToFile()) {
                Logger::Log("[DB] Error updating checkpoint: " + query.lastError().text());
            }
            lazyClose();
            return false;
        }
    } else {
        // Append new checkpoint entry
        query.prepare(
            "INSERT INTO durations (type, duration, end_date, end_time) "
            "VALUES (:type, :duration, :end_date, :end_time)"
        );
        query.bindValue(":type", static_cast<int>(type));
        query.bindValue(":duration", duration);
        query.bindValue(":end_date", endDateStr);
        query.bindValue(":end_time", endTimeStr);

        if (query.exec()) {
            checkpointId = query.lastInsertId().toLongLong(); // Capture the new ID
            success = db.commit();
            if (!success && settings_.logToFile()) {
                Logger::Log("[DB] Error committing checkpoint insert: " + db.lastError().text());
            }
        } else {
            db.rollback();
            if (settings_.logToFile()) {
                Logger::Log("[DB] Error inserting checkpoint: " + query.lastError().text());
            }
            lazyClose();
            return false;
        }
    }

    lazyClose();
    return success;
}

/**
 * Smart upsert (update or insert) for a batch of durations.
 *
 * Unlike saveDurations(Replace), this method preserves existing IDs where possible.
 * It matches in-memory objects to database rows using their Calculated Start Time.
 *
 * Use Case:
 * - When the timer stops or pauses, we want to update the "current" session in the DB
 *   without deleting and re-inserting it (which would change IDs and churn the DB).
 * - Also handles cases where new entries were added to the list.
 */
bool DatabaseManager::updateDurationsByStartTime(const std::deque<TimeDuration>& durations)
{
    if (durations.empty()) {
        return true;
    }

    // If history storage is disabled, treat as success (no-op)
    if (history_days_to_keep_ == 0) {
        return true;
    }

    if (!lazyOpen()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Could not lazy open DB to update durations");
        }
        return false;
    }

    if (!db.transaction()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error starting transaction for updating durations: " + db.lastError().text());
        }
        lazyClose();
        return false;
    }

    QSqlQuery updateQuery(db);
    updateQuery.prepare(
        "UPDATE durations SET duration = :duration, end_date = :end_date, end_time = :end_time "
        "WHERE id = :id"
    );

    QSqlQuery insertQuery(db);
    insertQuery.prepare(
        "INSERT INTO durations (type, duration, end_date, end_time) "
        "VALUES (:type, :duration, :end_date, :end_time)"
    );

    int updatedCount = 0;
    int insertedCount = 0;

    for (const auto& d : durations) {
        int existingId = -1;
        bool found = updateDurationByStartTime(d.type, d.duration, d.endTime, existingId);

        if (found && existingId >= 0) {
            // Update existing entry
            updateQuery.bindValue(":duration", d.duration);
            updateQuery.bindValue(":end_date", d.endTime.date().toString(Qt::ISODate));
            updateQuery.bindValue(":end_time", d.endTime.time().toString("HH:mm:ss.zzz"));
            updateQuery.bindValue(":id", existingId);

            if (!updateQuery.exec()) {
                db.rollback();
                if (settings_.logToFile()) {
                    Logger::Log("[DB] Error updating duration: " + updateQuery.lastError().text());
                }
                lazyClose();
                return false;
            }
            updatedCount++;
        } else {
            // Insert new entry
            insertQuery.bindValue(":type", static_cast<int>(d.type));
            insertQuery.bindValue(":duration", d.duration);
            insertQuery.bindValue(":end_date", d.endTime.date().toString(Qt::ISODate));
            insertQuery.bindValue(":end_time", d.endTime.time().toString("HH:mm:ss.zzz"));

            if (!insertQuery.exec()) {
                db.rollback();
                if (settings_.logToFile()) {
                    Logger::Log("[DB] Error inserting duration: " + insertQuery.lastError().text());
                }
                lazyClose();
                return false;
            }
            insertedCount++;
        }
    }

    bool success = db.commit();
    if (success && settings_.logToFile()) {
        Logger::Log(QString("[DB] Updated durations: %1 updated, %2 inserted").arg(updatedCount).arg(insertedCount));
    } else if (!success && settings_.logToFile()) {
        Logger::Log("[DB] Error committing update transaction: " + db.lastError().text());
    }

    lazyClose();
    return success;
}

/**
 * Helper to find a database entry that matches the given duration's start time.
 *
 * Logic:
 * - Start Time = End Time - Duration
 * - Because of slight timing variations (milliseconds) between DB saves and memory,
 *   we use a tolerance window (2 seconds) for matching.
 * - Searches only recent history (yesterday + today) for performance.
 *
 * Returns true and sets existingId if a match is found.
 */
bool DatabaseManager::updateDurationByStartTime(DurationType type, qint64 duration, const QDateTime& endTime, int& existingId)
{
    // Calculate the start time for this duration
    QDateTime startTime = endTime.addMSecs(-duration);
    int typeInt = static_cast<int>(type);
    const qint64 toleranceMs = 2000; // 2 seconds tolerance for start time matching
    
    existingId = -1;
    
    // Find entries with the same start time (within tolerance) and same type
    // Only check recent entries (yesterday and today) for performance
    QDate minDate = QDate::currentDate().addDays(-1);

    QSqlQuery checkQuery(db);
    checkQuery.prepare(
        "SELECT id, duration, end_date, end_time FROM durations "
        "WHERE type = :type AND end_date >= :min_date "
        "ORDER BY end_date DESC, end_time DESC"
    );
    checkQuery.bindValue(":type", typeInt);
    checkQuery.bindValue(":min_date", minDate.toString(Qt::ISODate));

    if (checkQuery.exec()) {
        while (checkQuery.next()) {
            qint64 existingDuration = checkQuery.value(1).toLongLong();
            QDate existingEndDate = QDate::fromString(checkQuery.value(2).toString(), Qt::ISODate);
            QTime existingEndTime = QTime::fromString(checkQuery.value(3).toString(), "HH:mm:ss.zzz");
            QDateTime existingEndDateTime(existingEndDate, existingEndTime);
            
            // Calculate start time of existing entry
            QDateTime existingStartTime = existingEndDateTime.addMSecs(-existingDuration);
            
            // Check if start times match (within tolerance)
            qint64 startTimeDiff = qAbs(existingStartTime.msecsTo(startTime));
            if (startTimeDiff <= toleranceMs) {
                existingId = checkQuery.value(0).toInt();
                return true; // Found matching start time
            }
        }
    }
    
    return false; // No matching entry found
}
