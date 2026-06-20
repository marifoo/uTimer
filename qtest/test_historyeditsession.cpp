#include "test_historyeditsession.h"
#include <QtTest>

// Expose private members of HistoryEditSession for direct field access in tests.
#define private public
#define protected public
#include "../historyeditsession.h"
#include "../timer.h"
#undef private
#undef protected

#include "fakesessionstore.h"
#include "testcommon.h"

using TestCommon::createSettingsFile;
using TestCommon::mk;

namespace {

QDateTime makeUTC(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC);
}

QDateTime todayUTC(int hour, int minute = 0, int second = 0) {
    return QDateTime(QDate::currentDate(), QTime(hour, minute, second), Qt::UTC);
}

QDateTime yesterdayUTC(int hour, int minute = 0, int second = 0) {
    return QDateTime(QDate::currentDate().addDays(-1), QTime(hour, minute, second), Qt::UTC);
}

} // namespace

void HistoryEditSessionTest::initTestCase()
{
    // No DB setup required — tests use FakeSessionStore.
}

void HistoryEditSessionTest::cleanupTestCase()
{
}

void HistoryEditSessionTest::resetDatabaseFile() const
{
    // No-op: this suite uses FakeSessionStore, not SQLite.
}

// ---------------------------------------------------------------------------
// Page creation
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_build_today_page_only_from_memory()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 30);
    tracker.session_.durations.push_back(TimeDuration(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    QCOMPARE(session.pages_.size(), static_cast<size_t>(1));
    QCOMPARE(session.pages_[0].isCurrent, true);
    QCOMPARE(session.pages_[0].pageDate, QDate::currentDate());
    // pendingTimelines[0] has 1 completed row (the memory row)
    QCOMPARE(session.pendingTimelines_.size(), static_cast<size_t>(1));
    QCOMPARE(session.pendingTimelines_[0].completed().size(), static_cast<size_t>(1));
    QVERIFY(!session.pendingTimelines_[0].ongoing().has_value());
}

void HistoryEditSessionTest::test_build_today_page_memory_plus_db()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Seed a today DB row in the fake store.
    const QDateTime dbStart = todayUTC(8, 0);
    const QDateTime dbEnd   = todayUTC(8, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration(DurationType::Pause, dbStart, dbEnd));

    Timer tracker(settings, fakeDb);

    // Memory row (different time, different segment_id)
    const QDateTime memStart = todayUTC(9, 0);
    const QDateTime memEnd   = todayUTC(9, 30);
    tracker.session_.durations.push_back(TimeDuration(DurationType::Activity, memStart, memEnd));

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    // Only 1 page: today.
    QCOMPARE(session.pages_.size(), static_cast<size_t>(1));
    QCOMPARE(session.pages_[0].isCurrent, true);
    // 2 completed rows: memory row + db today row.
    QCOMPARE(session.pendingTimelines_[0].completed().size(), static_cast<size_t>(2));
}

void HistoryEditSessionTest::test_build_historical_page_separate_from_today()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // A yesterday DB row.
    const QDateTime histStart = yesterdayUTC(10, 0);
    const QDateTime histEnd   = yesterdayUTC(10, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration(DurationType::Activity, histStart, histEnd));

    Timer tracker(settings, fakeDb);

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    // 2 pages: today (empty, isCurrent=true) + yesterday (1 entry, isCurrent=false).
    QCOMPARE(session.pages_.size(), static_cast<size_t>(2));
    QCOMPARE(session.pages_[0].isCurrent, true);
    QCOMPARE(session.pages_[1].isCurrent, false);
    QCOMPARE(session.pages_[1].pageDate, QDate::currentDate().addDays(-1));
    QCOMPARE(session.pendingTimelines_[1].completed().size(), static_cast<size_t>(1));
}

void HistoryEditSessionTest::test_build_deduplicates_db_row_matching_memory_segment_id()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const SegmentId sharedId = SegmentId::mint();
    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 30);

    // Memory row with sharedId
    Timer tracker(settings, fakeDb);
    tracker.session_.durations.push_back(TimeDuration(DurationType::Activity, start, end, sharedId));

    // DB row with the same segment_id — should be deduped out.
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration(DurationType::Activity, start.addMSecs(-2), end, sharedId));

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    // Only the memory row survives (DB duplicate is excluded).
    QCOMPARE(session.pendingTimelines_[0].completed().size(), static_cast<size_t>(1));
    QCOMPARE(session.pendingTimelines_[0].completed()[0].segment_id, sharedId);
}

// ---------------------------------------------------------------------------
// Origin routing
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_origin_memory_rows_marked_true()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 30);
    tracker.session_.durations.push_back(TimeDuration(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    const QString segId = session.pendingTimelines_[0].completed()[0].segment_id.toString();
    QVERIFY(session.originIsMemory_.value(segId, false));
}

void HistoryEditSessionTest::test_origin_db_rows_marked_false()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime dbStart = yesterdayUTC(10, 0);
    const QDateTime dbEnd   = yesterdayUTC(10, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration(DurationType::Activity, dbStart, dbEnd));

    Timer tracker(settings, fakeDb);

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    const QString segId = session.pendingTimelines_[1].completed()[0].segment_id.toString();
    QCOMPARE(session.originIsMemory_.value(segId, true), false);
}

// ---------------------------------------------------------------------------
// Split op
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_split_registers_child_with_same_origin()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 0, 8); // 8-second segment
    tracker.session_.durations.push_back(TimeDuration(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    const QString origId = session.pendingTimelines_[0].completed()[0].segment_id.toString();
    QCOMPARE(session.originIsMemory_.value(origId, false), true);

    // Split at 4 s
    const QDateTime splitAt = todayUTC(10, 0, 4);
    session.pendingTimelines_[0] = session.pendingTimelines_[0].withSplit(
        0, splitAt, DurationType::Activity, DurationType::Pause);

    // The second half gets a fresh segment_id; register it with the same origin.
    const auto& newComp = session.pendingTimelines_[0].completed();
    QCOMPARE(newComp.size(), static_cast<size_t>(2));
    const QString childId = newComp[1].segment_id.toString();
    session.registerSplitChild(childId, true /*wasMemory*/);

    QCOMPARE(session.originIsMemory_.value(childId, false), true);
}

void HistoryEditSessionTest::test_split_history_row_child_is_false_origin()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    FakeSessionStore fakeDb;

    const QDateTime histStart = yesterdayUTC(10, 0);
    const QDateTime histEnd   = yesterdayUTC(10, 0, 8);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration(DurationType::Activity, histStart, histEnd));

    Timer tracker(settings, fakeDb);

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    QCOMPARE(session.pages_.size(), static_cast<size_t>(2));

    const QString origId = session.pendingTimelines_[1].completed()[0].segment_id.toString();
    QCOMPARE(session.originIsMemory_.value(origId, true), false);

    const QDateTime splitAt = yesterdayUTC(10, 0, 4);
    session.pendingTimelines_[1] = session.pendingTimelines_[1].withSplit(
        0, splitAt, DurationType::Activity, DurationType::Pause);

    const auto& newComp = session.pendingTimelines_[1].completed();
    QCOMPARE(newComp.size(), static_cast<size_t>(2));
    const QString childId = newComp[1].segment_id.toString();
    session.registerSplitChild(childId, false /*wasMemory*/);

    QCOMPARE(session.originIsMemory_.value(childId, true), false);
}

// ---------------------------------------------------------------------------
// Type toggle
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_type_toggle_updates_pending_timeline()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 30);
    tracker.session_.durations.push_back(TimeDuration(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    QCOMPARE(session.pendingTimelines_[0].completed()[0].type, DurationType::Activity);

    session.setPageTimeline(0, session.pendingTimelines_[0].withSegmentType(0, DurationType::Pause));

    QCOMPARE(session.pendingTimelines_[0].completed()[0].type, DurationType::Pause);
}

// ---------------------------------------------------------------------------
// Merge detection
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_no_merge_needed_for_non_overlapping()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime dbStart = todayUTC(8, 0);
    const QDateTime dbEnd   = todayUTC(8, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration(DurationType::Pause, dbStart, dbEnd));

    Timer tracker(settings, fakeDb);

    const QDateTime memStart = todayUTC(9, 0);
    const QDateTime memEnd   = todayUTC(9, 30);
    tracker.session_.durations.push_back(TimeDuration(DurationType::Activity, memStart, memEnd));

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    const auto payload = session.buildSavePayload();
    QVERIFY(!payload.needsMergeConfirmation);
}

void HistoryEditSessionTest::test_merge_needed_for_overlapping_rows()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // DB row 10:00–10:30, memory row 10:15–10:45 → overlap
    const QDateTime dbStart = todayUTC(10, 0);
    const QDateTime dbEnd   = todayUTC(10, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration(DurationType::Activity, dbStart, dbEnd));

    Timer tracker(settings, fakeDb);

    const QDateTime memStart = todayUTC(10, 15);
    const QDateTime memEnd   = todayUTC(10, 45);
    tracker.session_.durations.push_back(TimeDuration(DurationType::Activity, memStart, memEnd));

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    const auto payload = session.buildSavePayload();
    QVERIFY(payload.needsMergeConfirmation);
}

// ---------------------------------------------------------------------------
// Cross-midnight preservation
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_cross_midnight_preserved_in_payload()
{
    // A cross-midnight row should appear in crossMidnightRows_ and then be
    // preserved in historyTimeline when buildSavePayload() is called.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Cross-midnight: 23:50 yesterday → 00:10 today (same-day invariant bypass)
    const QDate yesterday = QDate::currentDate().addDays(-1);
    const QDateTime cmStart(yesterday, QTime(23, 50, 0), Qt::LocalTime);
    const QDateTime cmEnd(QDate::currentDate(), QTime(0, 10, 0), Qt::LocalTime);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromTrusted(DurationType::Activity, cmStart, cmEnd));

    Timer tracker(settings, fakeDb);

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    // crossMidnightRows_ must hold exactly the one cross-midnight row.
    QCOMPARE(session.crossMidnightRows_.size(), static_cast<size_t>(1));

    // After buildSavePayload(), the cross-midnight row is in historyTimeline.
    const auto payload = session.buildSavePayload();
    const auto& hist = payload.historyTimeline.completed();
    QCOMPARE(hist.size(), static_cast<size_t>(1));
    QCOMPARE(hist[0].startTime.date(), yesterday);
}

// ---------------------------------------------------------------------------
// Ongoing refresh / clear behaviour
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_refresh_ongoing_updates_end_time()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    // Start the timer so there is an ongoing segment.
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(50);

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    QVERIFY(session.pendingTimelines_[0].ongoing().has_value());
    const QDateTime snapshotEnd = session.pendingTimelines_[0].ongoing()->endTime;

    // Wait so the engine's ongoing end-time advances.
    QTest::qWait(100);

    // refreshOngoing() should update the end-time to the newer value.
    session.refreshOngoing(tracker);

    const QDateTime refreshedEnd = session.pendingTimelines_[0].ongoing()->endTime;
    QVERIFY2(refreshedEnd > snapshotEnd,
             "refreshed end-time must be strictly later than snapshot end-time");

    tracker.useTimerViaButton(Button::Stop);
}

void HistoryEditSessionTest::test_refresh_ongoing_skipped_when_user_modified()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(30);

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    QVERIFY(session.pendingTimelines_[0].ongoing().has_value());
    const QDateTime snapshotEnd = session.pendingTimelines_[0].ongoing()->endTime;

    // Mark the ongoing row as user-modified.
    session.markOngoingModified();

    QTest::qWait(100);

    // refreshOngoing() must be a no-op when user has edited the ongoing row.
    session.refreshOngoing(tracker);

    QCOMPARE(session.pendingTimelines_[0].ongoing()->endTime, snapshotEnd);

    tracker.useTimerViaButton(Button::Stop);
}

void HistoryEditSessionTest::test_refresh_ongoing_clears_when_engine_has_no_ongoing()
{
    // If the engine no longer has an ongoing segment (e.g. engine stopped before
    // save), refreshOngoing() must clear the session's ongoing to avoid anchoring
    // a stale start time.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    QVERIFY(session.pendingTimelines_[0].ongoing().has_value());

    // Stop the engine (simulates a deferred midnight stop or similar).
    tracker.useTimerViaButton(Button::Stop);

    // refreshOngoing() must clear the ongoing in the session.
    session.refreshOngoing(tracker);

    QVERIFY(!session.pendingTimelines_[0].ongoing().has_value());
}

// ---------------------------------------------------------------------------
// Payload correctness
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_payload_history_vs_session_buckets()
{
    // DB row (yesterday): goes to historyTimeline (finalized).
    // Memory row (today): goes to sessionTimeline (unfinalized).
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime histStart = yesterdayUTC(10, 0);
    const QDateTime histEnd   = yesterdayUTC(10, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration(DurationType::Activity, histStart, histEnd));

    Timer tracker(settings, fakeDb);

    const QDateTime memStart = todayUTC(9, 0);
    const QDateTime memEnd   = todayUTC(9, 30);
    tracker.session_.durations.push_back(TimeDuration(DurationType::Activity, memStart, memEnd));

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    const auto payload = session.buildSavePayload();

    // 1 history row, 1 session row (no ongoing here).
    QCOMPARE(payload.historyTimeline.completed().size(), static_cast<size_t>(1));
    QCOMPARE(payload.sessionTimeline.completed().size(), static_cast<size_t>(1));
    QCOMPARE(payload.historyTimeline.completed()[0].startTime.toUTC().time(), QTime(10, 0, 0));
    QCOMPARE(payload.sessionTimeline.completed()[0].startTime.toUTC().time(), QTime(9, 0, 0));
    QVERIFY(!payload.needsMergeConfirmation);
}

void HistoryEditSessionTest::test_payload_memory_timeline_for_commit()
{
    // memoryTimeline must contain only the memory-origin rows (+ ongoing if present),
    // ready for Timer::commitEditedTimeline().
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // DB row today: should end up in historyTimeline, NOT in memoryTimeline.
    const QDateTime dbStart = todayUTC(8, 0);
    const QDateTime dbEnd   = todayUTC(8, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration(DurationType::Pause, dbStart, dbEnd));

    Timer tracker(settings, fakeDb);

    // Memory row today.
    const QDateTime memStart = todayUTC(9, 0);
    const QDateTime memEnd   = todayUTC(9, 30);
    tracker.session_.durations.push_back(TimeDuration(DurationType::Activity, memStart, memEnd));

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    const auto payload = session.buildSavePayload();

    // memoryTimeline has only the memory row.
    QCOMPARE(payload.memoryTimeline.completed().size(), static_cast<size_t>(1));
    QCOMPARE(payload.memoryTimeline.completed()[0].startTime.toUTC().time(), QTime(9, 0, 0));
    // historyTimeline has the DB row.
    QCOMPARE(payload.historyTimeline.completed().size(), static_cast<size_t>(1));
}

void HistoryEditSessionTest::test_payload_memory_absorbs_merged_memory_row()
{
    // When a DB row and a memory row overlap and are merged during normalization,
    // the merged row is placed into the session bucket (not history) because it
    // absorbed current-session time.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // DB Activity row 10:00–10:30 (today)
    const QDateTime dbStart = todayUTC(10, 0);
    const QDateTime dbEnd   = todayUTC(10, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration(DurationType::Activity, dbStart, dbEnd));

    Timer tracker(settings, fakeDb);

    // Memory Activity row 10:15–10:45 (overlaps DB row)
    const QDateTime memStart = todayUTC(10, 15);
    const QDateTime memEnd   = todayUTC(10, 45);
    tracker.session_.durations.push_back(TimeDuration(DurationType::Activity, memStart, memEnd));

    HistoryEditSession session;
    session.buildFromTimer(tracker);

    const auto payload = session.buildSavePayload();

    // After normalization: exactly 1 merged row covering 10:00–10:45.
    QVERIFY(payload.needsMergeConfirmation);
    // The merged row absorbed memory time, so it must be in the session bucket.
    QCOMPARE(payload.sessionTimeline.completed().size(), static_cast<size_t>(1));
    QCOMPARE(payload.sessionTimeline.completed()[0].startTime.toUTC().time(), QTime(10, 0, 0));
    QCOMPARE(payload.sessionTimeline.completed()[0].endTime.toUTC().time(), QTime(10, 45, 0));
    // History bucket should be empty.
    QCOMPARE(payload.historyTimeline.completed().size(), static_cast<size_t>(0));
    // Memory timeline also has the merged row.
    QCOMPARE(payload.memoryTimeline.completed().size(), static_cast<size_t>(1));
}
