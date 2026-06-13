#ifndef SQLITESESSIONSTORE_H
#define SQLITESESSIONSTORE_H

#include <QObject>
#include <QSqlDatabase>
#include <QDateTime>
#include <QMutex>
#include <deque>
#include <vector>
#include <optional>
#include "types.h"
#include "settings.h"
#include "sessionstore.h"

// saveDurations/createBackup use TransactionMode as an implementation detail.
// The enum is not part of SessionStore's public interface; it lives here
// so it can be removed from types.h.
enum class TransactionMode { Append, Replace };

// Three-way result returned by createBackup().
//   Success      — file copied and DB reopened.
//   BackupFailed — file copy failed; DB connection state is unchanged (caller
//                  may proceed without a backup — data is not at risk yet).
//   ReopenFailed — DB could not be reopened after the close-copy step; caller
//                  must abort the pending write — the connection is dead.
enum class BackupResult { Success, BackupFailed, ReopenFailed };

// Connection lifecycle: the database is opened in the constructor
// (ensureSchema creates tables, indexes, and sets PRAGMA once) and
// closed in the destructor.  Retention cleanup and backup pruning run once per
// startup via checkSchemaOnStartup() — safe to call more than once (idempotent),
// but intended to be called once.  All public methods call ensureOpen() as a
// lightweight health check; if the connection is unexpectedly lost, ensureOpen()
// reopens it transparently.

class SqliteSessionStore : public QObject, public SessionStore
{
    Q_OBJECT
public:
    explicit SqliteSessionStore(const Settings& settings, QObject *parent = nullptr);
    ~SqliteSessionStore();

    SessionStoreResult commitSession(const Timeline& session) override;
    bool replaceAll(const Timeline& history, const Timeline& session) override;
    LoadResult loadDurations() override;
    EntriesForDateResult hasEntriesForDate(const QDate& date) override;
    SessionStoreResult saveCheckpoint(DurationType type, const QDateTime& startTime, const QDateTime& endTime, const SegmentId& segmentId) override;
    bool checkSchemaOnStartup() override;
    void flushToDisc() override;
    std::deque<OrphanCheckpoint> loadUnfinalizedCheckpoints() override;
    bool finalizeIfNoOverlap(qint64 rowId, const QDateTime& startUtc, const QDateTime& endUtc) override;
    ReconcileResult reconcileUnfinalizedCheckpoints(const std::vector<OrphanCheckpoint>& orphansToFinalize,
                                                   const std::vector<long long>& outrightDropIds) override;
    bool setLastCleanShutdownMarker(const QDateTime& timestamp) override;
    std::optional<MarkerResult> consumeLastCleanShutdownMarker() override;

    // Non-virtual helpers kept for test seeding and internal use.
    // No longer part of SessionStore — callers that previously used the
    // interface to call these should use commitSession instead.
    bool saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode,
                       const std::vector<QString>& removedSegmentIds = {});
    SessionStoreResult updateDurationsById(const std::deque<TimeDuration>& durations,
                                           const std::vector<QString>& removedSegmentIds = {});

private:
    QSqlDatabase db;
    uint history_days_to_keep_;

    // Guards all public entry points so that no two operations can interleave.
    // This is particularly important for createBackup(), which closes and
    // reopens the database file: the mutex prevents any concurrent call from
    // hitting a closed connection during that window.  QRecursiveMutex is used
    // because public methods may call each other (e.g. saveDurations →
    // createBackup) and we must not deadlock on re-entry.
    mutable QRecursiveMutex db_mutex_;

    bool ensureOpen();
    bool ensureSchema();
    void lazyClose();
    bool ensureSettingsTable();
    BackupResult createBackup(const std::deque<TimeDuration>& durations, TransactionMode mode);
    void performRetentionCleanup();
    void pruneOldBackups(int keepCount = 5);
    bool insertRows(QSqlQuery& query, const std::deque<TimeDuration>& rows);

#ifndef QT_NO_DEBUG
    /// Debug-build verification: checks that no segment_id appears more than
    /// once in the durations table.  Called after every successful write
    /// operation.  Violations are logged via qWarning (not fatal).
    /// The database must be open when this is called.
    void checkSegmentIdUniqueness();
#endif

};

#endif // SQLITESESSIONSTORE_H
