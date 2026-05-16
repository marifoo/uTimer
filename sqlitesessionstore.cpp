/**
 * SqliteSessionStore - SQLite persistence for time duration data.
 *
 * Connection pattern:
 * - Long-lived: Database opened in the constructor, closed in the destructor.
 * - ensureOpen() is called at the start of every public method as a health
 *   check; if the connection was closed (e.g. by createBackup's copy window),
 *   it will be reopened transparently.
 *
 * Thread safety:
 * - A QRecursiveMutex (db_mutex_) guards every public entry point, preventing
 *   concurrent operations from interleaving.  This is essential because
 *   createBackup() temporarily closes and reopens the database connection;
 *   without the mutex, a concurrent call could hit a closed connection.
 *   QRecursiveMutex (rather than QMutex) is used because public methods
 *   call private helpers that are also called from other public methods.
 *
 * Save methods:
 * - saveDurations(): Full save with backup creation, used for stopTimer/HistoryDialog
 * - saveCheckpoint(): Lightweight update without backup, used for periodic checkpoints
 * - updateDurationsById(): Smart upsert matching entries by stable segment_id
 *
 * Backup strategy:
 * - saveDurations() creates timestamped .backup file + .durations.txt log before writes
 * - saveCheckpoint() skips backup for performance (called every 5 minutes)
 *
 * Configuration:
 * - history_days_to_keep_ = 0 disables database entirely (all methods return early)
 * - Old entries are purged once per startup via checkSchemaOnStartup() →
 *   performRetentionCleanup(), not in the constructor.
 */

#include "sqlitesessionstore.h"
#include <QCoreApplication>
#include <QDir>
#include <QMutexLocker>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTextStream>
#include <atomic>
#include "logger.h"

namespace {
const char* kLastCleanShutdownKey = "last_clean_shutdown";

// Monotonically increasing counter used to mint unique SQLite connection names.
// Using a counter instead of the object address (reinterpret_cast<quintptr>(this))
// avoids name collisions when a SqliteSessionStore is destroyed and a new one is
// allocated at the same address — which is perfectly legal and surprisingly common
// in loops or test harnesses.
std::atomic<uint64_t> s_connection_seq{0};
}

SqliteSessionStore::SqliteSessionStore(const Settings& settings, QObject *parent)
    : QObject(parent), history_days_to_keep_(std::max(settings.getHistoryDays(), 0))
{
    // Mint a unique connection name using a monotonic counter.
    // See s_connection_seq declaration for rationale (avoids address-reuse collisions).
    QString connectionName = QString("uTimer_connection_%1").arg(s_connection_seq.fetch_add(1, std::memory_order_relaxed));
    db = QSqlDatabase::addDatabase("QSQLITE", connectionName);

    // Use executable directory for portability
    db.setDatabaseName(QDir(QCoreApplication::applicationDirPath()).filePath("uTimer.sqlite"));

    if (history_days_to_keep_ == 0) {
        Logger::Log("[DB] History days to keep is set to 0, database will not be used.");
        return;
    }

    // Open the connection and run one-time schema setup.
    if (!db.open()) {
        Logger::Log("[DB] Error opening database: " + db.lastError().text());
        return;
    }
    // Reject read-only files immediately.  Without this check, db.open() can
    // succeed on a read-only SQLite file (opening in read-only mode), leaving
    // the connection open but unable to write — identical to the ensureOpen()
    // guard added in Step 6.
    if (!QFileInfo(db.databaseName()).isWritable()) {
        Logger::Log("[DB] Warning: Database file is not writable.");
        db.close();
        return;
    }
    if (!ensureSchema()) {
        db.close();
    }
}

SqliteSessionStore::~SqliteSessionStore()
{
    lazyClose();
    
    QString connName = db.connectionName();
    db = QSqlDatabase();
    
    if (QSqlDatabase::contains(connName)) {
        QSqlDatabase::removeDatabase(connName);
    }
}

/**
 * Lightweight connection health check.
 *
 * Returns true immediately if the connection is already open.  If the
 * connection was lost unexpectedly (e.g. after an OS-level file error),
 * reopens it and returns true.  Schema migrations, indexes, and PRAGMA are NOT
 * re-applied here — those run via ensureSchema() (called from the constructor
 * and idempotently from checkSchemaOnStartup()).
 *
 * After reopening, verifies the file is writable at the OS level.  SQLite can
 * open a read-only file in read-only mode (db.open() returns true), but all
 * subsequent write calls would fail with SQLITE_READONLY on the same connection.
 * Closing and returning false here forces the caller to treat it as a DB failure
 * and try again once write access is restored.  Note: isWritable() checks
 * permission bits only — it cannot detect immutable-bit or SELinux-denied writes.
 */
bool SqliteSessionStore::ensureOpen()
{
    if (history_days_to_keep_ == 0) {
        return false;
    }
    if (db.isOpen()) {
        return true;
    }
    if (!db.open()) {
        Logger::Log("[DB] Error reopening database: " + db.lastError().text());
        return false;
    }
    if (!QFileInfo(db.databaseName()).isWritable()) {
        db.close();
        return false;
    }
    return true;
}

/**
 * Idempotent schema setup: called from the constructor and checkSchemaOnStartup().
 *
 * Creates the durations table (if needed), runs forward-compatibility migrations
 * (creating a pre-migration backup before any table rebuild), creates indexes,
 * and sets PRAGMA synchronous=NORMAL.  All DDL uses IF NOT EXISTS so repeated
 * calls are safe.
 *
 * Journal mode note: this app uses SQLite's default rollback journal mode, NOT
 * WAL.  WAL's concurrent-readers advantage is irrelevant for a single-process
 * app.  Rollback journal keeps the database as a single file (no -wal/-shm
 * sidecars), which simplifies close-copy-reopen backups and portability.
 *
 * Returns false on schema/migration failures (caller closes the connection).
 * Non-fatal failures (index creation, PRAGMA) are logged and allowed through —
 * the DB is still usable for reads and writes.
 */
bool SqliteSessionStore::ensureSchema()
{
    // Create the durations table if it doesn't exist.
    // New installs get the clean schema directly; existing DBs go through
    // the migration chain (ensureIsFinalizedColumn → ensureSegmentIdColumn
    // → ensureUtcColumns → dropLegacyColumns).
    QSqlQuery query_new(db);
    bool tableCreated = query_new.exec(
        "CREATE TABLE IF NOT EXISTS durations ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "segment_id TEXT NOT NULL,"
        "type INTEGER NOT NULL,"
        "start_utc TEXT NOT NULL,"
        "end_utc TEXT NOT NULL,"
        "is_finalized INTEGER NOT NULL DEFAULT 0,"
        "UNIQUE(segment_id)"
        ")"
    );

    if (!tableCreated) {
        Logger::Log("[DB] Error creating table: " + query_new.lastError().text());
        return false;
    }

    if (!ensureIsFinalizedColumn()) {
        return false;
    }

    if (!ensureSegmentIdColumn()) {
        return false;
    }

    if (!ensureUtcColumns()) {
        return false;
    }

    if (!dropLegacyColumns()) {
        return false;
    }

    if (!ensureSettingsTable()) {
        return false;
    }

    if (!validateSchema()) {
        Logger::Log("[DB] CRITICAL: Schema validation failed - database is outdated");
        return false;
    }

    QSqlQuery segmentIndexQuery(db);
    if (!segmentIndexQuery.exec("CREATE INDEX IF NOT EXISTS idx_segment_id ON durations(segment_id)")) {
        Logger::Log("[DB] Warning: Failed to create segment_id index: " + segmentIndexQuery.lastError().text());
    }

    QSqlQuery startUtcIndexQuery(db);
    if (!startUtcIndexQuery.exec("CREATE INDEX IF NOT EXISTS idx_start_utc ON durations(start_utc) WHERE is_finalized = 1")) {
        Logger::Log("[DB] Warning: Failed to create start_utc index: " + startUtcIndexQuery.lastError().text());
    }

    // PRAGMA synchronous=NORMAL: in rollback journal mode this provides good
    // crash safety (only OS crash / power loss can lose data, not an app crash).
    // The shutdown path (flushToDisc) temporarily promotes to FULL for the
    // final durable write before the process exits.
    QSqlQuery syncQuery(db);
    if (!syncQuery.exec("PRAGMA synchronous=NORMAL")) {
        Logger::Log("[DB] Warning: Failed to set synchronous=NORMAL: " + syncQuery.lastError().text());
    }

    return true;
}

void SqliteSessionStore::lazyClose()
{
    if (db.isOpen()) {
        db.close();
    }
}

// ============================================================================
// Debug-build database invariant checking
// ============================================================================

#ifndef QT_NO_DEBUG

/**
 * Verifies that no segment_id appears more than once in the durations table.
 *
 * Called after every successful write operation in debug builds. The
 * UNIQUE(segment_id) constraint should prevent duplicates at the schema
 * level, but this check catches bugs where a write path accidentally
 * inserts a row without going through the constraint (e.g. using a
 * different table name, or a migration path that bypasses the constraint).
 *
 * The database must be open when this is called.  Violations are logged
 * via qWarning (not fatal) so they surface in test output.
 */
void SqliteSessionStore::checkSegmentIdUniqueness()
{
    if (!db.isOpen()) {
        return;
    }

    QSqlQuery query(db);
    if (!query.exec("SELECT segment_id, COUNT(*) as cnt FROM durations GROUP BY segment_id HAVING COUNT(*) > 1")) {
        qWarning("[INVARIANT-DB] Failed to run segment_id uniqueness check: %s",
                 qPrintable(query.lastError().text()));
        return;
    }

    while (query.next()) {
        QString segId = query.value(0).toString();
        int count = query.value(1).toInt();
        qWarning("[INVARIANT-DB] Duplicate segment_id detected: '%s' appears %d times",
                 qPrintable(segId), count);
    }
}

#endif // QT_NO_DEBUG

/**
 * Schema validation + startup housekeeping.  Call once at application startup.
 *
 * Safe to call more than once (all operations are idempotent), but intended to
 * run once: each call re-runs retention cleanup and backup pruning.
 *
 * Returns true if the database is ready for use (or history is disabled / file
 * doesn't exist yet).  Returns false if the schema is outdated or the connection
 * cannot be opened.
 */
bool SqliteSessionStore::checkSchemaOnStartup()
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return true;
    }

    if (!QFile::exists(db.databaseName())) {
        return true;
    }

    // The constructor already ran ensureSchema(); this call is idempotent and
    // will skip migrations that have already been applied.  Calling ensureSchema()
    // here (rather than validateSchema() alone) ensures that if a migration is
    // somehow still needed at startup time, a pre-migration backup is created
    // before the table rebuild runs (S15).
    if (!ensureOpen()) {
        return false;
    }

    if (!ensureSchema()) {
        return false;
    }

    // Retention cleanup and backup pruning run once per startup, after schema is
    // confirmed valid.  Failures are non-fatal (logged, but do not abort startup).
    performRetentionCleanup();
    pruneOldBackups();

    return true;
}

/**
 * Deletes entries older than history_days_to_keep_ from the durations table.
 * Called once per startup from checkSchemaOnStartup() after schema validation.
 * Non-fatal: failures are logged but do not propagate to the caller.
 */
void SqliteSessionStore::performRetentionCleanup()
{
    if (!db.transaction()) {
        Logger::Log("[DB] Error starting retention cleanup transaction: " + db.lastError().text());
        return;
    }
    const QDateTime threshold = QDateTime::currentDateTimeUtc().addDays(-static_cast<int>(history_days_to_keep_));
    QSqlQuery query(db);
    query.prepare("DELETE FROM durations WHERE end_utc < :threshold");
    query.bindValue(":threshold", threshold.toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        db.rollback();
        Logger::Log("[DB] Error clearing old durations: " + query.lastError().text());
        return;
    }
    if (!db.commit()) {
        Logger::Log("[DB] Error committing retention cleanup: " + db.lastError().text());
        db.rollback();
    }
}

/**
 * Prunes old backup files in the database directory, keeping only the
 * `keepCount` most recent of each file type (.backup and .durations.txt).
 * Files are named with ISO timestamps so alphabetical sort = chronological.
 * Called once per startup from checkSchemaOnStartup(). Non-fatal.
 *
 * Note: the glob "*.backup" also matches "*.pre-migration.backup" files created
 * by ensureSegmentIdColumn().  Both count toward the same keepCount limit, which
 * is intentional — pre-migration backups are created at most once per schema
 * upgrade and are unlikely to crowd out regular backups in practice.
 */
void SqliteSessionStore::pruneOldBackups(int keepCount)
{
    QDir dir(QFileInfo(db.databaseName()).absoluteDir());
    const QString base = QFileInfo(db.databaseName()).fileName();

    for (const QString& pattern : {base + ".*.backup", base + ".*.durations.txt"}) {
        QStringList files = dir.entryList(
            QStringList() << pattern, QDir::Files, QDir::Name);
        while (files.size() > keepCount) {
            const QString name = files.takeFirst(); // oldest (alphabetical = chronological)
            if (!QFile::remove(dir.filePath(name))) {
                Logger::Log("[DB] Warning: Failed to remove old backup file: " + name);
            }
        }
    }
}

/**
 * Validates that the database schema contains the required UTC timestamp columns.
 *
 * Checks for start_utc, end_utc, and segment_id. Returns false if any are absent
 * (e.g. a manually-created table or a DB predating the UTC migration).
 */
bool SqliteSessionStore::validateSchema()
{
    QSqlQuery query(db);
    if (!query.exec("PRAGMA table_info(durations)")) {
        Logger::Log("[DB] Error checking table schema: " + query.lastError().text());
        return false;
    }

    bool hasStartUtc = false;
    bool hasEndUtc = false;
    bool hasSegmentId = false;

    while (query.next()) {
        const QString col = query.value(1).toString();
        if (col == "start_utc") hasStartUtc = true;
        else if (col == "end_utc") hasEndUtc = true;
        else if (col == "segment_id") hasSegmentId = true;
    }

    if (!hasStartUtc || !hasEndUtc || !hasSegmentId) {
        Logger::Log(QString("[DB] Schema validation failed: start_utc=%1, end_utc=%2, segment_id=%3")
            .arg(hasStartUtc ? "present" : "MISSING")
            .arg(hasEndUtc ? "present" : "MISSING")
            .arg(hasSegmentId ? "present" : "MISSING"));
        return false;
    }

    return true;
}

bool SqliteSessionStore::ensureIsFinalizedColumn()
{
    QSqlQuery tableInfo(db);
    if (!tableInfo.exec("PRAGMA table_info(durations)")) {
        Logger::Log("[DB] Error checking table_info for is_finalized migration: " + tableInfo.lastError().text());
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

    Logger::Log("[DB] Migrating schema: adding durations.is_finalized and marking existing rows as finalized");

    QSqlQuery alterQuery(db);
    if (!alterQuery.exec("ALTER TABLE durations ADD COLUMN is_finalized INTEGER NOT NULL DEFAULT 0")) {
        Logger::Log("[DB] Error adding is_finalized column: " + alterQuery.lastError().text());
        return false;
    }

    QSqlQuery migrateQuery(db);
    if (!migrateQuery.exec("UPDATE durations SET is_finalized = 1")) {
        Logger::Log("[DB] Error finalizing migrated rows: " + migrateQuery.lastError().text());
        return false;
    }

    return true;
}

bool SqliteSessionStore::ensureSegmentIdColumn()
{
    QSqlQuery tableInfo(db);
    if (!tableInfo.exec("PRAGMA table_info(durations)")) {
        Logger::Log("[DB] Error checking table_info for segment_id migration: " + tableInfo.lastError().text());
        return false;
    }

    bool hasSegmentId = false;
    while (tableInfo.next()) {
        if (tableInfo.value(1).toString() == "segment_id") {
            hasSegmentId = true;
            break;
        }
    }

    if (hasSegmentId) {
        return true;
    }

    Logger::Log("[DB] Migrating schema: rebuilding durations table with segment_id identity");

    // S13: Create a pre-migration backup before the destructive table rebuild.
    // Close the DB, copy the file, reopen — same pattern as createBackup().
    // The PRAGMA synchronous=NORMAL will be re-applied at the end of ensureSchema().
    if (QFile::exists(db.databaseName())) {
        const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-ddTHH-mm-ss");
        const QString backupPath = db.databaseName() + "." + timestamp + ".pre-migration.backup";
        db.close();
        if (!QFile::copy(db.databaseName(), backupPath)) {
            Logger::Log("[DB] Warning: Failed to create pre-migration backup");
        } else {
            Logger::Log("[DB] Created pre-migration backup: " + backupPath);
        }
        if (!db.open()) {
            Logger::Log("[DB] CRITICAL: Failed to reopen database after pre-migration backup");
            return false;
        }
    }

    if (!db.transaction()) {
        return false;
    }

    QSqlQuery createNew(db);
    if (!createNew.exec(
            "CREATE TABLE IF NOT EXISTS durations_new ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL,"
            "type INTEGER NOT NULL,"
            "duration INTEGER NOT NULL,"
            "start_date DATE NOT NULL,"
            "start_time TEXT NOT NULL,"
            "end_date DATE NOT NULL,"
            "end_time TEXT NOT NULL,"
            "is_finalized INTEGER NOT NULL DEFAULT 0,"
            "UNIQUE(segment_id)"
            ")")) {
        db.rollback();
        return false;
    }

    QSqlQuery selectRows(db);
    if (!selectRows.exec("SELECT type, duration, start_date, start_time, end_date, end_time, COALESCE(is_finalized, 1) FROM durations ORDER BY id ASC")) {
        db.rollback();
        return false;
    }

    QSqlQuery insertNew(db);
    insertNew.prepare(
        "INSERT INTO durations_new (segment_id, type, duration, start_date, start_time, end_date, end_time, is_finalized) "
        "VALUES (:segment_id, :type, :duration, :start_date, :start_time, :end_date, :end_time, :is_finalized)"
    );

    int migratedRows = 0;
    while (selectRows.next()) {
        insertNew.bindValue(":segment_id", TimeDuration::createSegmentId());
        insertNew.bindValue(":type", selectRows.value(0));
        insertNew.bindValue(":duration", selectRows.value(1));
        insertNew.bindValue(":start_date", selectRows.value(2));
        insertNew.bindValue(":start_time", selectRows.value(3));
        insertNew.bindValue(":end_date", selectRows.value(4));
        insertNew.bindValue(":end_time", selectRows.value(5));
        insertNew.bindValue(":is_finalized", selectRows.value(6));
        if (!insertNew.exec()) {
            db.rollback();
            return false;
        }
        migratedRows++;
    }

    QSqlQuery dropOld(db);
    if (!dropOld.exec("DROP TABLE durations")) {
        db.rollback();
        return false;
    }

    QSqlQuery renameNew(db);
    if (!renameNew.exec("ALTER TABLE durations_new RENAME TO durations")) {
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        db.rollback();
        return false;
    }

    Logger::Log(QString("[DB] segment_id migration completed for %1 rows").arg(migratedRows));

    return true;
}

bool SqliteSessionStore::ensureUtcColumns()
{
    QSqlQuery tableInfo(db);
    if (!tableInfo.exec("PRAGMA table_info(durations)")) {
        Logger::Log("[DB] Error checking table_info for utc column migration: " + tableInfo.lastError().text());
        return false;
    }

    bool hasStartUtc = false;
    bool hasEndUtc = false;
    while (tableInfo.next()) {
        const QString col = tableInfo.value(1).toString();
        if (col == "start_utc") hasStartUtc = true;
        else if (col == "end_utc")   hasEndUtc   = true;
    }

    if (hasStartUtc && hasEndUtc) {
        return true;
    }

    Logger::Log("[DB] Migrating schema: adding start_utc / end_utc columns");

    // Adding nullable columns is non-destructive — use ALTER TABLE (no table rebuild needed).
    if (!hasStartUtc) {
        QSqlQuery alter(db);
        if (!alter.exec("ALTER TABLE durations ADD COLUMN start_utc TEXT")) {
            Logger::Log("[DB] Error adding start_utc column: " + alter.lastError().text());
            return false;
        }
    }
    if (!hasEndUtc) {
        QSqlQuery alter(db);
        if (!alter.exec("ALTER TABLE durations ADD COLUMN end_utc TEXT")) {
            Logger::Log("[DB] Error adding end_utc column: " + alter.lastError().text());
            return false;
        }
    }

    // Backfill existing rows by concatenating the stored UTC date/time parts.
    // The WHERE clause makes this idempotent: rows already written by the dual-write
    // path (Phase 1) already have start_utc set and are skipped.
    if (!db.transaction()) {
        Logger::Log("[DB] Error starting utc backfill transaction: " + db.lastError().text());
        return false;
    }
    QSqlQuery backfill(db);
    if (!backfill.exec(
            "UPDATE durations "
            "SET start_utc = start_date || 'T' || start_time || 'Z',"
            "    end_utc   = end_date   || 'T' || end_time   || 'Z' "
            "WHERE start_utc IS NULL")) {
        db.rollback();
        Logger::Log("[DB] Error backfilling utc columns: " + backfill.lastError().text());
        return false;
    }
    if (!db.commit()) {
        db.rollback();
        Logger::Log("[DB] Error committing utc backfill: " + db.lastError().text());
        return false;
    }

    Logger::Log("[DB] UTC column migration completed");
    return true;
}

bool SqliteSessionStore::dropLegacyColumns()
{
    QSqlQuery tableInfo(db);
    if (!tableInfo.exec("PRAGMA table_info(durations)")) {
        Logger::Log("[DB] Error checking table_info for legacy column drop: " + tableInfo.lastError().text());
        return false;
    }
    bool hasLegacy = false;
    while (tableInfo.next()) {
        if (tableInfo.value(1).toString() == "start_date") { hasLegacy = true; break; }
    }
    if (!hasLegacy) return true;

    const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-ddTHH-mm-ss");
    const QString backupPath = db.databaseName() + "." + timestamp + ".pre-migration.backup";
    db.close();
    if (!QFile::copy(db.databaseName(), backupPath)) {
        Logger::Log("[DB] Warning: Failed to create pre-migration backup for legacy column drop");
    } else {
        Logger::Log("[DB] Created pre-migration backup: " + backupPath);
    }
    if (!db.open()) {
        Logger::Log("[DB] CRITICAL: Failed to reopen database after pre-migration backup for legacy column drop");
        return false;
    }

    if (!db.transaction()) {
        Logger::Log("[DB] Error starting transaction for legacy column drop: " + db.lastError().text());
        return false;
    }

    QSqlQuery q(db);
    if (!q.exec(
            "CREATE TABLE durations_new ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL,"
            "type INTEGER NOT NULL,"
            "start_utc TEXT NOT NULL,"
            "end_utc TEXT NOT NULL,"
            "is_finalized INTEGER NOT NULL DEFAULT 0,"
            "UNIQUE(segment_id)"
            ")")) {
        db.rollback();
        Logger::Log("[DB] Error creating durations_new for legacy drop: " + q.lastError().text());
        return false;
    }

    // Copy data. Use COALESCE so rows with NULL start_utc/end_utc (which should
    // not occur in production) are still recovered from the legacy columns if present.
    // Rows where neither source is available are excluded — they are unrecoverable.
    if (!q.exec(
            "INSERT INTO durations_new (id, segment_id, type, start_utc, end_utc, is_finalized) "
            "SELECT id, segment_id, type, "
            "    COALESCE(start_utc, start_date || 'T' || start_time || 'Z'), "
            "    COALESCE(end_utc,   end_date   || 'T' || end_time   || 'Z'), "
            "    is_finalized "
            "FROM durations "
            "WHERE COALESCE(start_utc, start_date) IS NOT NULL "
            "  AND COALESCE(end_utc,   end_date)   IS NOT NULL")) {
        db.rollback();
        Logger::Log("[DB] Error copying data to durations_new: " + q.lastError().text());
        return false;
    }

    if (!q.exec("DROP TABLE durations")) {
        db.rollback();
        Logger::Log("[DB] Error dropping old durations table: " + q.lastError().text());
        return false;
    }
    if (!q.exec("ALTER TABLE durations_new RENAME TO durations")) {
        db.rollback();
        Logger::Log("[DB] Error renaming durations_new to durations: " + q.lastError().text());
        return false;
    }

    if (!db.commit()) {
        db.rollback();
        Logger::Log("[DB] Error committing legacy column drop: " + db.lastError().text());
        return false;
    }

    Logger::Log("[DB] Legacy timestamp columns dropped successfully");
    return true;
}

bool SqliteSessionStore::ensureSettingsTable()
{
    QSqlQuery query(db);
    if (!query.exec(
            "CREATE TABLE IF NOT EXISTS app_settings ("
            "key TEXT PRIMARY KEY,"
            "value TEXT NOT NULL"
            ")")) {
        Logger::Log("[DB] Error creating app_settings table: " + query.lastError().text());
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
 *
 * Concurrency:
 * - This method closes and reopens the database to ensure a clean file copy.
 *   The caller (a public method) must already hold db_mutex_, which prevents
 *   any other SqliteSessionStore operation from seeing the closed connection.
 *   QRecursiveMutex allows re-entrant calls within the same thread.
 *
 * TODO(S16): The current close-copy-reopen approach is correct for rollback-journal
 * mode (no -wal/-shm sidecars).  If WAL mode is ever enabled, switch to SQLite's
 * online backup API (sqlite3_backup_*) so the source DB stays open during the copy.
 */
BackupResult SqliteSessionStore::createBackup(const std::deque<TimeDuration>& durations, TransactionMode mode)
{
    if (!QFile::exists(db.databaseName())) {
        return BackupResult::Success;
    }

    // Skip backup if disk space is low (< 100 MB).  Proceeding with a full-table
    // write on a nearly-full volume risks leaving both the DB and the backup in a
    // corrupted state; warn once and continue without a backup instead.
    {
        QStorageInfo storage(QFileInfo(db.databaseName()).absoluteDir().absolutePath());
        const qint64 available = storage.bytesAvailable();
        if (available >= 0 && available < 100LL * 1024 * 1024) {
            Logger::Log(QString("[DB] Warning: Low disk space (%1 MB). Skipping backup.")
                .arg(available / (1024 * 1024)));
            return BackupResult::Success;
        }
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
        Logger::Log("[DB] Error: Failed to create backup of database");
    } else {
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
        Logger::Log("[DB] Created durations log: " + durationsFileName);
    } else {
        Logger::Log("[DB] Warning: Could not create durations log file");
    }

    // Reopen database if it was open before.
    // Re-apply PRAGMA synchronous=NORMAL since reopening creates a fresh connection
    // that loses all session-level PRAGMAs set in ensureSchema().
    if (wasOpen) {
        if (!db.open()) {
            Logger::Log("[DB] CRITICAL: Failed to reopen database after backup: " + db.lastError().text());
            return BackupResult::ReopenFailed;
        }
        QSqlQuery pragmaQuery(db);
        pragmaQuery.exec("PRAGMA synchronous=NORMAL");
    }

    return success ? BackupResult::Success : BackupResult::BackupFailed;
}

/**
 * Persists the completed segments of a session to the database.
 *
 * Normalizes the timeline internally (merging adjacent same-type segments),
 * computes orphaned segment IDs, and delegates to updateDurationsById()
 * with the clean deque and orphan list. Callers never see removedSegmentIds.
 */
bool SqliteSessionStore::commitSession(const Timeline& session)
{
    // Collect before-normalization segment IDs
    std::vector<QString> beforeIds;
    for (const auto& d : session.completed())
        beforeIds.push_back(d.segment_id);

    // Normalize to merge adjacent same-type segments
    Timeline normed = session.normalized();

    // Compute orphan IDs: segment IDs that disappeared during normalization
    std::vector<QString> orphanIds;
    for (const auto& id : beforeIds) {
        bool found = false;
        for (const auto& d : normed.completed())
            if (d.segment_id == id) { found = true; break; }
        if (!found)
            orphanIds.push_back(id);
    }

    return updateDurationsById(normed.completed(), orphanIds);
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
bool SqliteSessionStore::saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode,
                                     const std::vector<QString>& removedSegmentIds)
{
    QMutexLocker locker(&db_mutex_);

    // If history storage is disabled, treat as success (no-op)
    if (history_days_to_keep_ == 0) {
        return true;
    }

    // Validate connection first (S5: ensureOpen before createBackup so we don't
    // create a backup for a DB we can't open).
    if (!ensureOpen()) {
        Logger::Log("[DB] Could not open DB to save Durations");
        return false;
    }

    const BackupResult backup = createBackup(durations, mode);
    if (backup == BackupResult::ReopenFailed) {
        Logger::Log("[DB] CRITICAL: DB connection lost after backup - aborting save");
        return false;
    }
    if (backup != BackupResult::Success) {
        const QString modeStr = (mode == TransactionMode::Replace) ? "REPLACE" : "APPEND";
        Logger::Log("[DB] Warning: Backup failed before " + modeStr + " operation - proceeding without backup");
    }

    if (!db.transaction()) {
        Logger::Log("[DB] Error starting transaction for Saving: " + db.lastError().text());
        return false;
    }

    // Replace mode: clear existing entries before inserting new ones
    if (mode == TransactionMode::Replace) {
        QSqlQuery clearQuery(db);
        if (!clearQuery.exec("DELETE FROM durations")) {
            db.rollback();
            Logger::Log("[DB] Error clearing durations table: " + clearQuery.lastError().text());
            return false;
        }
    }

    // Delete orphaned segment_ids that were merged away by cleanDurations.
    // This must happen inside the same transaction as the INSERT for atomicity.
    if (!removedSegmentIds.empty()) {
        QSqlQuery deleteOrphanQuery(db);
        deleteOrphanQuery.prepare("DELETE FROM durations WHERE segment_id = :segment_id");
        for (const auto& orphanId : removedSegmentIds) {
            deleteOrphanQuery.bindValue(":segment_id", orphanId);
            if (!deleteOrphanQuery.exec()) {
                db.rollback();
                Logger::Log("[DB] Error deleting orphaned segment_id: " + deleteOrphanQuery.lastError().text());
                return false;
            }
        }
    }

    // Prepare upsert statement for batch insertion (storing times in UTC).
    // ON CONFLICT(segment_id) preserves the autoincrement id (unlike INSERT OR REPLACE),
    // which the reconciliation path relies on via OrphanCheckpoint.id, and avoids aborting
    // the surrounding transaction when an Append re-encounters an existing segment_id.
    QSqlQuery query(db);
    query.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:segment_id, :type, :start_utc, :end_utc, 1) "
                  "ON CONFLICT(segment_id) DO UPDATE SET "
                  "    type         = excluded.type, "
                  "    start_utc    = excluded.start_utc, "
                  "    end_utc      = excluded.end_utc, "
                  "    is_finalized = excluded.is_finalized");

    // Insert each duration entry (convert to UTC for storage)
    for (const auto& d : durations) {
        query.bindValue(":segment_id", d.segment_id);
        query.bindValue(":type", static_cast<int>(d.type));
        query.bindValue(":start_utc", d.startTime.toUTC().toString(Qt::ISODateWithMs));
        query.bindValue(":end_utc", d.endTime.toUTC().toString(Qt::ISODateWithMs));

        if (!query.exec()) {
            db.rollback();
            Logger::Log("[DB] Error inserting duration: " + query.lastError().text());
            return false;
        }
    }

    bool commitSuccessful = db.commit();
    if (!commitSuccessful) {
        Logger::Log("[DB] Error committing save transaction: " + db.lastError().text());
    }

#ifndef QT_NO_DEBUG
    if (commitSuccessful) {
        checkSegmentIdUniqueness();
    }
#endif

    return commitSuccessful;
}

bool SqliteSessionStore::replaceAll(const Timeline& history, const Timeline& session)
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return true;
    }

    const std::deque<TimeDuration>& historyDurations = history.completed();
    const std::deque<TimeDuration>& currentSessionDurations = session.completed();

    std::deque<TimeDuration> allDurations;
    allDurations.insert(allDurations.end(), historyDurations.begin(), historyDurations.end());
    allDurations.insert(allDurations.end(), currentSessionDurations.begin(), currentSessionDurations.end());

    if (!ensureOpen()) {
        Logger::Log("[DB] Could not open DB to replace durations");
        return false;
    }

    const BackupResult backup = createBackup(allDurations, TransactionMode::Replace);
    if (backup == BackupResult::ReopenFailed) {
        Logger::Log("[DB] CRITICAL: DB connection lost after backup - aborting replace");
        return false;
    }
    if (backup != BackupResult::Success) {
        Logger::Log("[DB] Warning: Backup failed before REPLACE operation - proceeding without backup");
    }

    if (!db.transaction()) {
        Logger::Log("[DB] Error starting transaction for replace: " + db.lastError().text());
        return false;
    }

    QSqlQuery clearQuery(db);
    if (!clearQuery.exec("DELETE FROM durations")) {
        db.rollback();
        Logger::Log("[DB] Error clearing durations table: " + clearQuery.lastError().text());
        return false;
    }

    QSqlQuery finalizedInsert(db);
    finalizedInsert.prepare(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES (:segment_id, :type, :start_utc, :end_utc, 1)"
    );

    for (const auto& d : historyDurations) {
        finalizedInsert.bindValue(":segment_id", d.segment_id);
        finalizedInsert.bindValue(":type", static_cast<int>(d.type));
        finalizedInsert.bindValue(":start_utc", d.startTime.toUTC().toString(Qt::ISODateWithMs));
        finalizedInsert.bindValue(":end_utc", d.endTime.toUTC().toString(Qt::ISODateWithMs));

        if (!finalizedInsert.exec()) {
            db.rollback();
            Logger::Log("[DB] Error inserting finalized duration: " + finalizedInsert.lastError().text());
            return false;
        }
    }

    QSqlQuery unfinalizedInsert(db);
    unfinalizedInsert.prepare(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES (:segment_id, :type, :start_utc, :end_utc, 0)"
    );

    for (const auto& d : currentSessionDurations) {
        unfinalizedInsert.bindValue(":segment_id", d.segment_id);
        unfinalizedInsert.bindValue(":type", static_cast<int>(d.type));
        unfinalizedInsert.bindValue(":start_utc", d.startTime.toUTC().toString(Qt::ISODateWithMs));
        unfinalizedInsert.bindValue(":end_utc", d.endTime.toUTC().toString(Qt::ISODateWithMs));

        if (!unfinalizedInsert.exec()) {
            db.rollback();
            Logger::Log("[DB] Error inserting current-session duration: " + unfinalizedInsert.lastError().text());
            return false;
        }
    }

    const bool success = db.commit();
    if (!success) {
        Logger::Log("[DB] Error committing replace transaction: " + db.lastError().text());
    }

#ifndef QT_NO_DEBUG
    if (success) {
        checkSegmentIdUniqueness();
    }
#endif

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
LoadResult SqliteSessionStore::loadDurations()
{
    QMutexLocker locker(&db_mutex_);

    LoadResult result;

    if (!ensureOpen()) {
        Logger::Log("[DB] Could not open DB to load Durations");
        return result;
    }

    // Start read transaction for consistent snapshot
    if (!db.transaction()) {
        Logger::Log("[DB] Error starting read transaction: " + db.lastError().text());
        return result;
    }

    // Load all durations ordered by start_utc
    QSqlQuery query(db);
    query.prepare("SELECT segment_id, type, start_utc, end_utc FROM durations WHERE is_finalized = 1 ORDER BY start_utc");
    if (!query.exec()) {
        db.rollback();
        Logger::Log("[DB] Error executing load query: " + query.lastError().text());
        return result;
    }

    // Parse each row and reconstruct TimeDuration objects
    while (query.next()) {
        const QString segmentId = query.value(0).toString();
        const int typeInt = query.value(1).toInt();
        const QDateTime startDateTime = QDateTime::fromString(query.value(2).toString(), Qt::ISODateWithMs).toLocalTime();
        const QDateTime endDateTime   = QDateTime::fromString(query.value(3).toString(), Qt::ISODateWithMs).toLocalTime();

        // Validate type enum range
        if (typeInt != static_cast<int>(DurationType::Activity) &&
            typeInt != static_cast<int>(DurationType::Pause)) {
            Logger::Log(QString("[DB] Warning: Invalid type value %1, skipping entry").arg(typeInt));
            result.skipped++;
            continue;
        }

        DurationType type = static_cast<DurationType>(typeInt);

        // Validate timestamps
        if (!startDateTime.isValid() || !endDateTime.isValid()) {
            Logger::Log(QString("[DB] Warning: Skipped entry with invalid UTC timestamp(s) - start: %1, end: %2")
                .arg(query.value(2).toString()).arg(query.value(3).toString()));
            result.skipped++;
            continue;
        }

        // Validate start <= end
        if (startDateTime > endDateTime) {
            Logger::Log(QString("[DB] Warning: Skipped entry with start > end - Start: %1, End: %2")
                .arg(startDateTime.toString(Qt::ISODate)).arg(endDateTime.toString(Qt::ISODate)));
            result.skipped++;
            continue;
        }

        // Create TimeDuration with explicit start/end times
        auto seg = TimeDuration::create(type, startDateTime, endDateTime, segmentId);
        if (!seg.has_value()) {
            Logger::Log(QString("[DB] Dropped cross-midnight row at load: %1 → %2")
                .arg(startDateTime.toString(Qt::ISODateWithMs))
                .arg(endDateTime.toString(Qt::ISODateWithMs)));
            result.skipped++;
            continue;
        }
        result.durations.emplace_back(std::move(*seg));
    }

    // Rollback is idiomatic for read-only transactions: no writes were made,
    // so there is nothing to commit. Both commit() and rollback() release the
    // transaction lock, but rollback() signals the intent more clearly.
    db.rollback();

    if (result.skipped > 0) {
        Logger::Log(QString("[DB] loadDurations reconciliation summary: skipped=%1").arg(result.skipped));
    }

    return result;
}

/**
 * Checks whether finalized entries exist for a given date.
 *
 * Returns a tri-state result:
 * - Yes:     At least one finalized entry exists for the date.
 * - No:      The DB was queried successfully and zero entries were found.
 * - Unknown: The DB could not be opened (history disabled or open failure).
 *            Callers should treat Unknown conservatively — e.g. do NOT assume
 *            "no entries exist" when the answer is actually unknowable.
 */
EntriesForDateResult SqliteSessionStore::hasEntriesForDate(const QDate& date)
{
    QMutexLocker locker(&db_mutex_);

    if (!ensureOpen()) {
        Logger::Log("[DB] Could not open DB to check entries for date");
        return EntriesForDateResult::Unknown;
    }

    // Compute the UTC half-open range covering the caller's local day.
    const QDateTime localStart(date, QTime(0, 0, 0), Qt::LocalTime);
    const QDateTime localEnd = localStart.addDays(1);
    const QDateTime utcStart = localStart.toUTC();
    const QDateTime utcEnd   = localEnd.toUTC();

    // Check if any finalized entry started within that UTC range.
    QSqlQuery query(db);
    query.prepare(
        "SELECT 1 FROM durations"
        " WHERE is_finalized = 1"
        "   AND start_utc >= :utcStartTs"
        "   AND start_utc <  :utcEndTs"
        " LIMIT 1"
    );
    query.bindValue(":utcStartTs", utcStart.toString(Qt::ISODateWithMs));
    query.bindValue(":utcEndTs",   utcEnd.toString(Qt::ISODateWithMs));

    EntriesForDateResult result = EntriesForDateResult::Unknown;
    if (query.exec()) {
        result = query.next() ? EntriesForDateResult::Yes : EntriesForDateResult::No;
    } else {
        Logger::Log("[DB] Error checking entries for date: " + query.lastError().text());
    }

    return result;
}

/**
 * Saves or updates a checkpoint entry for crash recovery.
 *
 * Unlike saveDurations(), this method:
 * - Does NOT create backups (for performance - called every 5 minutes)
 * - Uses segment_id to update existing row instead of inserting new one
 * - Stores the actual segment_start_time_ (preserves original start, only updates end/duration)
 *
 * The caller (Timer) rotates current_checkpoint_segment_id_ on mode changes,
 * causing the next checkpoint to create a new row for the new segment.
 */
bool SqliteSessionStore::saveCheckpoint(DurationType type, qint64 duration, const QDateTime& startTime, const QDateTime& endTime, const QString& segmentId)
{
    QMutexLocker locker(&db_mutex_);
    Q_UNUSED(duration)  // duration is computed from timestamps at load; not stored

    // If history storage is disabled, treat as success (no-op)
    if (history_days_to_keep_ == 0) {
        return true;
    }

    if (!ensureOpen()) {
        Logger::Log("[DB] Could not open DB to save checkpoint");
        return false;
    }

    const QString startUtcStr = startTime.toUTC().toString(Qt::ISODateWithMs);
    const QString endUtcStr   = endTime.toUTC().toString(Qt::ISODateWithMs);

    if (!db.transaction()) {
        Logger::Log("[DB] Error starting transaction for checkpoint: " + db.lastError().text());
        return false;
    }

    bool success = false;
    QSqlQuery updateQuery(db);
    updateQuery.prepare(
        "UPDATE durations SET end_utc = :end_utc, is_finalized = 0 "
        "WHERE segment_id = :segment_id"
    );
    updateQuery.bindValue(":end_utc", endUtcStr);
    updateQuery.bindValue(":segment_id", segmentId);

    if (!updateQuery.exec()) {
        db.rollback();
        Logger::Log("[DB] Error updating checkpoint by segment_id: " + updateQuery.lastError().text());
        return false;
    }

    if (updateQuery.numRowsAffected() > 0) {
        success = db.commit();
        if (!success) {
            Logger::Log("[DB] Error committing checkpoint update: " + db.lastError().text());
        }
    } else {
        QSqlQuery insertQuery(db);
        insertQuery.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:segment_id, :type, :start_utc, :end_utc, 0)"
        );
        insertQuery.bindValue(":segment_id", segmentId);
        insertQuery.bindValue(":type", static_cast<int>(type));
        insertQuery.bindValue(":start_utc", startUtcStr);
        insertQuery.bindValue(":end_utc", endUtcStr);

        if (!insertQuery.exec()) {
            db.rollback();
            Logger::Log("[DB] Error inserting checkpoint by segment_id: " + insertQuery.lastError().text());
            return false;
        }

        success = db.commit();
        if (!success) {
            Logger::Log("[DB] Error committing checkpoint insert: " + db.lastError().text());
        }
    }

#ifndef QT_NO_DEBUG
    if (success) {
        checkSegmentIdUniqueness();
    }
#endif

    return success;
}

/**
 * Smart upsert (update or insert) for a batch of durations using segment identity.
 *
 * Use Case:
 * - When the timer stops or pauses, we update existing segment rows by segment_id.
 * - If a segment row is missing, it is inserted with the same segment_id.
 */
bool SqliteSessionStore::updateDurationsById(const std::deque<TimeDuration>& durations,
                                           const std::vector<QString>& removedSegmentIds)
{
    QMutexLocker locker(&db_mutex_);

    if (durations.empty()) {
        return true;
    }

    // If history storage is disabled, treat as success (no-op)
    if (history_days_to_keep_ == 0) {
        return true;
    }

    if (!ensureOpen()) {
        Logger::Log("[DB] Could not open DB to update durations");
        return false;
    }

    if (!db.transaction()) {
        Logger::Log("[DB] Error starting transaction for updating durations: " + db.lastError().text());
        return false;
    }

    // Delete orphaned segment_ids that were merged away by cleanDurations.
    // This must happen inside the same transaction as the UPDATE/INSERT for atomicity.
    if (!removedSegmentIds.empty()) {
        QSqlQuery deleteOrphanQuery(db);
        deleteOrphanQuery.prepare("DELETE FROM durations WHERE segment_id = :segment_id");
        for (const auto& orphanId : removedSegmentIds) {
            deleteOrphanQuery.bindValue(":segment_id", orphanId);
            if (!deleteOrphanQuery.exec()) {
                db.rollback();
                Logger::Log("[DB] Error deleting orphaned segment_id: " + deleteOrphanQuery.lastError().text());
                return false;
            }
        }
    }

    QSqlQuery updateQuery(db);
    updateQuery.prepare(
        "UPDATE durations SET type = :type, start_utc = :start_utc, end_utc = :end_utc, is_finalized = 1 "
        "WHERE segment_id = :segment_id"
    );

    QSqlQuery insertQuery(db);
    insertQuery.prepare(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES (:segment_id, :type, :start_utc, :end_utc, 1)"
    );

    int count = 0;

    for (const auto& d : durations) {
        const QString startUtcStr = d.startTime.toUTC().toString(Qt::ISODateWithMs);
        const QString endUtcStr   = d.endTime.toUTC().toString(Qt::ISODateWithMs);

        updateQuery.bindValue(":segment_id", d.segment_id);
        updateQuery.bindValue(":type", static_cast<int>(d.type));
        updateQuery.bindValue(":start_utc", startUtcStr);
        updateQuery.bindValue(":end_utc", endUtcStr);

        if (!updateQuery.exec()) {
            db.rollback();
            Logger::Log("[DB] Error updating duration by segment_id: " + updateQuery.lastError().text());
            return false;
        }

        if (updateQuery.numRowsAffected() == 0) {
            insertQuery.bindValue(":segment_id", d.segment_id);
            insertQuery.bindValue(":type", static_cast<int>(d.type));
            insertQuery.bindValue(":start_utc", startUtcStr);
            insertQuery.bindValue(":end_utc", endUtcStr);

            if (!insertQuery.exec()) {
                db.rollback();
                Logger::Log("[DB] Error inserting duration by segment_id: " + insertQuery.lastError().text());
                return false;
            }
        }
        count++;
    }

    bool success = db.commit();
    if (success) {
        Logger::Log(QString("[DB] Upserted %1 durations").arg(count));
    } else {
        Logger::Log("[DB] Error committing update transaction: " + db.lastError().text());
    }

#ifndef QT_NO_DEBUG
    if (success) {
        checkSegmentIdUniqueness();
    }
#endif

    return success;
}

/**
 * Forces any pending database writes to be physically written to disk.
 *
 * This is critical during Windows shutdown to ensure data is persisted
 * before the process is terminated. SQLite may buffer writes even after
 * commit() returns — this method ensures they're flushed by temporarily
 * promoting to PRAGMA synchronous=FULL and issuing a dummy write to
 * trigger the fsync.
 *
 * Durability guarantee depends on journal mode:
 * This app uses rollback journal mode (the SQLite default), NOT WAL.  In
 * rollback journal mode, promoting synchronous to FULL is sufficient to
 * guarantee durability — the journal file is fully flushed before the
 * transaction is considered committed.
 *
 * If WAL mode is ever enabled (e.g. for concurrent reader performance),
 * this method must also call:
 *   PRAGMA wal_checkpoint(RESTART)
 * after the commit, to flush the WAL file into the main database file.
 * Without the checkpoint, the most recent writes live only in the WAL
 * and a power failure could leave them unrecoverable from the main file.
 */
void SqliteSessionStore::flushToDisc()
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return;
    }

    if (!ensureOpen()) {
        Logger::Log("[DB] flushToDisc: could not open DB");
        return;
    }

    QSqlQuery pragma(db);
    if (!pragma.exec("PRAGMA synchronous=FULL")) {
        Logger::Log("[DB] flushToDisc: set synchronous=FULL failed: " + pragma.lastError().text());
        return;
    }

    if (!db.transaction()) {
        Logger::Log("[DB] flushToDisc: could not begin transaction");
        return;
    }

    const QString ts = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QSqlQuery query(db);
    query.prepare("INSERT OR REPLACE INTO app_settings (key, value) VALUES ('last_flush_utc', :ts)");
    query.bindValue(":ts", ts);

    if (!query.exec()) {
        Logger::Log("[DB] flushToDisc: write failed: " + query.lastError().text());
        db.rollback();
        return;
    }
    if (!db.commit()) {
        Logger::Log("[DB] flushToDisc: commit failed: " + db.lastError().text());
        db.rollback();
        return;
    }

    Logger::Log("[DB] flushToDisc: durable heartbeat committed");
}

std::deque<OrphanCheckpoint> SqliteSessionStore::loadUnfinalizedCheckpoints()
{
    QMutexLocker locker(&db_mutex_);

    std::deque<OrphanCheckpoint> orphans;

    if (!ensureOpen()) {
        return orphans;
    }

    QSqlQuery query(db);
    query.prepare("SELECT id, segment_id, type, start_utc, end_utc FROM durations WHERE is_finalized = 0 ORDER BY id ASC");
    if (!query.exec()) {
        Logger::Log("[DB] Error loading orphan checkpoints: " + query.lastError().text());
        return orphans;
    }

    while (query.next()) {
        const int typeInt = query.value(2).toInt();
        if (typeInt != static_cast<int>(DurationType::Activity) &&
            typeInt != static_cast<int>(DurationType::Pause)) {
            continue;
        }

        const QDateTime startTime = QDateTime::fromString(query.value(3).toString(), Qt::ISODateWithMs).toLocalTime();
        const QDateTime endTime   = QDateTime::fromString(query.value(4).toString(), Qt::ISODateWithMs).toLocalTime();
        if (!startTime.isValid() || !endTime.isValid()) {
            Logger::Log(QString("[DB] loadUnfinalizedCheckpoints: skipped row with invalid timestamp(s): start=%1 end=%2")
                .arg(query.value(3).toString()).arg(query.value(4).toString()));
            continue;
        }
        if (startTime >= endTime) {
            Logger::Log(QString("[DB] loadUnfinalizedCheckpoints: skipped row with start >= end: start=%1 end=%2")
                .arg(startTime.toString(Qt::ISODateWithMs)).arg(endTime.toString(Qt::ISODateWithMs)));
            continue;
        }

        OrphanCheckpoint checkpoint;
        checkpoint.id = query.value(0).toLongLong();
        checkpoint.segment_id = query.value(1).toString();
        checkpoint.type = static_cast<DurationType>(typeInt);
        checkpoint.duration = startTime.msecsTo(endTime);
        checkpoint.startTime = startTime;
        checkpoint.endTime = endTime;
        orphans.push_back(checkpoint);
    }

    return orphans;
}

bool SqliteSessionStore::finalizeIfNoOverlap(qint64 rowId, const QDateTime& startUtc, const QDateTime& endUtc)
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return false;
    }

    if (!ensureOpen()) {
        return false;
    }

    if (!db.transaction()) {
        Logger::Log("[DB] Error starting finalizeIfNoOverlap transaction: " + db.lastError().text());
        return false;
    }

    // Probe for any *other* finalised row that overlaps [startUtc, endUtc).
    // Two intervals [a, b) and [c, d) overlap iff a < d AND c < b.
    // ISO-8601 strings compare lexicographically in chronological order.
    // The idx_start_utc partial index covers the LHS of the predicate.
    const QString startTs = startUtc.toString(Qt::ISODateWithMs);
    const QString endTs   = endUtc.toString(Qt::ISODateWithMs);

    QSqlQuery probe(db);
    probe.prepare(
        "SELECT 1 FROM durations"
        " WHERE is_finalized = 1"
        "   AND id != :rowId"
        "   AND start_utc < :endTs"
        "   AND end_utc   > :startTs"
        " LIMIT 1"
    );
    probe.bindValue(":rowId", static_cast<qlonglong>(rowId));
    probe.bindValue(":endTs", endTs);
    probe.bindValue(":startTs", startTs);

    if (!probe.exec()) {
        Logger::Log("[DB] Error probing for orphan overlap: " + probe.lastError().text());
        db.rollback();
        return false;
    }

    if (probe.next()) {
        // Overlap detected: leave the row unchanged.
        db.rollback();
        return false;
    }

    QSqlQuery updateQuery(db);
    updateQuery.prepare("UPDATE durations SET is_finalized = 1 WHERE id = :id AND is_finalized = 0");
    updateQuery.bindValue(":id", static_cast<qlonglong>(rowId));
    if (!updateQuery.exec()) {
        Logger::Log("[DB] Error finalising orphan row: " + updateQuery.lastError().text());
        db.rollback();
        return false;
    }

    const bool updated = updateQuery.numRowsAffected() > 0;
    if (!updated) {
        // Row missing or already finalised — nothing to commit, but treat as a
        // no-op rather than a failure.
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        Logger::Log("[DB] Error committing finalizeIfNoOverlap: " + db.lastError().text());
        db.rollback();
        return false;
    }

    return true;
}

ReconcileResult SqliteSessionStore::reconcileUnfinalizedCheckpoints(const std::vector<OrphanCheckpoint>& orphansToFinalize,
                                                                   const std::vector<long long>& outrightDropIds)
{
    QMutexLocker locker(&db_mutex_);

    ReconcileResult result;

    if (history_days_to_keep_ == 0) {
        result.ok = false;
        return result;
    }

    if (orphansToFinalize.empty() && outrightDropIds.empty()) {
        return result;
    }

    if (!ensureOpen()) {
        result.ok = false;
        return result;
    }

    // Outright drops happen first in their own transaction so they survive even
    // if a subsequent finalize call fails.  Finalise calls are then issued one
    // by one via finalizeIfNoOverlap, each in its own atomic transaction.
    if (!outrightDropIds.empty()) {
        if (!db.transaction()) {
            Logger::Log("[DB] Error starting orphan drop transaction: " + db.lastError().text());
            result.ok = false;
            return result;
        }

        QSqlQuery dropQuery(db);
        dropQuery.prepare("DELETE FROM durations WHERE id = :id AND is_finalized = 0");
        for (long long id : outrightDropIds) {
            dropQuery.bindValue(":id", static_cast<qlonglong>(id));
            if (!dropQuery.exec()) {
                Logger::Log("[DB] Error dropping orphan row: " + dropQuery.lastError().text());
                db.rollback();
                result.ok = false;
                return result;
            }
        }

        if (!db.commit()) {
            Logger::Log("[DB] Error committing orphan drop transaction: " + db.lastError().text());
            db.rollback();
            result.ok = false;
            return result;
        }
    }

    for (const auto& orphan : orphansToFinalize) {
        const QDateTime startUtc = orphan.startTime.toUTC();
        const QDateTime endUtc   = orphan.endTime.toUTC();
        if (finalizeIfNoOverlap(orphan.id, startUtc, endUtc)) {
            result.finalized.push_back(orphan.id);
        } else {
            result.dropped.push_back(orphan.id);
        }
    }

    return result;
}

bool SqliteSessionStore::setLastCleanShutdownMarker(const QDateTime& timestamp)
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return true;
    }

    if (!ensureOpen()) {
        return false;
    }

    if (!db.transaction()) {
        return false;
    }

    const QString markerValue = timestamp.toUTC().toString(Qt::ISODateWithMs);
    QSqlQuery query(db);
    query.prepare("INSERT OR REPLACE INTO app_settings (key, value) VALUES (:key, :value)");
    query.bindValue(":key", QString::fromLatin1(kLastCleanShutdownKey));
    query.bindValue(":value", markerValue);

    if (!query.exec()) {
        db.rollback();
        return false;
    }

    const bool committed = db.commit();
    if (!committed) {
        db.rollback();
    }

    return committed;
}

std::optional<MarkerResult> SqliteSessionStore::consumeLastCleanShutdownMarker()
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return std::nullopt;
    }

    if (!ensureOpen()) {
        return MarkerResult { {}, MarkerResult::Status::Error };
    }

    if (!db.transaction()) {
        return MarkerResult { {}, MarkerResult::Status::Error };
    }

    std::optional<QDateTime> parsedTs;
    QSqlQuery readQuery(db);
    readQuery.prepare("SELECT value FROM app_settings WHERE key = :key");
    readQuery.bindValue(":key", QString::fromLatin1(kLastCleanShutdownKey));
    if (!readQuery.exec()) {
        db.rollback();
        return MarkerResult { {}, MarkerResult::Status::Error };
    }
    if (readQuery.next()) {
        const QString value = readQuery.value(0).toString();
        QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
        if (!parsed.isValid()) {
            parsed = QDateTime::fromString(value, Qt::ISODate);
        }
        if (parsed.isValid()) {
            parsedTs = parsed.toLocalTime();
        }
    }

    QSqlQuery deleteQuery(db);
    deleteQuery.prepare("DELETE FROM app_settings WHERE key = :key");
    deleteQuery.bindValue(":key", QString::fromLatin1(kLastCleanShutdownKey));
    if (!deleteQuery.exec()) {
        db.rollback();
        return MarkerResult { {}, MarkerResult::Status::Error };
    }

    if (!db.commit()) {
        db.rollback();
        return MarkerResult { {}, MarkerResult::Status::Error };
    }

    if (parsedTs.has_value()) {
        return MarkerResult { *parsedTs, MarkerResult::Status::Found };
    }
    return MarkerResult { {}, MarkerResult::Status::NotFound };
}
