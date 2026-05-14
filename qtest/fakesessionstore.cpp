/**
 * FakeSessionStore -- implementation of the in-memory test double.
 *
 * Every method records its name in callLog and operates on in-memory data
 * structures.  No SQLite is involved.  Return values come from the
 * configurable *Result members set by the test.
 */

#include "fakesessionstore.h"

FakeSessionStore::FakeSessionStore()
{
}

bool FakeSessionStore::commitSession(const Timeline& session)
{
    callLog.append("commitSession");
    if (commitSessionResult) {
        // Mirror SqliteSessionStore: normalize internally, compute orphans, then upsert.
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

bool FakeSessionStore::replaceAll(const Timeline& history, const Timeline& session)
{
    callLog.append("replaceAll");
    if (replaceDurationsResult) {
        storedDurations.clear();
        for (const auto& d : history.completed()) {
            storedDurations.push_back(d);
        }
        for (const auto& d : session.completed()) {
            storedDurations.push_back(d);
        }
    }
    return replaceDurationsResult;
}

LoadResult FakeSessionStore::loadDurations()
{
    callLog.append("loadDurations");
    return loadDurationsResult;
}

EntriesForDateResult FakeSessionStore::hasEntriesForDate(const QDate& /*date*/)
{
    callLog.append("hasEntriesForDate");
    return entriesForDateResult;
}

bool FakeSessionStore::saveCheckpoint(DurationType type, qint64 duration, const QDateTime& startTime,
                                          const QDateTime& endTime, const QString& segmentId)
{
    callLog.append("saveCheckpoint");
    if (saveCheckpointResult) {
        savedCheckpoints.push_back({type, duration, startTime, endTime, segmentId});
    }
    return saveCheckpointResult;
}

bool FakeSessionStore::checkSchemaOnStartup()
{
    callLog.append("checkSchemaOnStartup");
    return checkSchemaResult;
}

void FakeSessionStore::flushToDisc()
{
    callLog.append("flushToDisc");
}

std::deque<OrphanCheckpoint> FakeSessionStore::loadUnfinalizedCheckpoints()
{
    callLog.append("loadUnfinalizedCheckpoints");
    return orphanCheckpoints;
}

bool FakeSessionStore::reconcileUnfinalizedCheckpoints(const std::vector<long long>& /*finalizeIds*/,
                                                           const std::vector<long long>& /*dropIds*/)
{
    callLog.append("reconcileUnfinalizedCheckpoints");
    return reconcileResult;
}

bool FakeSessionStore::setLastCleanShutdownMarker(const QDateTime& /*timestamp*/)
{
    callLog.append("setLastCleanShutdownMarker");
    return setMarkerResult;
}

std::optional<QDateTime> FakeSessionStore::consumeLastCleanShutdownMarker()
{
    callLog.append("consumeLastCleanShutdownMarker");
    auto result = cleanShutdownMarker;
    cleanShutdownMarker = std::nullopt;
    return result;
}
