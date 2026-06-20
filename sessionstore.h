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
 * represented here as a pure virtual.  Internal helpers (ensureOpen,
 * ensureSchema, createBackup, etc.) remain private implementation details
 * of the concrete class.
 */

#ifndef SESSIONSTORE_H
#define SESSIONSTORE_H

#include <QDateTime>
#include <QString>
#include <deque>
#include "types.h"
#include "timeline.h"

/**
 * Forward declaration of LoadResult type.
 * Defined here so both the interface and the concrete class share the same
 * type without requiring SqliteSessionStore to be included.
 */
struct LoadResult {
    std::deque<TimeDuration> durations;
    int skipped = 0;
    int repaired = 0;

    size_t size() const { return durations.size(); }
    bool empty() const { return durations.empty(); }
    const TimeDuration& operator[](size_t idx) const { return durations[idx]; }
    std::deque<TimeDuration>::const_iterator begin() const { return durations.begin(); }
    std::deque<TimeDuration>::const_iterator end() const { return durations.end(); }
};

/**
 * Result returned by recoverStartupCheckpoints().
 *
 * The store loads unfinalized checkpoint rows, applies the too-short (<1 s) /
 * stale (>24 h) and overlap policies, finalizes qualifying rows, and decides
 * whether the user needs to be notified of unclean recovery.
 *
 *   ok == true  — recovery ran to completion (even if zero rows were finalized).
 *   ok == false — a critical store failure (e.g. marker read error) prevented
 *                 reconciliation; no rows were mutated.  Timer must treat
 *                 recovered_seconds as 0 and must NOT show a notification.
 *
 * When history storage is disabled (history_days_to_keep_ == 0) the store
 * returns ok=true with all counts zero and notify_user false.
 */
struct StartupRecoveryResult {
    qint64 recovered_seconds = 0;
    int finalized_count = 0;
    int dropped_count = 0;
    bool notify_user = false;
    bool ok = true;
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

/**
 * Result of checkSchemaOnStartup().
 *
 *   Ready        — existing DB has the expected schema; safe to use.
 *   Created      — DB file did not exist; a fresh schema was built.
 *   Outdated     — existing DB has wrong columns or missing/non-unique constraints;
 *                  caller must refuse to start and ask the user to remove the file.
 *   Inaccessible — DB file could not be opened (permissions, I/O error);
 *                  caller must refuse to start.
 */
enum class SchemaStatus { Ready, Created, Outdated, Inaccessible };

class SessionStore
{
public:
    virtual ~SessionStore() = default;

    /// @pre All segment_ids in the timeline must be non-empty
    virtual SessionStoreResult commitSession(const Timeline& session) = 0;
    virtual bool replaceAll(const Timeline& history, const Timeline& session) = 0;
    virtual LoadResult loadDurations() = 0;
    // Returns Yes iff at least one finalised row's start falls on `localDate`
    // interpreted in the system local time zone.
    virtual EntriesForDateResult hasEntriesForDate(const QDate& localDate) = 0;
    /// Writes or extends an unfinalized checkpoint row identified by segmentId.
    /// Invariant: checkpoint writes must never demote finalized history. If segmentId
    /// already belongs to a finalized row, the call returns CallerBug and leaves the
    /// row unchanged. Only unfinalized rows may be created or updated by this method.
    /// @pre segmentId must be non-empty
    virtual SessionStoreResult saveCheckpoint(DurationType type, const QDateTime& startTime,
                                              const QDateTime& endTime, const SegmentId& segmentId) = 0;
    virtual SchemaStatus checkSchemaOnStartup() = 0;
    virtual void flushToDisc() = 0;
    virtual bool setLastCleanShutdownMarker(const QDateTime& timestamp) = 0;
    /// Recovers unfinalized checkpoint rows left by a previous crash or unclean exit.
    /// Internally consumes the last-clean-shutdown marker, applies too-short/stale/
    /// overlap policies, and finalizes qualifying rows.  Returns domain facts only —
    /// row ids and finalize/drop mechanics are hidden inside the store.
    /// `now` is used for the stale-age check (>24 h) and should be the current time.
    virtual StartupRecoveryResult recoverStartupCheckpoints(const QDateTime& now) = 0;
};

#endif // SESSIONSTORE_H
