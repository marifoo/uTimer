#ifndef TIMETRACKER_H
#define TIMETRACKER_H

#include <QObject>
#include <QtGlobal>
#include <QElapsedTimer>
#include <QDateTime>
#include <QMutex>
#include <QTimer>
#include <deque>
#include <memory>
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
    Mode mode_;
    bool was_active_before_autopause_;
    bool has_unsaved_data_;  // Track if previous DB save failed
    bool is_locked_;  // Track if desktop is currently locked (to prevent checkpoints while locked)
    DatabaseManager db_;
    mutable QRecursiveMutex mutex_;  // Protects state transitions from concurrent access
    QTimer checkpointTimer_;  // Timer for periodic checkpoint saving
    long long current_checkpoint_id_; // ID of the current database entry being updated by checkpoints

    void startTimer();
    void stopTimer();
    void pauseTimer();
    void backpauseTimer();
    void addDurationWithMidnightSplit(DurationType type, qint64 duration, const QDateTime& endTime);
    void saveCheckpointInternal();  // Internal checkpoint save (called when mutex already held)

public:
    explicit TimeTracker(const Settings & settings, QObject *parent = nullptr);
    ~TimeTracker();

    qint64 getActiveTime() const;
    qint64 getPauseTime() const;
    const std::deque<TimeDuration>& getCurrentDurations() const;
    void setCurrentDurations(const std::deque<TimeDuration>& newDurations);
    std::deque<TimeDuration> getDurationsHistory();
    void setDurationType(size_t idx, DurationType type);
    bool appendDurationsToDB();
    bool updateDurationsInDB();
    bool replaceDurationsInDB(std::deque<TimeDuration> durations);
    bool hasEntriesForToday();

public slots:
    void useTimerViaButton(Button button);
    void useTimerViaLockEvent(LockEvent event);

private slots:
    void saveCheckpoint();  // Periodic checkpoint saving every 5 minutes
};

#endif // TIMETRACKER_H
