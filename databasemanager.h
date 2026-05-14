#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QObject>
#include <QSqlDatabase>
#include <QDateTime>
#include <QMutex>
#include <deque>
#include <vector>
#include <optional>
#include "types.h"
#include "settings.h"
#include "idatabasemanager.h"

class DatabaseManager : public QObject, public IDatabaseManager
{
    Q_OBJECT
public:
    explicit DatabaseManager(const Settings& settings, QObject *parent = nullptr);
    ~DatabaseManager();

    bool commitSession(const Timeline& session) override;
    bool replaceDurationsInDB(const std::deque<TimeDuration>& historyDurations,
                              const std::deque<TimeDuration>& currentSessionDurations) override;
    LoadResult loadDurations() override;
    EntriesForDateResult hasEntriesForDate(const QDate& date) override;
    bool saveCheckpoint(DurationType type, qint64 duration, const QDateTime& startTime, const QDateTime& endTime, const QString& segmentId) override;
    bool checkSchemaOnStartup() override;

    // Non-virtual helpers kept for test seeding and internal use.
    // No longer part of IDatabaseManager — callers that previously used the
    // interface to call these should use commitSession instead.
    bool saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode,
                       const std::vector<QString>& removedSegmentIds = {});
    bool updateDurationsById(const std::deque<TimeDuration>& durations,
                             const std::vector<QString>& removedSegmentIds = {});
    void flushToDisc() override;
    std::deque<OrphanCheckpoint> loadUnfinalizedCheckpoints() override;
    bool reconcileUnfinalizedCheckpoints(const std::vector<long long>& finalizeIds, const std::vector<long long>& dropIds) override;
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

    bool lazyOpen();
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

    // Ensures retention cleanup (DELETE of entries older than history_days_to_keep_)
    // runs at most once per application session.  Set to true after a successful
    // cleanup; left false on failure so the next lazyOpen() retries automatically.
    bool retention_cleanup_done_ = false;
};

#endif // DATABASEMANAGER_H
