/**
 * FakeSessionStore -- implementation of the in-memory test double.
 *
 * Every method records its name in callLog and operates on in-memory data
 * structures.  No SQLite is involved.  Return values come from the
 * configurable *Result members set by the test.
 */

#include "fakesessionstore.h"

FakeSessionStore::FakeSessionStore()
    : startupRecoveryResult{}
{
}

SessionStoreResult FakeSessionStore::commitSession(const Timeline& session)
{
    callLog.append("commitSession");
    if (commitSessionResult.ok()) {
        // Mirror SqliteSessionStore: normalize internally, compute orphans, then upsert.
        std::vector<SegmentId> beforeIds;
        for (const auto& d : session.completed())
            beforeIds.push_back(d.segment_id);

        Timeline normed = session.normalized();

        // Delete orphaned segment IDs (those that disappeared during normalization)
        for (const auto& id : beforeIds) {
            bool stillPresent = false;
            for (const auto& d : normed.completed())
                if (d.segment_id == id) { stillPresent = true; break; }
            if (!stillPresent) {
                committedSegmentIds.remove(id.toString());
                for (auto it = storedDurations.begin(); it != storedDurations.end(); ) {
                    if (it->segment_id == id)
                        it = storedDurations.erase(it);
                    else
                        ++it;
                }
            }
        }

        // Upsert normalized segments; enforce UNIQUE(segment_id) for new inserts.
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
                Q_ASSERT_X(!committedSegmentIds.contains(d.segment_id.toString()),
                           "FakeSessionStore::commitSession",
                           "UNIQUE(segment_id) violation: duplicate segment_id submitted");
                committedSegmentIds.insert(d.segment_id.toString());
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
        committedSegmentIds.clear();
        for (const auto& d : history.completed()) {
            storedDurations.push_back(d);
            committedSegmentIds.insert(d.segment_id.toString());
        }
        for (const auto& d : session.completed()) {
            storedDurations.push_back(d);
            committedSegmentIds.insert(d.segment_id.toString());
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

SessionStoreResult FakeSessionStore::saveCheckpoint(DurationType type, const QDateTime& startTime,
                                                     const QDateTime& endTime, const SegmentId& segmentId)
{
    callLog.append("saveCheckpoint");
    if (segmentId.isEmpty()) {
        return SessionStoreResult::callerBug("empty segment_id");
    }
    if (saveCheckpointResult.ok()) {
        // Check whether this segment_id already has a checkpoint record.
        // Multiple saves for the same segment_id are expected (it is an upsert/update),
        // so we allow them — we only assert that no *different* finalized row already owns this id.
        bool alreadySaved = false;
        for (const auto& cp : savedCheckpoints) {
            if (cp.segmentId == segmentId) {
                alreadySaved = true;
                break;
            }
        }
        // A segment_id that was finalized via commitSession must not also appear as a
        // checkpoint (that would mean Timer submitted the same id to two separate paths).
        Q_ASSERT_X(!committedSegmentIds.contains(segmentId.toString()),
                   "FakeSessionStore::saveCheckpoint",
                   "UNIQUE(segment_id) violation: segment_id already committed via commitSession");
        (void)alreadySaved; // Multiple checkpoint saves for the same id are fine (upsert).
        savedCheckpoints.push_back({type, startTime, endTime, segmentId});
    }
    return saveCheckpointResult;
}

SchemaStatus FakeSessionStore::checkSchemaOnStartup()
{
    callLog.append("checkSchemaOnStartup");
    return checkSchemaResult;
}

void FakeSessionStore::flushToDisc()
{
    callLog.append("flushToDisc");
}

bool FakeSessionStore::setLastCleanShutdownMarker(const QDateTime& /*timestamp*/)
{
    callLog.append("setLastCleanShutdownMarker");
    return setMarkerResult;
}

StartupRecoveryResult FakeSessionStore::recoverStartupCheckpoints(const QDateTime& /*now*/)
{
    callLog.append("recoverStartupCheckpoints");
    return startupRecoveryResult;
}
