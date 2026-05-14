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
#include "sessionstore.h"
#include "timeline.h"

/**
 * SessionState — groups the mutable per-session data that TimeTracker manages.
 *
 * Previously these fields were scattered as raw members on TimeTracker,
 * making it easy for code paths to mutate them as side effects without the
 * caller being aware. Grouping them into a struct with explicit transition
 * methods ensures:
 *   - Every mutation is named and logged (in debug builds)
 *   - State changes are traceable during debugging
 *   - Callers like addDuration can compute results without touching the struct
 *     directly, and the caller applies changes explicitly
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

    /// Updates segment_start_time (e.g. after a pause transition).
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


class TimeTracker : public QObject
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
        explicit DayBoundaryWatcher(TimeTracker& owner);

        /// Called on every 100 ms heartbeat. Runs the cross-midnight watchdog.
        void tick(const QDateTime& now);

        /// Arms the single-shot timer to fire at 23:59:59.500 today.
        void armScheduledStop(const QDateTime& now);

        /// Cancels the single-shot timer (call on any stop).
        void cancel();

    private:
        TimeTracker& owner_;
        QTimer midnight_timer_;

        void onMidnightTimerFired();
    };

    const Settings & settings_;
    QElapsedTimer timer_;
    SessionState session_;
    Mode mode_;
    bool was_active_before_autopause_;
    bool is_locked_;  // Track if desktop is currently locked (to prevent checkpoints while locked)
    bool checkpoints_paused_;  // Track if checkpoints are paused (e.g., while HistoryDialog is open)
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
    void finalizeActivityToPause(const QDateTime& pauseSegmentStart);
    // Appends a single same-day segment to session_.durations.
    // Silently discards segments where startTime.date() != endTime.date()
    // (last-line defence for cross-midnight). DayBoundaryWatcher's scheduled
    // stop and watchdog are supposed to make cross-midnight inputs unreachable.
    void addDuration(DurationType type,
                     const QDateTime& startTime,
                     const QDateTime& endTime,
                     const QString& segmentId = QString());

    // If the ongoing segment is cross-midnight, forces the engine into None,
    // discarding the in-flight segment. Returns true when it fired.
    // Safe to call multiple times; only the first call does real work.
    bool discardCrossMidnightOngoingAndStop(const QDateTime& now);
    EntriesForDateResult hasEntriesForDate(const QDate& date);
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
    explicit TimeTracker(const Settings & settings, SessionStore& db, QObject *parent = nullptr);
    ~TimeTracker();

    qint64 getActiveTime() const;
    qint64 getPauseTime() const;
    Timeline snapshot() const;
    const std::deque<TimeDuration>& getCurrentDurations() const;
    void replaceCurrentDurations(const std::deque<TimeDuration>& newDurations,
                                 const std::optional<TimeDuration>& ongoing = std::nullopt);
    void applyEdits(const Timeline& edited);
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
    void pauseCheckpoints();
    void resumeCheckpoints();
    bool canMarkCleanShutdown() const;

public slots:
    void useTimerViaButton(Button button);
    void useTimerViaLockEvent(LockEvent event);
    void onTick(const QDateTime& now);  // 100 ms heartbeat for the watchdog

signals:
    void userWarning(const QString& text);
    void stopped(TimeTracker::StopReason reason);
    /// Emitted when a lock-driven autopause or autoresume occurs so the GUI
    /// can follow without maintaining its own was_active_before_autopause_ flag.
    void modeChanged(TimeTracker::PauseCause cause);

private slots:
    void saveCheckpoint();  // Periodic checkpoint saving every 5 minutes
};

Q_DECLARE_METATYPE(TimeTracker::StopReason)
Q_DECLARE_METATYPE(TimeTracker::PauseCause)

#endif // TIMETRACKER_H
