#ifndef TIMETRACKER_H
#define TIMETRACKER_H

#include <QObject>
#include <QtGlobal>
#include <QElapsedTimer>
#include <QDateTime>
#include <QMutex>
#include <QTimer>
#include <optional>
#include <deque>
#include <memory>
#include <utility>
#include <vector>
#include "settings.h"
#include "types.h"
#include "idatabasemanager.h"

/**
 * SessionState — groups the mutable per-session data that TimeTracker manages.
 *
 * Previously these fields were scattered as raw members on TimeTracker,
 * making it easy for code paths (notably addDurationWithMidnightSplit) to
 * mutate them as side effects without the caller being aware. Grouping them
 * into a struct with explicit transition methods ensures:
 *   - Every mutation is named and logged (in debug builds)
 *   - State changes are traceable during debugging
 *   - Pure helper functions can compute results without touching the struct,
 *     and the caller applies changes explicitly
 *
 * The struct is intentionally not a class — TimeTracker is its sole owner
 * and accesses fields directly. The transition methods are convenience
 * helpers that enforce logging discipline, not access control.
 */
struct SessionState {
    /// Completed duration segments for the current session (in-memory).
    std::deque<TimeDuration> durations;

    /// Cached copy of durations that still need DB append-retry after a
    /// failed save. Empty when there is nothing to retry.
    std::deque<TimeDuration> unsaved_durations;

    /// Segment identity (UUID) currently used by checkpoint saves. Empty
    /// when the timer is stopped (Mode::None). Rotated on every mode
    /// transition so each segment gets its own DB row.
    QString current_checkpoint_segment_id;

    /// Wall-clock time when the current segment started. Invalid when the
    /// timer is stopped. Used together with "now" to compute the ongoing
    /// segment's duration.
    QDateTime segment_start_time;

    /// True when a previous DB save failed and the data in durations (or
    /// unsaved_durations) still needs to be persisted.
    bool has_unsaved_data = false;

    // ---- Explicit transition methods ----
    // Each method logs the old→new state at debug level so that state
    // mutations are always traceable in the log output.

    /// Starts a new segment: assigns a fresh segment_id and records the
    /// wall-clock start time.
    void beginNewSegment(const QDateTime& startTime, const Settings& settings);

    /// Clears the segment_id and start time (used when the timer stops).
    void clearSegment(const Settings& settings);

    /// Updates segment_start_time (e.g. after midnight split or pause).
    void updateSegmentStartTime(const QDateTime& newStart, const Settings& settings);

    /// Marks unsaved data state after a failed DB write.
    void markUnsaved(const Settings& settings);

    /// Clears the unsaved flag and the retry cache after a successful save
    /// or a fresh start.
    void clearUnsaved(const Settings& settings);

    /// Clears durations and unsaved state (fresh start).
    void resetForNewSession(const Settings& settings);

    /// Adopts the segment identity from an externally-provided ongoing
    /// duration (used when HistoryDialog replaces durations).
    void adoptOngoingSegment(const TimeDuration& ongoing, const Settings& settings);
};

/**
 * Result of splitting a duration at midnight boundaries.
 *
 * addDurationWithMidnightSplit was previously a side-effectful method that
 * mutated durations_, has_unsaved_data_, unsaved_durations_, and
 * segment_start_time_ directly. This struct captures its output so the
 * caller can apply the changes explicitly.
 */
struct MidnightSplitResult {
    /// New duration entries to append to durations.
    std::deque<TimeDuration> new_entries;

    /// Entries that belong to the previous day and should be saved to DB
    /// immediately (non-empty only when a midnight boundary was crossed).
    std::deque<TimeDuration> previous_day_entries;

    /// When a midnight crossing occurs, the segment_start_time must be
    /// updated to the start of the new day. std::nullopt when no update
    /// is needed.
    std::optional<QDateTime> updated_segment_start_time;

    /// True if a midnight boundary was crossed (triggers DB save of
    /// previous-day data).
    bool crossed_midnight = false;
};

class TimeTracker : public QObject
{
    Q_OBJECT
private:
    enum class Mode {Activity, Pause, None};

    const Settings & settings_;
    QElapsedTimer timer_;
    SessionState session_;
    Mode mode_;
    bool was_active_before_autopause_;
    bool is_locked_;  // Track if desktop is currently locked (to prevent checkpoints while locked)
    bool checkpoints_paused_;  // Track if checkpoints are paused (e.g., while HistoryDialog is open)
    IDatabaseManager& db_;
    mutable QRecursiveMutex mutex_;  // Protects state transitions from concurrent access
    QTimer checkpointTimer_;  // Timer for periodic checkpoint saving
    qint64 checkpoint_interval_msec_; // Checkpoint interval (0 = disabled)
    int last_history_load_skipped_;
    int last_history_load_repaired_;
    qint64 startup_recovered_seconds_;
    bool startup_recovery_notification_needed_;

    void startTimer(const QDateTime& now);
    void stopTimer(const QDateTime& now);
    void pauseTimer(const QDateTime& now);
    void backpauseTimer(const QDateTime& now);
    void finalizeActivityToPause(const QDateTime& pauseSegmentStart);
    MidnightSplitResult computeMidnightSplit(DurationType type, const QDateTime& startTime, const QDateTime& endTime, const QString& segmentId = QString()) const;
    void applyMidnightSplit(const MidnightSplitResult& result);
    void addDurationWithMidnightSplit(DurationType type, const QDateTime& startTime, const QDateTime& endTime, const QString& segmentId = QString());
    void saveCheckpointInternal(const QDateTime& now);  // Internal checkpoint save (called when mutex already held)
    bool appendDurationsChunkToDB(const std::deque<TimeDuration>& durations);
    qint64 reconcileOrphanCheckpoints(
        const std::deque<OrphanCheckpoint>& orphans,
        const std::optional<QDateTime>& cleanShutdownMarker);

#ifndef QT_NO_DEBUG
    /// Debug-build state snapshot used by StateGuard to detect unlogged mutations.
    struct StateSnapshot {
        size_t durations_size;
        QString segment_id;
        QDateTime segment_start_time;
        bool has_unsaved_data;
    };
    StateSnapshot takeStateSnapshot() const;

    /// RAII guard that captures state on entry to a public method and checks
    /// for unexpected mutations on exit. Only active in debug builds.
    class StateGuard {
    public:
        StateGuard(const TimeTracker& tracker, const char* methodName);
        ~StateGuard();
        /// Call this to acknowledge that the method intentionally modified state.
        void markTransitioned();
    private:
        const TimeTracker& tracker_;
        const char* method_;
        StateSnapshot entry_;
        bool transitioned_ = false;
    };

    /// Checks structural invariants on session_.durations:
    ///   - Segment ordering: startTimes are non-decreasing
    ///   - No overlapping segments of the same type on the same day
    ///   - All segment_ids are non-empty
    /// Violations are logged via qWarning (not fatal) so they are visible
    /// in test output and debug runs without crashing the application.
    void checkDurationInvariants() const;
#endif // QT_NO_DEBUG

public:
    explicit TimeTracker(const Settings & settings, IDatabaseManager& db, QObject *parent = nullptr);
    ~TimeTracker();

    qint64 getActiveTime() const;
    qint64 getPauseTime() const;
    const std::deque<TimeDuration>& getCurrentDurations() const;
    void replaceCurrentDurations(const std::deque<TimeDuration>& newDurations,
                                 const std::optional<TimeDuration>& ongoing = std::nullopt);
    std::deque<TimeDuration> getDurationsHistory();
    std::pair<int, int> getLastHistoryLoadStats() const;
    std::optional<TimeDuration> getOngoingDuration() const;
    qint64 getStartupRecoveredSeconds() const;
    bool shouldShowStartupRecoveryNotification() const;
    void setDurationType(size_t idx, DurationType type);
    bool appendDurationsToDB();
    bool updateDurationsInDB();
    bool replaceDurationsInDB(std::deque<TimeDuration> historyDurations,
                              std::deque<TimeDuration> currentSessionDurations);
    EntriesForDateResult hasEntriesForDate(const QDate& date);
    void pauseCheckpoints();
    void resumeCheckpoints();
    bool checkDatabaseSchema(); // Returns true if DB schema is valid, false if outdated
    void flushDatabaseToDisc(); // Force pending writes to disk (for shutdown safety)
    bool markCleanShutdown();
    bool canMarkCleanShutdown() const;

public slots:
    void useTimerViaButton(Button button);
    void useTimerViaLockEvent(LockEvent event);

signals:
    void userWarning(const QString& text);

private slots:
    void saveCheckpoint();  // Periodic checkpoint saving every 5 minutes
};

#endif // TIMETRACKER_H
