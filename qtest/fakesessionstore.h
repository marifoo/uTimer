/**
 * FakeSessionStore -- in-memory test double for SessionStore.
 *
 * Records every call made to it so tests can assert which operations were
 * performed and in what order.  Return values are configurable per-method
 * so tests can simulate failures.  Data is stored in-memory (no SQLite).
 *
 * Typical usage:
 *     FakeSessionStore fakeDb;
 *     Timer tracker(settings, fakeDb);
 *     // ... exercise tracker ...
 *     QCOMPARE(fakeDb.callLog.count("saveCheckpoint"), 1);
 *     QCOMPARE(fakeDb.storedDurations.size(), 3);
 */

#ifndef FAKESESSIONSTORE_H
#define FAKESESSIONSTORE_H

#include "sessionstore.h"
#include "timeline.h"
#include <QStringList>
#include <deque>
#include <optional>
#include <vector>

class FakeSessionStore : public SessionStore
{
public:
    FakeSessionStore();
    ~FakeSessionStore() override = default;

    // ---- SessionStore interface ----

    bool commitSession(const Timeline& session) override;
    bool replaceAll(const Timeline& history, const Timeline& session) override;
    LoadResult loadDurations() override;
    EntriesForDateResult hasEntriesForDate(const QDate& date) override;
    bool saveCheckpoint(DurationType type, qint64 duration, const QDateTime& startTime,
                        const QDateTime& endTime, const QString& segmentId) override;
    bool checkSchemaOnStartup() override;
    void flushToDisc() override;
    std::deque<OrphanCheckpoint> loadUnfinalizedCheckpoints() override;
    bool finalizeIfNoOverlap(qint64 rowId, const QDateTime& startUtc, const QDateTime& endUtc) override;
    ReconcileResult reconcileUnfinalizedCheckpoints(const std::vector<OrphanCheckpoint>& orphansToFinalize,
                                                   const std::vector<long long>& outrightDropIds) override;
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

    bool commitSessionResult = true;
    bool replaceDurationsResult = true;
    bool saveCheckpointResult = true;
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

#endif // FAKESESSIONSTORE_H
