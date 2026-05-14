/**
 * FakeDatabaseManager -- implementation of the in-memory test double.
 *
 * Every method records its name in callLog and operates on in-memory data
 * structures.  No SQLite is involved.  Return values come from the
 * configurable *Result members set by the test.
 */

#include "fakedatabasemanager.h"

FakeDatabaseManager::FakeDatabaseManager()
{
}

bool FakeDatabaseManager::commitSession(const Timeline& session)
{
    callLog.append("commitSession");
    if (commitSessionResult) {
        // Mirror DatabaseManager: normalize internally, compute orphans, then upsert.
        std::vector<QString> beforeIds;
        for (const auto& d : session.completed())
            beforeIds.push_back(d.segment_id);

        Timeline normed = session.normalized();

        // Delete orphaned segment IDs (those that disappeared during normalization)
        for (const auto& id : beforeIds) {
            bool stillPresent = false;
            for (const auto& d : normed.completed())
                if (d.segment_id == id) { stillPresent = true; break; }
            if (!stillPresent) {
                for (auto it = storedDurations.begin(); it != storedDurations.end(); ) {
                    if (it->segment_id == id)
                        it = storedDurations.erase(it);
                    else
                        ++it;
                }
            }
        }

        // Upsert normalized segments
        for (const auto& d : normed.completed()) {
            bool found = false;
            for (auto& existing : storedDurations) {
                if (existing.segment_id == d.segment_id) {
                    existing = d;
                    found = true;
                    break;
                }
            }
            if (!found) {
                storedDurations.push_back(d);
            }
        }
    }
    return commitSessionResult;
}

bool FakeDatabaseManager::saveDurations(const std::deque<TimeDuration>& durations, TransactionMode /*mode*/,
                                         const std::vector<QString>& /*removedSegmentIds*/)
{
    callLog.append("saveDurations");
    if (saveDurationsResult) {
        for (const auto& d : durations) {
            storedDurations.push_back(d);
        }
    }
    return saveDurationsResult;
}

bool FakeDatabaseManager::replaceDurationsInDB(const std::deque<TimeDuration>& historyDurations,
                                                const std::deque<TimeDuration>& currentSessionDurations)
{
    callLog.append("replaceDurationsInDB");
    if (replaceDurationsResult) {
        storedDurations.clear();
        for (const auto& d : historyDurations) {
            storedDurations.push_back(d);
        }
        for (const auto& d : currentSessionDurations) {
            storedDurations.push_back(d);
        }
    }
    return replaceDurationsResult;
}

LoadResult FakeDatabaseManager::loadDurations()
{
    callLog.append("loadDurations");
    return loadDurationsResult;
}

EntriesForDateResult FakeDatabaseManager::hasEntriesForDate(const QDate& /*date*/)
{
    callLog.append("hasEntriesForDate");
    return entriesForDateResult;
}

bool FakeDatabaseManager::saveCheckpoint(DurationType type, qint64 duration, const QDateTime& startTime,
                                          const QDateTime& endTime, const QString& segmentId)
{
    callLog.append("saveCheckpoint");
    if (saveCheckpointResult) {
        savedCheckpoints.push_back({type, duration, startTime, endTime, segmentId});
    }
    return saveCheckpointResult;
}

bool FakeDatabaseManager::updateDurationsById(const std::deque<TimeDuration>& durations,
                                               const std::vector<QString>& /*removedSegmentIds*/)
{
    callLog.append("updateDurationsById");
    if (updateDurationsByIdResult) {
        // Upsert semantics: update matching segment_id or append.
        for (const auto& d : durations) {
            bool found = false;
            for (auto& existing : storedDurations) {
                if (existing.segment_id == d.segment_id) {
                    existing = d;
                    found = true;
                    break;
                }
            }
            if (!found) {
                storedDurations.push_back(d);
            }
        }
    }
    return updateDurationsByIdResult;
}

bool FakeDatabaseManager::checkSchemaOnStartup()
{
    callLog.append("checkSchemaOnStartup");
    return checkSchemaResult;
}

void FakeDatabaseManager::flushToDisc()
{
    callLog.append("flushToDisc");
}

std::deque<OrphanCheckpoint> FakeDatabaseManager::loadUnfinalizedCheckpoints()
{
    callLog.append("loadUnfinalizedCheckpoints");
    return orphanCheckpoints;
}

bool FakeDatabaseManager::reconcileUnfinalizedCheckpoints(const std::vector<long long>& /*finalizeIds*/,
                                                           const std::vector<long long>& /*dropIds*/)
{
    callLog.append("reconcileUnfinalizedCheckpoints");
    return reconcileResult;
}

bool FakeDatabaseManager::setLastCleanShutdownMarker(const QDateTime& /*timestamp*/)
{
    callLog.append("setLastCleanShutdownMarker");
    return setMarkerResult;
}

std::optional<QDateTime> FakeDatabaseManager::consumeLastCleanShutdownMarker()
{
    callLog.append("consumeLastCleanShutdownMarker");
    auto result = cleanShutdownMarker;
    cleanShutdownMarker = std::nullopt;
    return result;
}
