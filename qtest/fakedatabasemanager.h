/**
 * FakeDatabaseManager -- in-memory test double for IDatabaseManager.
 *
 * Records every call made to it so tests can assert which operations were
 * performed and in what order.  Return values are configurable per-method
 * so tests can simulate failures.  Data is stored in-memory (no SQLite).
 *
 * Typical usage:
 *     FakeDatabaseManager fakeDb;
 *     TimeTracker tracker(settings, fakeDb);
 *     // ... exercise tracker ...
 *     QCOMPARE(fakeDb.callLog.count("saveCheckpoint"), 1);
 *     QCOMPARE(fakeDb.storedDurations.size(), 3);
 */

#ifndef FAKEDATABASEMANAGER_H
#define FAKEDATABASEMANAGER_H

#include "idatabasemanager.h"
#include <QStringList>
#include <deque>
#include <optional>
#include <vector>

class FakeDatabaseManager : public IDatabaseManager
{
public:
    FakeDatabaseManager();
    ~FakeDatabaseManager() override = default;

    // ---- IDatabaseManager interface ----

    bool saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode,
                       const std::vector<QString>& removedSegmentIds = {}) override;
    bool replaceDurationsInDB(const std::deque<TimeDuration>& historyDurations,
                              const std::deque<TimeDuration>& currentSessionDurations) override;
    LoadResult loadDurations() override;
    EntriesForDateResult hasEntriesForDate(const QDate& date) override;
    bool saveCheckpoint(DurationType type, qint64 duration, const QDateTime& startTime,
                        const QDateTime& endTime, const QString& segmentId) override;
    bool updateDurationsById(const std::deque<TimeDuration>& durations,
                             const std::vector<QString>& removedSegmentIds = {}) override;
    bool checkSchemaOnStartup() override;
    void flushToDisc() override;
    std::deque<OrphanCheckpoint> loadUnfinalizedCheckpoints() override;
    bool reconcileUnfinalizedCheckpoints(const std::vector<long long>& finalizeIds,
                                          const std::vector<long long>& dropIds) override;
    bool setLastCleanShutdownMarker(const QDateTime& timestamp) override;
    std::optional<QDateTime> consumeLastCleanShutdownMarker() override;

    // ---- Call log (for assertions) ----

    /// Ordered log of method names invoked, e.g. "saveDurations", "saveCheckpoint".
    QStringList callLog;

    // ---- In-memory data store ----

    /// All stored durations (populated by saveDurations/updateDurationsById).
    std::deque<TimeDuration> storedDurations;

    /// Last checkpoint saved (for inspection).
    struct CheckpointRecord {
        DurationType type;
        qint64 duration;
        QDateTime startTime;
        QDateTime endTime;
        QString segmentId;
    };
    std::vector<CheckpointRecord> savedCheckpoints;

    // ---- Configurable return values ----

    bool saveDurationsResult = true;
    bool replaceDurationsResult = true;
    bool saveCheckpointResult = true;
    bool updateDurationsByIdResult = true;
    bool checkSchemaResult = true;
    bool reconcileResult = true;
    bool setMarkerResult = true;

    /// Pre-loaded durations returned by loadDurations(). Tests populate this.
    LoadResult loadDurationsResult;

    /// Pre-loaded orphans returned by loadUnfinalizedCheckpoints().
    std::deque<OrphanCheckpoint> orphanCheckpoints;

    /// Result returned by hasEntriesForDate(). Default: No entries.
    EntriesForDateResult entriesForDateResult = EntriesForDateResult::No;

    /// Marker returned (once) by consumeLastCleanShutdownMarker().
    std::optional<QDateTime> cleanShutdownMarker;
};

#endif // FAKEDATABASEMANAGER_H
