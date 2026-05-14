/**
 * IDatabaseManager — Abstract interface for time-duration persistence.
 *
 * Extracted from the concrete DatabaseManager class so that TimeTracker
 * (and anything else that needs DB access) can depend on an abstraction
 * rather than the SQLite implementation.  This enables:
 *
 *   - FakeDatabaseManager in tests: records every call, returns
 *     configurable success/failure, stores data in memory (no SQLite).
 *   - Easier reasoning about which DB operations TimeTracker actually needs.
 *
 * Every public method that TimeTracker calls on DatabaseManager is
 * represented here as a pure virtual.  Internal helpers (lazyOpen,
 * createBackup, etc.) remain private implementation details of the
 * concrete class.
 */

#ifndef IDATABASEMANAGER_H
#define IDATABASEMANAGER_H

#include <QDateTime>
#include <deque>
#include <optional>
#include <vector>
#include "types.h"
#include "timeline.h"

/**
 * Forward declaration of the OrphanCheckpoint and LoadResult types.
 * These are defined here (not inside the interface) so that both the
 * interface and the concrete class share the same types without
 * requiring DatabaseManager to be included.
 */
struct OrphanCheckpoint {
    long long id = -1;
    QString segment_id;
    DurationType type = DurationType::Activity;
    qint64 duration = 0;
    QDateTime startTime;
    QDateTime endTime;
};

struct LoadResult {
    std::deque<TimeDuration> durations;
    int skipped = 0;
    int repaired = 0;

    size_t size() const { return durations.size(); }
    bool empty() const { return durations.empty(); }
    const TimeDuration& operator[](size_t idx) const { return durations[idx]; }
    TimeDuration& operator[](size_t idx) { return durations[idx]; }
    std::deque<TimeDuration>::const_iterator begin() const { return durations.begin(); }
    std::deque<TimeDuration>::const_iterator end() const { return durations.end(); }
    std::deque<TimeDuration>::iterator begin() { return durations.begin(); }
    std::deque<TimeDuration>::iterator end() { return durations.end(); }
    operator const std::deque<TimeDuration>&() const { return durations; }
    operator std::deque<TimeDuration>&() { return durations; }
};

class IDatabaseManager
{
public:
    virtual ~IDatabaseManager() = default;

    virtual bool commitSession(const Timeline& session) = 0;
    virtual bool saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode,
                               const std::vector<QString>& removedSegmentIds = {}) = 0;
    virtual bool replaceDurationsInDB(const std::deque<TimeDuration>& historyDurations,
                                      const std::deque<TimeDuration>& currentSessionDurations) = 0;
    virtual LoadResult loadDurations() = 0;
    virtual EntriesForDateResult hasEntriesForDate(const QDate& date) = 0;
    virtual bool saveCheckpoint(DurationType type, qint64 duration, const QDateTime& startTime,
                                const QDateTime& endTime, const QString& segmentId) = 0;
    virtual bool updateDurationsById(const std::deque<TimeDuration>& durations,
                                     const std::vector<QString>& removedSegmentIds = {}) = 0;
    virtual bool checkSchemaOnStartup() = 0;
    virtual void flushToDisc() = 0;
    virtual std::deque<OrphanCheckpoint> loadUnfinalizedCheckpoints() = 0;
    virtual bool reconcileUnfinalizedCheckpoints(const std::vector<long long>& finalizeIds,
                                                  const std::vector<long long>& dropIds) = 0;
    virtual bool setLastCleanShutdownMarker(const QDateTime& timestamp) = 0;
    virtual std::optional<QDateTime> consumeLastCleanShutdownMarker() = 0;
};

#endif // IDATABASEMANAGER_H
