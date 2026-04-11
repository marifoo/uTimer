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

namespace {
constexpr qint64 kDurationReconciliationToleranceMs = 100;
const char* kLastCleanShutdownKey = "last_clean_shutdown";
}

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
    
    QString connName = db.connectionName();
    db = QSqlDatabase();
    
    if (QSqlDatabase::contains(connName)) {
        QSqlDatabase::removeDatabase(connName);
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
        "start_date DATE NOT NULL," // Date when the duration started
        "start_time TEXT NOT NULL," // Time when the duration started (UTC, TIME(3) precision)
        "end_date DATE NOT NULL," // Date when the duration ended
        "end_time TEXT NOT NULL," // Time when the duration ended (UTC, TIME(3) precision)
        "is_finalized INTEGER NOT NULL DEFAULT 0,"
        "UNIQUE(start_date, start_time, type) ON CONFLICT REPLACE" // Prevent duplicate entries
        ")"
    );

    if (!tableCreated) {
        if (settings_.logToFile())
            Logger::Log("[DB] Error creating table: " + query_new.lastError().text());
        db.close();
        return false;
    }

    // Keep schema forward-compatible: older databases do not have is_finalized.
    // We migrate all existing rows to finalized=1 because checkpoints from older
    // versions are indistinguishable from completed entries.
    if (!ensureIsFinalizedColumn()) {
        db.close();
        return false;
    }

    // Keep a tiny key/value table for lifecycle markers (e.g. clean shutdown).
    if (!ensureSettingsTable()) {
        db.close();
        return false;
    }

    // Validate schema - ensure start_date and start_time columns exist
    if (!validateSchema()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] CRITICAL: Schema validation failed - database is outdated");
        }
        db.close();
        return false;
    }

    // Create index for date-based queries (cleanup and hasEntriesForDate)
    QSqlQuery indexQuery(db);
    if (!indexQuery.exec("CREATE INDEX IF NOT EXISTS idx_end_date ON durations(end_date)")) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Warning: Failed to create end_date index: " + indexQuery.lastError().text());
        }
        // Non-fatal: continue even if index creation fails
    }

    // Create composite index for start time queries
    QSqlQuery startIndexQuery(db);
    if (!startIndexQuery.exec("CREATE INDEX IF NOT EXISTS idx_start_datetime ON durations(start_date, start_time, type)")) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Warning: Failed to create start_datetime index: " + startIndexQuery.lastError().text());
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
 * Public method to check schema validity on application startup.
 *
 * This should be called before any other database operations to ensure
 * the database schema is compatible with the current version. If the
 * database file exists but has an outdated schema, returns false so
 * the application can show an appropriate error message.
 *
 * Returns true if:
 * - History storage is disabled (no DB needed)
 * - Database file doesn't exist (will be created fresh)
 * - Database schema is valid
 *
 * Returns false if:
 * - Database exists but schema is outdated
 */
bool DatabaseManager::checkSchemaOnStartup()
{
    // If history storage is disabled, no need to check
    if (history_days_to_keep_ == 0) {
        return true;
    }

    // If database file doesn't exist, it will be created fresh with correct schema
    if (!QFile::exists(db.databaseName())) {
        return true;
    }

    // Open database to check schema
    if (!db.open()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error opening database for schema check: " + db.lastError().text());
        }
        return false;
    }

    bool valid = validateSchema();
    db.close();

    return valid;
}

/**
 * Validates that the database schema contains required columns.
 *
 * This function checks for the presence of start_date and start_time columns
 * which are required for the explicit start time feature. If these columns
 * are missing, the database is considered outdated and needs to be recreated.
 *
 * Returns true if schema is valid, false if outdated.
 */
bool DatabaseManager::validateSchema()
{
    QSqlQuery query(db);
    if (!query.exec("PRAGMA table_info(durations)")) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error checking table schema: " + query.lastError().text());
        }
        return false;
    }

    bool hasStartDate = false;
    bool hasStartTime = false;

    while (query.next()) {
        QString columnName = query.value(1).toString();
        if (columnName == "start_date") {
            hasStartDate = true;
        } else if (columnName == "start_time") {
            hasStartTime = true;
        }
    }

    if (!hasStartDate || !hasStartTime) {
        if (settings_.logToFile()) {
            Logger::Log(QString("[DB] Schema validation failed: start_date=%1, start_time=%2")
                .arg(hasStartDate ? "present" : "MISSING")
                .arg(hasStartTime ? "present" : "MISSING"));
        }
        return false;
    }

    return true;
}

bool DatabaseManager::ensureIsFinalizedColumn()
{
    QSqlQuery tableInfo(db);
    if (!tableInfo.exec("PRAGMA table_info(durations)")) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error checking table_info for is_finalized migration: " + tableInfo.lastError().text());
        }
        return false;
    }

    bool hasIsFinalized = false;
    while (tableInfo.next()) {
        if (tableInfo.value(1).toString() == "is_finalized") {
            hasIsFinalized = true;
            break;
        }
    }

    if (hasIsFinalized) {
        return true;
    }

    if (settings_.logToFile()) {
        Logger::Log("[DB] Migrating schema: adding durations.is_finalized and marking existing rows as finalized");
    }

    QSqlQuery alterQuery(db);
    if (!alterQuery.exec("ALTER TABLE durations ADD COLUMN is_finalized INTEGER NOT NULL DEFAULT 0")) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error adding is_finalized column: " + alterQuery.lastError().text());
        }
        return false;
    }

    QSqlQuery migrateQuery(db);
    if (!migrateQuery.exec("UPDATE durations SET is_finalized = 1")) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error finalizing migrated rows: " + migrateQuery.lastError().text());
        }
        return false;
    }

    return true;
}

bool DatabaseManager::ensureSettingsTable()
{
    QSqlQuery query(db);
    if (!query.exec(
            "CREATE TABLE IF NOT EXISTS app_settings ("
            "key TEXT PRIMARY KEY,"
            "value TEXT NOT NULL"
            ")")) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error creating app_settings table: " + query.lastError().text());
        }
        return false;
    }

    return true;
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
            out << "Start Date: " << d.startTime.toUTC().date().toString(Qt::ISODate) << " | ";
            out << "Start Time: " << d.startTime.toUTC().time().toString("HH:mm:ss.zzz") << " | ";
            out << "End Date: " << d.endTime.toUTC().date().toString(Qt::ISODate) << " | ";
            out << "End Time: " << d.endTime.toUTC().time().toString("HH:mm:ss.zzz") << "\n";
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

    // Prepare insert statement for batch insertion (storing times in UTC)
    QSqlQuery query(db);
    query.prepare("INSERT INTO durations (type, duration, start_date, start_time, end_date, end_time, is_finalized) "
                  "VALUES (:type, :duration, :start_date, :start_time, :end_date, :end_time, 1)");

    // Insert each duration entry (convert to UTC for storage)
    for (const auto& d : durations) {
        QDateTime startUtc = d.startTime.toUTC();
        QDateTime endUtc = d.endTime.toUTC();

        query.bindValue(":type", static_cast<int>(d.type));
        query.bindValue(":duration", d.duration);
        query.bindValue(":start_date", startUtc.date().toString(Qt::ISODate));
        query.bindValue(":start_time", startUtc.time().toString("HH:mm:ss.zzz"));
        query.bindValue(":end_date", endUtc.date().toString(Qt::ISODate));
        query.bindValue(":end_time", endUtc.time().toString("HH:mm:ss.zzz"));

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

bool DatabaseManager::replaceDurationsInDB(const std::deque<TimeDuration>& historyDurations,
                                           const std::deque<TimeDuration>& currentSessionDurations)
{
    if (history_days_to_keep_ == 0) {
        return true;
    }

    std::deque<TimeDuration> allDurations;
    allDurations.insert(allDurations.end(), historyDurations.begin(), historyDurations.end());
    allDurations.insert(allDurations.end(), currentSessionDurations.begin(), currentSessionDurations.end());

    if (!createBackup(allDurations, TransactionMode::Replace)) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Warning: Backup failed before REPLACE operation - proceeding without backup");
        }
    }

    if (!lazyOpen()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Could not lazy open DB to replace durations");
        }
        return false;
    }

    if (!db.transaction()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error starting transaction for replace: " + db.lastError().text());
        }
        lazyClose();
        return false;
    }

    QSqlQuery clearQuery(db);
    if (!clearQuery.exec("DELETE FROM durations")) {
        db.rollback();
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error clearing durations table: " + clearQuery.lastError().text());
        }
        lazyClose();
        return false;
    }

    QSqlQuery finalizedInsert(db);
    finalizedInsert.prepare(
        "INSERT INTO durations (type, duration, start_date, start_time, end_date, end_time, is_finalized) "
        "VALUES (:type, :duration, :start_date, :start_time, :end_date, :end_time, 1)"
    );

    for (const auto& d : historyDurations) {
        const QDateTime startUtc = d.startTime.toUTC();
        const QDateTime endUtc = d.endTime.toUTC();

        finalizedInsert.bindValue(":type", static_cast<int>(d.type));
        finalizedInsert.bindValue(":duration", d.duration);
        finalizedInsert.bindValue(":start_date", startUtc.date().toString(Qt::ISODate));
        finalizedInsert.bindValue(":start_time", startUtc.time().toString("HH:mm:ss.zzz"));
        finalizedInsert.bindValue(":end_date", endUtc.date().toString(Qt::ISODate));
        finalizedInsert.bindValue(":end_time", endUtc.time().toString("HH:mm:ss.zzz"));

        if (!finalizedInsert.exec()) {
            db.rollback();
            if (settings_.logToFile()) {
                Logger::Log("[DB] Error inserting finalized duration: " + finalizedInsert.lastError().text());
            }
            lazyClose();
            return false;
        }
    }

    QSqlQuery unfinalizedInsert(db);
    unfinalizedInsert.prepare(
        "INSERT INTO durations (type, duration, start_date, start_time, end_date, end_time, is_finalized) "
        "VALUES (:type, :duration, :start_date, :start_time, :end_date, :end_time, 0)"
    );

    for (const auto& d : currentSessionDurations) {
        const QDateTime startUtc = d.startTime.toUTC();
        const QDateTime endUtc = d.endTime.toUTC();

        unfinalizedInsert.bindValue(":type", static_cast<int>(d.type));
        unfinalizedInsert.bindValue(":duration", d.duration);
        unfinalizedInsert.bindValue(":start_date", startUtc.date().toString(Qt::ISODate));
        unfinalizedInsert.bindValue(":start_time", startUtc.time().toString("HH:mm:ss.zzz"));
        unfinalizedInsert.bindValue(":end_date", endUtc.date().toString(Qt::ISODate));
        unfinalizedInsert.bindValue(":end_time", endUtc.time().toString("HH:mm:ss.zzz"));

        if (!unfinalizedInsert.exec()) {
            db.rollback();
            if (settings_.logToFile()) {
                Logger::Log("[DB] Error inserting current-session duration: " + unfinalizedInsert.lastError().text());
            }
            lazyClose();
            return false;
        }
    }

    const bool success = db.commit();
    if (!success && settings_.logToFile()) {
        Logger::Log("[DB] Error committing replace transaction: " + db.lastError().text());
    }

    lazyClose();
    return success;
}

/**
 * Retrieves the entire history of durations from the database.
 *
 * - Returns entries sorted by Start Date/Time (chronological).
 * - Validates each row:
 *   - Type must be valid enum (Activity/Pause).
 *   - Start must be <= End.
 *   - Duration must be non-negative.
 *   - Computed duration must match stored duration within reconciliation tolerance.
 *   - Corrupted/Invalid rows are skipped and logged.
 * - Timestamps are stored in UTC and converted to local time on load.
 */
DatabaseManager::LoadResult DatabaseManager::loadDurations()
{
    LoadResult result;

    if (!lazyOpen()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Could not lazy open DB to load Durations");
        }
        return result;
    }

    // Start read transaction for consistent snapshot
    if (!db.transaction()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error starting read transaction: " + db.lastError().text());
        }
        lazyClose();
        return result;
    }

    // Load all durations ordered by start date and time
    QSqlQuery query(db);
    query.prepare("SELECT type, duration, start_date, start_time, end_date, end_time FROM durations WHERE is_finalized = 1 ORDER BY start_date, start_time");
    if (!query.exec()) {
        db.rollback();
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error executing load query: " + query.lastError().text());
        }
        lazyClose();
        return result;
    }

    // Parse each row and reconstruct TimeDuration objects
    while (query.next()) {
        int typeInt = query.value(0).toInt();
        qint64 storedDuration = query.value(1).toLongLong();
        QDate startDate = QDate::fromString(query.value(2).toString(), Qt::ISODate);
        QTime startTime = QTime::fromString(query.value(3).toString(), "HH:mm:ss.zzz");
        QDate endDate = QDate::fromString(query.value(4).toString(), Qt::ISODate);
        QTime endTime = QTime::fromString(query.value(5).toString(), "HH:mm:ss.zzz");

        // Reconstruct QDateTime in UTC, then convert to local time
        QDateTime startDateTime(startDate, startTime, Qt::UTC);
        QDateTime endDateTime(endDate, endTime, Qt::UTC);
        startDateTime = startDateTime.toLocalTime();
        endDateTime = endDateTime.toLocalTime();

        // Validate type enum range
        if (typeInt != static_cast<int>(DurationType::Activity) &&
            typeInt != static_cast<int>(DurationType::Pause)) {
            if (settings_.logToFile()) {
                Logger::Log(QString("[DB] Warning: Invalid type value %1, skipping entry").arg(typeInt));
            }
            result.skipped++;
            continue;
        }

        DurationType type = static_cast<DurationType>(typeInt);

        // Validate timestamps
        if (!startDate.isValid() || !startTime.isValid() || !endDate.isValid() || !endTime.isValid()) {
            if (settings_.logToFile()) {
                Logger::Log(QString("[DB] Warning: Skipped invalid timestamp entry - StartDate: %1, StartTime: %2, EndDate: %3, EndTime: %4")
                    .arg(startDate.toString()).arg(startTime.toString()).arg(endDate.toString()).arg(endTime.toString()));
            }
            result.skipped++;
            continue;
        }

        // Validate start <= end
        if (startDateTime > endDateTime) {
            if (settings_.logToFile()) {
                Logger::Log(QString("[DB] Warning: Skipped entry with start > end - Start: %1, End: %2")
                    .arg(startDateTime.toString(Qt::ISODate)).arg(endDateTime.toString(Qt::ISODate)));
            }
            result.skipped++;
            continue;
        }

        // Compute duration from timestamps and validate against stored duration
        qint64 computedDuration = startDateTime.msecsTo(endDateTime);

        if (storedDuration < 0) {
            if (settings_.logToFile()) {
                Logger::Log(QString("[DB] Warning: Negative stored duration %1ms - using computed duration %2ms")
                    .arg(storedDuration).arg(computedDuration));
            }
            result.repaired++;
        } else if (qAbs(computedDuration - storedDuration) > kDurationReconciliationToleranceMs) {
            if (settings_.logToFile()) {
                Logger::Log(QString("[DB] Warning: Duration mismatch (stored: %1ms, computed: %2ms) - using computed value")
                    .arg(storedDuration).arg(computedDuration));
            }
            result.repaired++;
        }

        // Create TimeDuration with explicit start/end times
        result.durations.emplace_back(TimeDuration(type, startDateTime, endDateTime));
    }

    db.commit();  // Commit read transaction
    lazyClose();

    if ((result.skipped > 0 || result.repaired > 0) && settings_.logToFile()) {
        Logger::Log(QString("[DB] loadDurations reconciliation summary: skipped=%1, repaired=%2")
            .arg(result.skipped)
            .arg(result.repaired));
    }

    return result;
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
    query.prepare("SELECT COUNT(*) FROM durations WHERE end_date = :date AND is_finalized = 1");
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
 * - Stores the actual segment_start_time_ (preserves original start, only updates end/duration)
 *
 * The caller (TimeTracker) resets checkpointId to -1 on mode changes,
 * causing the next checkpoint to create a new row for the new segment.
 */
bool DatabaseManager::saveCheckpoint(DurationType type, qint64 duration, const QDateTime& startTime, const QDateTime& endTime, long long& checkpointId)
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

    // Convert to UTC for storage
    QDateTime startUtc = startTime.toUTC();
    QDateTime endUtc = endTime.toUTC();

    QString startDateStr = startUtc.date().toString(Qt::ISODate);
    QString startTimeStr = startUtc.time().toString("HH:mm:ss.zzz");
    QString endDateStr = endUtc.date().toString(Qt::ISODate);
    QString endTimeStr = endUtc.time().toString("HH:mm:ss.zzz");

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
        // Update existing entry using known ID (preserve start time, only update end/duration)
        query.prepare(
            "UPDATE durations SET duration = :duration, end_date = :end_date, end_time = :end_time, is_finalized = 0 "
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
                    "INSERT INTO durations (type, duration, start_date, start_time, end_date, end_time, is_finalized) "
                    "VALUES (:type, :duration, :start_date, :start_time, :end_date, :end_time, 0)"
                );
                insertQuery.bindValue(":type", static_cast<int>(type));
                insertQuery.bindValue(":duration", duration);
                insertQuery.bindValue(":start_date", startDateStr);
                insertQuery.bindValue(":start_time", startTimeStr);
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
        // Append new checkpoint entry with start time
        query.prepare(
            "INSERT INTO durations (type, duration, start_date, start_time, end_date, end_time, is_finalized) "
            "VALUES (:type, :duration, :start_date, :start_time, :end_date, :end_time, 0)"
        );
        query.bindValue(":type", static_cast<int>(type));
        query.bindValue(":duration", duration);
        query.bindValue(":start_date", startDateStr);
        query.bindValue(":start_time", startTimeStr);
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
 * Smart upsert (update or insert) for a batch of durations using exact start time matching.
 *
 * This method uses the UNIQUE constraint on (start_date, start_time, type) for
 * conflict resolution. When a duration with matching start time/type already exists,
 * the INSERT OR REPLACE will update the existing row.
 *
 * Use Case:
 * - When the timer stops or pauses, we want to update the "current" session in the DB
 *   without creating duplicates.
 * - The UNIQUE constraint ensures no ambiguous matching is needed.
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

    // Use INSERT OR REPLACE which leverages the UNIQUE constraint on (start_date, start_time, type)
    // If a row with matching start_date, start_time, and type exists, it will be replaced
    QSqlQuery insertQuery(db);
    insertQuery.prepare(
        "INSERT OR REPLACE INTO durations (type, duration, start_date, start_time, end_date, end_time, is_finalized) "
        "VALUES (:type, :duration, :start_date, :start_time, :end_date, :end_time, 1)"
    );

    int count = 0;

    for (const auto& d : durations) {
        // Convert to UTC for storage
        QDateTime startUtc = d.startTime.toUTC();
        QDateTime endUtc = d.endTime.toUTC();

        insertQuery.bindValue(":type", static_cast<int>(d.type));
        insertQuery.bindValue(":duration", d.duration);
        insertQuery.bindValue(":start_date", startUtc.date().toString(Qt::ISODate));
        insertQuery.bindValue(":start_time", startUtc.time().toString("HH:mm:ss.zzz"));
        insertQuery.bindValue(":end_date", endUtc.date().toString(Qt::ISODate));
        insertQuery.bindValue(":end_time", endUtc.time().toString("HH:mm:ss.zzz"));

        if (!insertQuery.exec()) {
            db.rollback();
            if (settings_.logToFile()) {
                Logger::Log("[DB] Error upserting duration: " + insertQuery.lastError().text());
            }
            lazyClose();
            return false;
        }
        count++;
    }

    bool success = db.commit();
    if (success && settings_.logToFile()) {
        Logger::Log(QString("[DB] Upserted %1 durations").arg(count));
    } else if (!success && settings_.logToFile()) {
        Logger::Log("[DB] Error committing update transaction: " + db.lastError().text());
    }

    lazyClose();
    return success;
}

/**
 * Forces any pending database writes to be physically written to disk.
 *
 * This is critical during Windows shutdown to ensure data is persisted
 * before the process is terminated. SQLite may buffer writes even after
 * commit() returns - this method ensures they're flushed.
 *
 * Uses WAL checkpoint if WAL mode is active, otherwise uses PRAGMA synchronous.
 */
void DatabaseManager::flushToDisc()
{
    if (history_days_to_keep_ == 0) {
        return;
    }

    if (!lazyOpen()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Could not open DB to flush to disc");
        }
        return;
    }

    QSqlQuery query(db);

    // Check if WAL mode is being used
    if (query.exec("PRAGMA journal_mode") && query.next()) {
        QString mode = query.value(0).toString().toLower();
        if (mode == "wal") {
            // Force WAL checkpoint - this writes all pending WAL changes to the main DB file
            if (query.exec("PRAGMA wal_checkpoint(FULL)")) {
                if (settings_.logToFile()) {
                    Logger::Log("[DB] WAL checkpoint completed");
                }
            } else if (settings_.logToFile()) {
                Logger::Log("[DB] WAL checkpoint failed: " + query.lastError().text());
            }
        }
    }

    // Additionally ensure synchronous writes are fully flushed
    // FULL mode waits for data to be physically written to disk
    if (!query.exec("PRAGMA synchronous=FULL")) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Failed to set synchronous mode: " + query.lastError().text());
        }
    }

    // Execute a no-op write to trigger the synchronous flush
    // This ensures the PRAGMA takes effect
    query.exec("SELECT 1");

    lazyClose();

    if (settings_.logToFile()) {
        Logger::Log("[DB] Flush to disc completed");
    }
}

std::deque<DatabaseManager::OrphanCheckpoint> DatabaseManager::loadUnfinalizedCheckpoints()
{
    std::deque<OrphanCheckpoint> orphans;

    if (!lazyOpen()) {
        return orphans;
    }

    QSqlQuery query(db);
    query.prepare("SELECT id, type, duration, start_date, start_time, end_date, end_time FROM durations WHERE is_finalized = 0 ORDER BY id ASC");
    if (!query.exec()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error loading orphan checkpoints: " + query.lastError().text());
        }
        lazyClose();
        return orphans;
    }

    while (query.next()) {
        int typeInt = query.value(1).toInt();
        if (typeInt != static_cast<int>(DurationType::Activity) &&
            typeInt != static_cast<int>(DurationType::Pause)) {
            continue;
        }

        QDate startDate = QDate::fromString(query.value(3).toString(), Qt::ISODate);
        QTime startTime = QTime::fromString(query.value(4).toString(), "HH:mm:ss.zzz");
        QDate endDate = QDate::fromString(query.value(5).toString(), Qt::ISODate);
        QTime endTime = QTime::fromString(query.value(6).toString(), "HH:mm:ss.zzz");
        if (!startDate.isValid() || !startTime.isValid() || !endDate.isValid() || !endTime.isValid()) {
            continue;
        }

        OrphanCheckpoint checkpoint;
        checkpoint.id = query.value(0).toLongLong();
        checkpoint.type = static_cast<DurationType>(typeInt);
        checkpoint.duration = query.value(2).toLongLong();
        checkpoint.startTime = QDateTime(startDate, startTime, Qt::UTC).toLocalTime();
        checkpoint.endTime = QDateTime(endDate, endTime, Qt::UTC).toLocalTime();
        orphans.push_back(checkpoint);
    }

    lazyClose();
    return orphans;
}

bool DatabaseManager::reconcileUnfinalizedCheckpoints(const std::vector<long long>& finalizeIds, const std::vector<long long>& dropIds)
{
    if (finalizeIds.empty() && dropIds.empty()) {
        return true;
    }

    if (!lazyOpen()) {
        return false;
    }

    if (!db.transaction()) {
        if (settings_.logToFile()) {
            Logger::Log("[DB] Error starting orphan reconciliation transaction: " + db.lastError().text());
        }
        lazyClose();
        return false;
    }

    QSqlQuery finalizeQuery(db);
    finalizeQuery.prepare("UPDATE durations SET is_finalized = 1 WHERE id = :id AND is_finalized = 0");
    for (long long id : finalizeIds) {
        finalizeQuery.bindValue(":id", id);
        if (!finalizeQuery.exec()) {
            db.rollback();
            lazyClose();
            return false;
        }
    }

    QSqlQuery dropQuery(db);
    dropQuery.prepare("DELETE FROM durations WHERE id = :id AND is_finalized = 0");
    for (long long id : dropIds) {
        dropQuery.bindValue(":id", id);
        if (!dropQuery.exec()) {
            db.rollback();
            lazyClose();
            return false;
        }
    }

    const bool committed = db.commit();
    if (!committed) {
        db.rollback();
    }
    lazyClose();
    return committed;
}

bool DatabaseManager::setLastCleanShutdownMarker(const QDateTime& timestamp)
{
    if (history_days_to_keep_ == 0) {
        return true;
    }

    if (!lazyOpen()) {
        return false;
    }

    if (!db.transaction()) {
        lazyClose();
        return false;
    }

    const QString markerValue = timestamp.toUTC().toString(Qt::ISODateWithMs);
    QSqlQuery query(db);
    query.prepare("INSERT OR REPLACE INTO app_settings (key, value) VALUES (:key, :value)");
    query.bindValue(":key", QString::fromLatin1(kLastCleanShutdownKey));
    query.bindValue(":value", markerValue);

    if (!query.exec()) {
        db.rollback();
        lazyClose();
        return false;
    }

    const bool committed = db.commit();
    if (!committed) {
        db.rollback();
    }

    lazyClose();
    return committed;
}

std::optional<QDateTime> DatabaseManager::consumeLastCleanShutdownMarker()
{
    if (history_days_to_keep_ == 0) {
        return std::nullopt;
    }

    if (!lazyOpen()) {
        return std::nullopt;
    }

    if (!db.transaction()) {
        lazyClose();
        return std::nullopt;
    }

    std::optional<QDateTime> marker;
    QSqlQuery readQuery(db);
    readQuery.prepare("SELECT value FROM app_settings WHERE key = :key");
    readQuery.bindValue(":key", QString::fromLatin1(kLastCleanShutdownKey));
    if (readQuery.exec() && readQuery.next()) {
        const QString value = readQuery.value(0).toString();
        QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
        if (!parsed.isValid()) {
            parsed = QDateTime::fromString(value, Qt::ISODate);
        }
        if (parsed.isValid()) {
            marker = parsed.toLocalTime();
        }
    }

    QSqlQuery deleteQuery(db);
    deleteQuery.prepare("DELETE FROM app_settings WHERE key = :key");
    deleteQuery.bindValue(":key", QString::fromLatin1(kLastCleanShutdownKey));
    if (!deleteQuery.exec()) {
        db.rollback();
        lazyClose();
        return std::nullopt;
    }

    const bool committed = db.commit();
    if (!committed) {
        db.rollback();
        lazyClose();
        return std::nullopt;
    }

    lazyClose();
    return marker;
}
