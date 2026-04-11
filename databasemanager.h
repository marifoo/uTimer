#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QObject>
#include <QSqlDatabase>
#include <QDateTime>
#include <deque>
#include <vector>
#include <optional>
#include "types.h"
#include "settings.h"

class DatabaseManager : public QObject
{
    Q_OBJECT
public:
    struct OrphanCheckpoint {
        long long id = -1;
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

    explicit DatabaseManager(const Settings& settings, QObject *parent = nullptr);
    ~DatabaseManager();

    bool saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode);
    LoadResult loadDurations();
    bool hasEntriesForDate(const QDate& date);
    bool saveCheckpoint(DurationType type, qint64 duration, const QDateTime& startTime, const QDateTime& endTime, long long& checkpointId);
    bool updateDurationsByStartTime(const std::deque<TimeDuration>& durations);
    bool checkSchemaOnStartup(); // Returns true if schema is valid, false if outdated
    void flushToDisc(); // Force pending writes to disk (for shutdown safety)
    std::deque<OrphanCheckpoint> loadUnfinalizedCheckpoints();
    bool reconcileUnfinalizedCheckpoints(const std::vector<long long>& finalizeIds, const std::vector<long long>& dropIds);
    bool setLastCleanShutdownMarker(const QDateTime& timestamp);
    std::optional<QDateTime> consumeLastCleanShutdownMarker();

private:
    QSqlDatabase db;
    uint history_days_to_keep_;
    const Settings& settings_;
    bool lazyOpen();
    void lazyClose();
    bool validateSchema();
    bool ensureIsFinalizedColumn();
    bool ensureSettingsTable();
    bool createBackup(const std::deque<TimeDuration>& durations, TransactionMode mode);
};

#endif // DATABASEMANAGER_H
