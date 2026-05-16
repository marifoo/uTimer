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

// Connection lifecycle: the database is opened in the constructor
// (initializeNewConnection runs schema migrations, indexes, PRAGMA, and retention
// cleanup exactly once) and closed in the destructor.  All public methods call
// ensureOpen() as a lightweight health check; if the connection is unexpectedly
// lost, ensureOpen() reopens it without re-running migrations.

class SqliteSessionStore : public QObject, public SessionStore
{
    Q_OBJECT
public:
    explicit SqliteSessionStore(const Settings& settings, QObject *parent = nullptr);
    ~SqliteSessionStore();

    bool commitSession(const Timeline& session) override;
    bool replaceAll(const Timeline& history, const Timeline& session) override;
    LoadResult loadDurations() override;
    EntriesForDateResult hasEntriesForDate(const QDate& date) override;
    bool saveCheckpoint(DurationType type, qint64 duration, const QDateTime& startTime, const QDateTime& endTime, const QString& segmentId) override;
    bool checkSchemaOnStartup() override;

    // Non-virtual helpers kept for test seeding and internal use.
    // No longer part of SessionStore — callers that previously used the
    // interface to call these should use commitSession instead.
    bool saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode,
                       const std::vector<QString>& removedSegmentIds = {});
    bool updateDurationsById(const std::deque<TimeDuration>& durations,
                             const std::vector<QString>& removedSegmentIds = {});
    void flushToDisc() override;
    std::deque<OrphanCheckpoint> loadUnfinalizedCheckpoints() override;
    bool finalizeIfNoOverlap(qint64 rowId, const QDateTime& startUtc, const QDateTime& endUtc) override;
    ReconcileResult reconcileUnfinalizedCheckpoints(const std::vector<OrphanCheckpoint>& orphansToFinalize,
                                                   const std::vector<long long>& outrightDropIds) override;
    bool setLastCleanShutdownMarker(const QDateTime& timestamp) override;
    std::optional<QDateTime> consumeLastCleanShutdownMarker() override;

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
    bool initializeNewConnection();
    void lazyClose();
    bool validateSchema();
    bool ensureIsFinalizedColumn();
    bool ensureSegmentIdColumn();
    bool ensureSettingsTable();
    bool createBackup(const std::deque<TimeDuration>& durations, TransactionMode mode);

#ifndef QT_NO_DEBUG
    /// Debug-build verification: checks that no segment_id appears more than
    /// once in the durations table.  Called after every successful write
    /// operation.  Violations are logged via qWarning (not fatal).
    /// The database must be open when this is called.
    void checkSegmentIdUniqueness();
#endif

    // Tracks whether retention cleanup ran during this process lifetime.
    // Set to true on success; left false on failure (no automatic retry — Step 7
    // moves this to checkSchemaOnStartup() where it can be called explicitly).
    bool retention_cleanup_done_ = false;
};

#endif // SQLITESESSIONSTORE_H
