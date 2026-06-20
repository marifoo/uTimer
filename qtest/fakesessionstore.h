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
#include <QSet>
#include <QStringList>
#include <deque>
#include <vector>

class FakeSessionStore : public SessionStore
{
public:
    FakeSessionStore();
    ~FakeSessionStore() override = default;

    // ---- SessionStore interface ----

    SessionStoreResult commitSession(const Timeline& session) override;
    bool replaceAll(const Timeline& history, const Timeline& session) override;
    LoadResult loadDurations() override;
    EntriesForDateResult hasEntriesForDate(const QDate& date) override;
    SessionStoreResult saveCheckpoint(DurationType type, const QDateTime& startTime,
                                      const QDateTime& endTime, const SegmentId& segmentId) override;
    SchemaStatus checkSchemaOnStartup() override;
    void flushToDisc() override;
    bool setLastCleanShutdownMarker(const QDateTime& timestamp) override;
    StartupRecoveryResult recoverStartupCheckpoints(const QDateTime& now) override;

    // ---- Call log (for assertions) ----

    /// Ordered log of method names invoked, e.g. "saveCheckpoint".
    QStringList callLog;

    // ---- In-memory data store ----

    /// All stored durations (populated by saveDurations/updateDurationsById).
    std::deque<TimeDuration> storedDurations;

    /// Last checkpoint saved (for inspection).
    struct CheckpointRecord {
        DurationType type;
        QDateTime startTime;
        QDateTime endTime;
        SegmentId segmentId;
    };
    std::vector<CheckpointRecord> savedCheckpoints;

    // ---- Configurable return values ----

    SessionStoreResult commitSessionResult = SessionStoreResult::success();
    bool replaceDurationsResult = true;
    SessionStoreResult saveCheckpointResult = SessionStoreResult::success();
    SchemaStatus checkSchemaResult = SchemaStatus::Ready;
    bool setMarkerResult = true;

    /// Set of segment_ids that have been committed via commitSession().
    /// Used to enforce UNIQUE(segment_id): duplicate submissions Q_ASSERT-fail.
    QSet<QString> committedSegmentIds;

    /// Pre-loaded durations returned by loadDurations(). Tests populate this.
    LoadResult loadDurationsResult;

    /// Result returned by hasEntriesForDate(). Default: No entries.
    EntriesForDateResult entriesForDateResult = EntriesForDateResult::No;

    /// Result returned by recoverStartupCheckpoints().
    /// Default: success with zero recovery and no notification.
    StartupRecoveryResult startupRecoveryResult;
};

#endif // FAKESESSIONSTORE_H
