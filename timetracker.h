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
#include "databasemanager.h"

class TimeTracker : public QObject
{
    Q_OBJECT
private:
    enum class Mode {Activity, Pause, None};

    const Settings & settings_;
    QElapsedTimer timer_;
    std::deque<TimeDuration> durations_;
    std::deque<TimeDuration> unsaved_durations_;  // Cached subset that still needs append-retry
    Mode mode_;
    bool was_active_before_autopause_;
    bool has_unsaved_data_;  // Track if previous DB save failed
    bool is_locked_;  // Track if desktop is currently locked (to prevent checkpoints while locked)
    bool checkpoints_paused_;  // Track if checkpoints are paused (e.g., while HistoryDialog is open)
    DatabaseManager db_;
    mutable QRecursiveMutex mutex_;  // Protects state transitions from concurrent access
    QTimer checkpointTimer_;  // Timer for periodic checkpoint saving
    QString current_checkpoint_segment_id_; // Segment identity currently used by checkpoints
    qint64 checkpoint_interval_msec_; // Checkpoint interval (0 = disabled)
    QDateTime segment_start_time_; // Wall-clock time when current segment started (set in startTimer())
    int last_history_load_skipped_;
    int last_history_load_repaired_;
    qint64 startup_recovered_seconds_;
    bool startup_recovery_notification_needed_;

    void startTimer();
    void stopTimer();
    void pauseTimer();
    void backpauseTimer();
    void addDurationWithMidnightSplit(DurationType type, const QDateTime& startTime, const QDateTime& endTime, const QString& segmentId = QString());
    void saveCheckpointInternal();  // Internal checkpoint save (called when mutex already held)
    bool appendDurationsChunkToDB(const std::deque<TimeDuration>& durations);
    qint64 reconcileOrphanCheckpoints(
        const std::deque<DatabaseManager::OrphanCheckpoint>& orphans,
        const std::optional<QDateTime>& cleanShutdownMarker);

public:
    explicit TimeTracker(const Settings & settings, QObject *parent = nullptr);
    ~TimeTracker();

    qint64 getActiveTime() const;
    qint64 getPauseTime() const;
    const std::deque<TimeDuration>& getCurrentDurations() const;
    void setCurrentDurations(const std::deque<TimeDuration>& newDurations);
    void resetCheckpointTrackingForOngoing(const TimeDuration& ongoing);
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
    bool hasEntriesForToday();
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
