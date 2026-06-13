#ifndef TIMER_H
#define TIMER_H

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
#include "sessionstore.h"
#include "timeline.h"

/**
 * SessionState — groups the mutable per-session data that Timer manages.
 *
 * Previously these fields were scattered as raw members on Timer,
 * making it easy for code paths to mutate them as side effects without the
 * caller being aware. Grouping them into a struct with explicit transition
 * methods ensures:
 *   - Every mutation is named and logged (in debug builds)
 *   - State changes are traceable during debugging
 *   - Callers like addDuration can compute results without touching the struct
 *     directly, and the caller applies changes explicitly
 *
 * The struct is intentionally not a class — Timer is its sole owner
 * and accesses fields directly. The transition methods are convenience
 * helpers that enforce logging discipline, not access control.
 */
struct SessionState {
    /// Completed duration segments for the current session (in-memory).
    std::deque<TimeDuration> durations;

    /// Cached copy of durations that still need DB append-retry after a
    /// failed save. Empty when there is nothing to retry.
    std::deque<TimeDuration> unsaved_durations;

    /// Segment identity for checkpoint saves.  Empty when the timer
    /// is stopped (Mode::None).  Rotated on every mode transition so each
    /// segment gets its own DB row.
    SegmentId segment_id;

    /// Wall-clock time when the current segment started. Invalid when the
    /// timer is stopped. Used together with "now" to compute the ongoing
    /// segment's duration.
    QDateTime segment_start_time;

    /// True when a previous DB save failed and the data in durations (or
    /// unsaved_durations) still needs to be persisted.
    bool has_unsaved_data = false;

    /// Counts consecutive retry failures at the top of startTimer (None → Activity).
    /// Reset to 0 on first success. When it reaches 3 the engine refuses to start
    /// a new session until the user acknowledges the warning.
    int consecutive_retry_failures = 0;

    // ---- Explicit transition methods ----
    // Each method logs the old→new state at debug level so that state
    // mutations are always traceable in the log output.

    /// Starts a new segment: assigns a fresh segment_id and records the
    /// wall-clock start time.
    void beginNewSegment(const QDateTime& startTime);

    /// Clears the segment_id and start time (used when the timer stops).
    void clearSegment();

    /// Updates segment_start_time (e.g. after a pause transition).
    void updateSegmentStartTime(const QDateTime& newStart);

    /// Marks unsaved data state after a failed DB write.
    void markUnsaved();

    /// Clears the unsaved flag and the retry cache after a successful save
    /// or a fresh start.
    void clearUnsaved();

    /// Clears durations and unsaved state (fresh start).
    void resetForNewSession();

    /// Adopts the segment identity from an externally-provided ongoing
    /// duration (used when HistoryDialog replaces durations).
    void adoptOngoingSegment(const TimeDuration& ongoing);
};


class Timer : public QObject
{
    Q_OBJECT
public:
    /// Reason a stop was initiated. Carried by the stopped() signal so
    /// observers (e.g. MainWin) can distinguish user-driven from engine-driven stops.
    enum class StopReason {
        ButtonStop,         ///< User pressed the Stop button.
        MidnightScheduled,  ///< DayBoundaryWatcher scheduled timer fired at 23:59:59.500.
        MidnightWatchdog,   ///< Watchdog detected a cross-midnight ongoing segment.
        Shutdown,           ///< Application shutdown (destructor or ShutdownCoordinator).
        EditApplied,        ///< Reserved for future use (history edit that stops the engine).
    };

    /// Cause of a pause/resume transition. Carried by modeChanged() so MainWin
    /// can distinguish lock-driven autopause from user-driven pause.
    enum class PauseCause {
        UserAction,    ///< User pressed Pause or Start.
        LockAutopause, ///< Desktop locked and backpause triggered.
        LockResume,    ///< Desktop unlocked and timer was auto-resumed.
    };

private:
    enum class Mode {Activity, Pause, None};

    /**
     * DayBoundaryWatcher — owns the 23:59:59.500 single-shot QTimer and the
     * cross-midnight watchdog logic.
     *
     * T5.1: skeleton with tick(). Arms/cancels the internal timer.
     * T5.2: timer fires and calls the engine's stop path.
     * T5.3: tick() contains the watchdog poll (moved from MainWin::update()).
     */
    class DayBoundaryWatcher {
    public:
        explicit DayBoundaryWatcher(Timer& owner);

        /// Called on every 100 ms heartbeat. Runs the cross-midnight watchdog.
        void tick(const QDateTime& now);

        /// Arms the single-shot timer to fire at 23:59:59.500 today.
        void armScheduledStop(const QDateTime& now);

        /// Cancels the single-shot timer (call on any stop).
        void cancel();

    private:
        Timer& owner_;
        QTimer midnight_timer_;

        void onMidnightTimerFired();
    };

    const Settings & settings_;
    QElapsedTimer timer_;
    SessionState session_;
    Mode mode_;
    bool autopause_pending_resume_;
    bool is_locked_;     // Track if desktop is currently locked (to prevent checkpoints while locked)
    bool dialog_open_;   // True while HistoryDialog is open (suspends mutation)
    bool pending_midnight_stop_ = false;
    LockEvent pending_lock_event_ = LockEvent::None;
    SessionStore& db_;
    mutable QRecursiveMutex mutex_;  // Protects state transitions from concurrent access
    QTimer checkpointTimer_;  // Timer for periodic checkpoint saving
    qint64 checkpoint_interval_msec_; // Checkpoint interval (0 = disabled)
    int last_history_load_skipped_;
    int last_history_load_repaired_;
    qint64 startup_recovered_seconds_;
    bool startup_recovery_notification_needed_;
    DayBoundaryWatcher day_boundary_watcher_; // Must be last: constructed after all other members

    void startTimer(const QDateTime& now);
    void stopTimer(const QDateTime& now, StopReason reason);
    void pauseTimer(const QDateTime& now);
    void backpauseTimer(const QDateTime& now);
    // Appends a single same-day segment to session_.durations.
    // Silently discards segments where startTime.date() != endTime.date()
    // (last-line defence for cross-midnight). DayBoundaryWatcher's scheduled
    // stop and watchdog are supposed to make cross-midnight inputs unreachable.
    void addDuration(DurationType type,
                     const QDateTime& startTime,
                     const QDateTime& endTime,
                     SegmentId segmentId = SegmentId{});

    // If the ongoing segment is cross-midnight, forces the engine into None,
    // discarding the in-flight segment. Returns true when it fired.
    // Safe to call multiple times; only the first call does real work.
    bool discardCrossMidnightOngoingAndStop(const QDateTime& now);
    EntriesForDateResult hasEntriesForDate(const QDate& date);
    void saveCheckpointInternal(const QDateTime& now);  // Internal checkpoint save (called when mutex already held)
    SessionStoreResult appendDurationsChunkToDB(const std::deque<TimeDuration>& durations);
    /// Applies the standard log/warn policy for a SessionStoreResult in the
    /// named context.  Does NOT affect control flow — callers decide what to do
    /// after calling this.
    void handleDbResult(const SessionStoreResult& result, const QString& context);
    // Retries saving previously unsaved durations at the start of a new session.
    // Returns true if startTimer should continue, false if it should abort.
    bool retryUnsavedDurations();
    qint64 reconcileOrphanCheckpoints(
        const std::deque<OrphanCheckpoint>& orphans,
        const std::optional<MarkerResult>& markerResult);
    void maybeReanchorCheckpoint(const TimeDuration& seg);

#ifndef QT_NO_DEBUG
    /// Debug-build state snapshot used by StateGuard to detect unlogged mutations.
    struct StateSnapshot {
        size_t durations_size;
        SegmentId segment_id;
        QDateTime segment_start_time;
        bool has_unsaved_data;
        Mode mode;
    };
    StateSnapshot takeStateSnapshot() const;

    /// RAII guard that captures state on entry to a public method and checks
    /// for unexpected mutations on exit. Only active in debug builds.
    class StateGuard {
    public:
        StateGuard(const Timer& tracker, const char* methodName);
        ~StateGuard();
        /// Call this to acknowledge that the method intentionally modified state.
        void markTransitioned();
    private:
        const Timer& tracker_;
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

    /// Cross-layer invariant check: for each segment in session_.durations,
    /// queries the DB and asserts either no row exists for that segment_id, or
    /// exactly one finalized row with matching fields. Called after successful
    /// db_.commitSession() calls in updateDurationsInDB().
    void checkCrossLayerInvariants() const;
#endif // QT_NO_DEBUG

public:
    bool isActive() const  { QMutexLocker lk(&mutex_); return mode_ == Mode::Activity; }
    bool isPaused() const  { QMutexLocker lk(&mutex_); return mode_ == Mode::Pause; }
    bool isStopped() const { QMutexLocker lk(&mutex_); return mode_ == Mode::None; }

    explicit Timer(const Settings & settings, SessionStore& db, QObject *parent = nullptr);
    ~Timer();

    qint64 getActiveTime() const;
    qint64 getPauseTime() const;
    Timeline snapshot() const;
    Timeline getCurrentDurations() const;
    void replaceCurrentDurations(const std::deque<TimeDuration>& newDurations,
                                 const std::optional<TimeDuration>& ongoing = std::nullopt);
    // Atomically replaces the live session with `edited`.
    // Precondition: caller holds no Timer locks.
    void commit(const Timeline& edited);
    std::deque<TimeDuration> getDurationsHistory();
    std::pair<int, int> getLastHistoryLoadStats() const;
    std::optional<TimeDuration> getOngoingDuration() const;
    // Cheap thread-safe predicate used by the MainWin watchdog. Returns true
    // iff there is an ongoing segment whose start date is earlier than today.
    bool isOngoingSegmentCrossMidnight() const;
    qint64 getStartupRecoveredSeconds() const;
    bool shouldShowStartupRecoveryNotification() const;
    void setDurationType(size_t idx, DurationType type);
    bool appendDurationsToDB();
    bool updateDurationsInDB();
    bool replaceAll(const Timeline& history, const Timeline& session);
    void beginExclusiveEdit();
    void endExclusiveEdit();
    bool canMarkCleanShutdown() const;

public slots:
    void useTimerViaButton(Button button);
    void useTimerViaLockEvent(LockEvent event);
    void onTick(const QDateTime& now);  // 100 ms heartbeat for the watchdog

signals:
    void userWarning(const QString& text);
    void stopped(Timer::StopReason reason);
    /// Emitted after a successful None→Activity or Pause→Activity transition,
    /// including lock-driven resume. fromPause is true when the previous mode
    /// was Pause, false for a fresh start from None.
    void started(bool fromPause);
    /// Emitted after a successful Activity→Pause transition (user-driven only;
    /// lock-driven autopause is signalled by modeChanged(LockAutopause)).
    void paused();
    /// Emitted for lock-driven autopause (LockAutopause) and autoresume
    /// (LockResume). On LockResume, started(true) also fires — GUI must not
    /// double-act on both.
    void modeChanged(Timer::PauseCause cause);

private slots:
    void saveCheckpoint();  // Periodic checkpoint saving every 5 minutes
};

Q_DECLARE_METATYPE(Timer::StopReason)
Q_DECLARE_METATYPE(Timer::PauseCause)

#endif // TIMER_H
