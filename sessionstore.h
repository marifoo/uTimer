/**
 * SessionStore — Abstract interface for time-duration persistence.
 *
 * Extracted from the concrete SqliteSessionStore class so that Timer
 * (and anything else that needs DB access) can depend on an abstraction
 * rather than the SQLite implementation.  This enables:
 *
 *   - FakeSessionStore in tests: records every call, returns
 *     configurable success/failure, stores data in memory (no SQLite).
 *   - Easier reasoning about which DB operations Timer actually needs.
 *
 * Every public method that Timer calls on SqliteSessionStore is
 * represented here as a pure virtual.  Internal helpers (lazyOpen,
 * createBackup, etc.) remain private implementation details of the
 * concrete class.
 */

#ifndef SESSIONSTORE_H
#define SESSIONSTORE_H

#include <QDateTime>
#include <QString>
#include <deque>
#include <optional>
#include <vector>
#include "types.h"
#include "timeline.h"

/**
 * Forward declaration of the OrphanCheckpoint and LoadResult types.
 * These are defined here (not inside the interface) so that both the
 * interface and the concrete class share the same types without
 * requiring SqliteSessionStore to be included.
 */
struct OrphanCheckpoint {
    long long id = -1;
    SegmentId segment_id;
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

/**
 * Result of reconcileUnfinalizedCheckpoints.
 *
 *  - finalized: row ids that were successfully promoted from is_finalized=0 to 1.
 *  - dropped:   row ids that the store would not adopt.  Most commonly because
 *               another already-finalised row overlaps the orphan's
 *               [startUtc, endUtc) interval; also covers missing/already-final
 *               rows.  These rows are left in the database (still is_finalized=0
 *               if they exist) so a future reconciliation pass can re-evaluate.
 *  - ok:        false iff a transaction-level failure prevented the call from
 *               doing meaningful work.  When false the two lists are empty and
 *               the caller must assume nothing happened.
 */
struct ReconcileResult {
    std::vector<long long> finalized;
    std::vector<long long> dropped;
    bool ok = true;
};

/**
 * Tri-state result of consumeLastCleanShutdownMarker().
 *
 * Status semantics:
 *   Found    — marker was present; `timestamp` holds its value.
 *   NotFound — DB was queried successfully; no marker existed.
 *   Error    — a transaction or query failure occurred; state is unknown.
 *              Callers should refuse to reconcile orphan checkpoints on Error
 *              because doing so on unknown state may overwrite valid data.
 *
 * The outer std::optional<MarkerResult> on consumeLastCleanShutdownMarker()
 * is std::nullopt only when history storage is entirely disabled
 * (history_days_to_keep_ == 0).  All other outcomes are expressed via Status.
 */
struct MarkerResult {
    enum class Status { Found, NotFound, Error };
    QDateTime timestamp;  // valid only when status == Found
    Status status = Status::NotFound;
};

/**
 * Typed result returned by SessionStore operations that write to the database.
 *
 * Category semantics:
 *   Success       — operation completed successfully.
 *   TransientError — a recoverable failure (e.g. SQL execution error); the
 *                   caller may retry.
 *   CallerBug     — invalid input supplied by the caller (e.g. empty segment_id);
 *                   retrying with the same arguments will not help. Log as critical.
 *   FatalError    — unrecoverable failure (e.g. DB connection lost); the caller
 *                   should emit a user warning and refuse new sessions.
 */
struct SessionStoreResult {
    enum Category { Success, TransientError, CallerBug, FatalError };
    Category category;
    QString message;
    bool ok() const { return category == Success; }
    static SessionStoreResult success() { return {Success, {}}; }
    static SessionStoreResult transient(QString msg) { return {TransientError, std::move(msg)}; }
    static SessionStoreResult callerBug(QString msg) { return {CallerBug, std::move(msg)}; }
    static SessionStoreResult fatal(QString msg) { return {FatalError, std::move(msg)}; }
};

class SessionStore
{
public:
    virtual ~SessionStore() = default;

    /// @pre All segment_ids in the timeline must be non-empty
    virtual SessionStoreResult commitSession(const Timeline& session) = 0;
    virtual bool replaceAll(const Timeline& history, const Timeline& session) = 0;
    virtual LoadResult loadDurations() = 0;
    // Returns Yes iff at least one finalised row's end falls on `localDate`
    // interpreted in the system local time zone.
    virtual EntriesForDateResult hasEntriesForDate(const QDate& localDate) = 0;
    /// @pre segmentId must be non-empty
    virtual SessionStoreResult saveCheckpoint(DurationType type, qint64 duration, const QDateTime& startTime,
                                              const QDateTime& endTime, const SegmentId& segmentId) = 0;
    virtual bool checkSchemaOnStartup() = 0;
    virtual void flushToDisc() = 0;
    virtual std::deque<OrphanCheckpoint> loadUnfinalizedCheckpoints() = 0;
    // Atomically check overlap and finalise.  Inside a single transaction:
    //   (a) probe for any other finalised row whose [startUtc, endUtc) interval
    //       intersects the supplied [startUtc, endUtc);
    //   (b) only if no overlap, UPDATE `rowId` to is_finalized = 1.
    // Returns true iff the UPDATE took effect.
    virtual bool finalizeIfNoOverlap(qint64 rowId, const QDateTime& startUtc, const QDateTime& endUtc) = 0;
    // Reconcile a batch of orphan checkpoints.
    //   `orphansToFinalize` is attempted via finalizeIfNoOverlap, one per row.
    //   `outrightDropIds`   are deleted unconditionally.
    // See ReconcileResult for the return-value contract.
    virtual ReconcileResult reconcileUnfinalizedCheckpoints(const std::vector<OrphanCheckpoint>& orphansToFinalize,
                                                            const std::vector<long long>& outrightDropIds) = 0;
    virtual bool setLastCleanShutdownMarker(const QDateTime& timestamp) = 0;
    // Reads and deletes the last-clean-shutdown marker in a single transaction.
    // Returns nullopt only when history storage is disabled.  All other outcomes
    // (found, not found, error) are expressed via MarkerResult::Status.
    virtual std::optional<MarkerResult> consumeLastCleanShutdownMarker() = 0;
};

#endif // SESSIONSTORE_H
