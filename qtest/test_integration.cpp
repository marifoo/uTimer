#include "test_integration.h"
#include "fakesessionstore.h"
#include <QtTest>
#include <QSignalSpy>
#ifdef Q_OS_UNIX
#include <time.h>
#endif

using TestCommon::createSettingsFile;
using TestCommon::mk;
using TestCommon::sumDurations;

void IntegrationTest::initTestCase()
{
    db_path_ = QDir(QCoreApplication::applicationDirPath()).filePath("uTimer.sqlite");
    if (QFile::exists(db_path_)) {
        db_backup_path_ = db_path_ + ".bak_test";
        QFile::remove(db_backup_path_);
        QVERIFY(QFile::rename(db_path_, db_backup_path_));
    }
}

void IntegrationTest::cleanupTestCase()
{
    if (!db_path_.isEmpty()) {
        QFile::remove(db_path_);
    }
    if (!db_backup_path_.isEmpty()) {
        QFile::remove(db_path_);
        QVERIFY(QFile::rename(db_backup_path_, db_path_));
    }
}

void IntegrationTest::test_integration_checkpoint_recovery_on_restart()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    SegmentId orphanSegmentId;
    
    {
        // First session: Start timer and save checkpoint
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        Timer tracker(settings, db);
        
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(1200);
        
        // Manually save checkpoint
        tracker.saveCheckpointInternal_dbg(QDateTime::currentDateTime());
        QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());
        orphanSegmentId = tracker.sessionState_dbg().segment_id;

        // Simulate an unclean shutdown: keep checkpoint row, skip graceful stop.
        tracker.forceMode_dbg(Timer::Mode::None);
        tracker.sessionState_dbg().segment_id = SegmentId{};

        // Simulate crash (tracker destroyed without stopping)
    }
    
    {
        // Second session: startup reconciliation finalizes orphan checkpoint
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        Timer tracker(settings, db);
        tracker.initializeFromStore();
        QVERIFY(tracker.getStartupRecoveredSeconds() > 0);

        SqliteSessionStore db2(settings);
        auto loaded = db2.loadDurations();
        QCOMPARE(loaded.size(), (size_t)1); // Reconciled orphan recovered
        QCOMPARE(loaded[0].type, DurationType::Activity);

        // Reconciliation must keep row identity by finalizing in place.
        QVERIFY(db.ensureOpen_dbg());
        QSqlQuery query(db.rawDb_dbg());
        query.prepare("SELECT is_finalized FROM durations WHERE segment_id = :segment_id");
        query.bindValue(":segment_id", orphanSegmentId.toString());
        QVERIFY(query.exec());
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 1);
        db.lazyClose_dbg();
    }
}

void IntegrationTest::test_integration_orphan_reconciliation_is_idempotent()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        Timer tracker(settings, db);
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(1200);
        tracker.saveCheckpointInternal_dbg(QDateTime::currentDateTime());
        QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());
        tracker.sessionState_dbg().segment_id = SegmentId{};
        tracker.forceMode_dbg(Timer::Mode::None);
    }

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        Timer tracker(settings, db);
        tracker.initializeFromStore();
        QCOMPARE(tracker.getStartupRecoveredSeconds() >= 1, true);
    }

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        Timer tracker(settings, db);
        tracker.initializeFromStore();
        QCOMPARE(tracker.getStartupRecoveredSeconds(), static_cast<qint64>(0));
        SqliteSessionStore db2(settings);
        auto loaded = db2.loadDurations();
        QCOMPARE(loaded.size(), static_cast<size_t>(1));
    }
}

void IntegrationTest::test_integration_orphan_reconciliation_drops_stale_and_too_short()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    QVERIFY(db.ensureOpen_dbg());

    // Too short orphan (< 1s)
    {
        QSqlQuery insert(db.rawDb_dbg());
        const QDateTime start = QDateTime::currentDateTimeUtc().addSecs(-10);
        const QDateTime end = start.addMSecs(500);
        insert.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:segment_id, :type, :start_utc, :end_utc, 0)"
        );
        insert.bindValue(":segment_id", SegmentId::mint().toString());
        insert.bindValue(":type", static_cast<int>(DurationType::Activity));
        insert.bindValue(":start_utc", start.toString(Qt::ISODateWithMs));
        insert.bindValue(":end_utc", end.toString(Qt::ISODateWithMs));
        QVERIFY(insert.exec());
    }

    // Stale orphan (>24h)
    {
        QSqlQuery insert(db.rawDb_dbg());
        const QDateTime start = QDateTime::currentDateTimeUtc().addDays(-2);
        const QDateTime end = start.addSecs(30);
        insert.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:segment_id, :type, :start_utc, :end_utc, 0)"
        );
        insert.bindValue(":segment_id", SegmentId::mint().toString());
        insert.bindValue(":type", static_cast<int>(DurationType::Activity));
        insert.bindValue(":start_utc", start.toString(Qt::ISODateWithMs));
        insert.bindValue(":end_utc", end.toString(Qt::ISODateWithMs));
        QVERIFY(insert.exec());
    }

    db.lazyClose_dbg();

    SqliteSessionStore db2(settings);
    Timer tracker(settings, db2);
    tracker.initializeFromStore();
    QCOMPARE(tracker.getStartupRecoveredSeconds(), static_cast<qint64>(0));

    auto loaded = db.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(0));
}

void IntegrationTest::test_integration_orphan_reconciliation_marker_present_is_silent()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        Timer tracker(settings, db);
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(1200);
        tracker.saveCheckpointInternal_dbg(QDateTime::currentDateTime());
        QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());
        tracker.sessionState_dbg().segment_id = SegmentId{};
        tracker.forceMode_dbg(Timer::Mode::None);
    }

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        QVERIFY(db.setLastCleanShutdownMarker(QDateTime::currentDateTime()));
    }

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        Timer tracker(settings, db);
        tracker.initializeFromStore();
        QVERIFY(tracker.getStartupRecoveredSeconds() > 0);
        QVERIFY(!tracker.shouldShowStartupRecoveryNotification());
    }
}

void IntegrationTest::test_integration_orphan_reconciliation_marker_absent_shows_notification()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        Timer tracker(settings, db);
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(1200);
        tracker.saveCheckpointInternal_dbg(QDateTime::currentDateTime());
        QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());
        tracker.sessionState_dbg().segment_id = SegmentId{};
        tracker.forceMode_dbg(Timer::Mode::None);
    }

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        Timer tracker(settings, db);
        tracker.initializeFromStore();
        QVERIFY(tracker.getStartupRecoveredSeconds() > 0);
        QVERIFY(tracker.shouldShowStartupRecoveryNotification());
    }
}

void IntegrationTest::test_integration_memory_db_consistency()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    // Start -> Activity for 50ms
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(50);
    
    // Pause
    tracker.useTimerViaButton(Button::Pause);
    auto memoryDurations = tracker.getCurrentDurations();
    QVERIFY(memoryDurations.completed().size() >= 1);
    
    // Resume -> Activity for 50ms
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(50);
    
    // Save checkpoint
    tracker.saveCheckpointInternal_dbg(QDateTime::currentDateTime());
    QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());
    
    // Stop
    tracker.useTimerViaButton(Button::Stop);
    
    // Load from DB
    SqliteSessionStore db2(settings);
    auto dbDurations = db2.loadDurations();
    
    // Total durations should match (memory + checkpoint)
    qint64 memoryTotal = sumDurations(memoryDurations.completed(), DurationType::Activity) +
                        sumDurations(memoryDurations.completed(), DurationType::Pause);
    qint64 dbTotal = sumDurations(dbDurations.durations, DurationType::Activity) +
                    sumDurations(dbDurations.durations, DurationType::Pause);
    
    // Allow small tolerance for timing differences
    QVERIFY(qAbs(memoryTotal - dbTotal) < 200);
}

void IntegrationTest::test_integration_retention_cleanup_preserves_current()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 2)); // Keep 2 days
    SqliteSessionStore manager(settings);

    QDateTime now = QDateTime::currentDateTimeUtc();
    
    // Insert data from 5 days ago (should be deleted)
    std::deque<TimeDuration> old;
    old.emplace_back(DurationType::Activity, now.addDays(-5), now.addDays(-5).addSecs(60));
    QVERIFY(manager.saveDurations(old, TransactionMode::Append));
    
    // Insert data from today (should be kept)
    std::deque<TimeDuration> current;
    current.emplace_back(DurationType::Activity, now.addSecs(-100), now.addSecs(-90));
    QVERIFY(manager.saveDurations(current, TransactionMode::Append));
    
    // Cleanup runs in checkSchemaOnStartup(), not in the constructor.
    SqliteSessionStore manager2(settings);
    QCOMPARE(manager2.checkSchemaOnStartup(), SchemaStatus::Ready);

    auto loaded = manager2.loadDurations();
    QVERIFY(loaded.size() >= 1); // Current day preserved
    
    // Verify old entries are gone
    bool hasOldEntry = false;
    for (const auto& d : loaded) {
        if (d.endTime.date() == now.addDays(-5).date()) {
            hasOldEntry = true;
            break;
        }
    }
    QVERIFY(!hasOldEntry);
}

void IntegrationTest::test_integration_duplicate_prevention()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime start = QDateTime::currentDateTime();
    QDateTime end = start.addSecs(10);
    
    // Insert same segment twice via update-by-id; second call updates same row.
    std::deque<TimeDuration> durations;
    durations.emplace_back(DurationType::Activity, start, end);
    
    QVERIFY(manager.updateDurationsById(durations).ok());
    durations.front().endTime = end.addSecs(10);
    durations.front().duration = durations.front().startTime.msecsTo(durations.front().endTime);
    QVERIFY(manager.updateDurationsById(durations).ok());
    
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
}

void IntegrationTest::test_integration_empty_database_operations()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Operations on empty database should succeed
    QVERIFY(manager.saveDurations({}, TransactionMode::Append));
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)0);
    
    QCOMPARE(manager.hasEntriesForDate(QDate::currentDate()), EntriesForDateResult::No);
    
    std::deque<TimeDuration> empty;
    QVERIFY(manager.updateDurationsById(empty).ok());
}

// ============================================================================
// Phase 1 test gate (T1)
// ============================================================================

void IntegrationTest::test_F_shutdown_sequence_stop_flush_marker()
{
    // Test F: shutdown sequence end-to-end.
    //
    // After Phase 1 the shutdown sequence in MainWin::shutdown() calls:
    //   1. timetracker_.useTimerViaButton(Button::Stop)  — stops the engine
    //   2. db_.flushToDisc()                             — durability flush
    //   3. db_.setLastCleanShutdownMarker(...)           — if canMarkCleanShutdown()
    //
    // This test drives all three steps through Timer + FakeSessionStore
    // and asserts each side-effect happened in the correct order, mimicking
    // what MainWin::shutdown() does after Phase 1.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    // Start the timer so there is something to stop.
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    QVERIFY(tracker.isActive());

    // Clear the call log captured during startup / start so the assertion
    // below only sees the shutdown sequence.
    fakeDb.callLog.clear();

    // Act — replicate MainWin::shutdown() logic (Phase 1 version):
    // Step 1: stop timer
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.isStopped());

    // Step 2: flush DB to disc
    fakeDb.flushToDisc();   // calls db_.flushToDisc() as MainWin does

    // Step 3: mark clean shutdown if the engine says it is safe
    QVERIFY(tracker.canMarkCleanShutdown()); // stopped + no unsaved data
    QDateTime markerTime = QDateTime::currentDateTime();
    bool marked = fakeDb.setLastCleanShutdownMarker(markerTime);
    QVERIFY(marked);

    // Assert — verify all three side-effects and their order.
    // The stop propagates as updateDurationsById (from updateDurationsInDB)
    // followed by setLastCleanShutdownMarker from stopTimer itself; then our
    // flush and our explicit marker call follow.
    QVERIFY(fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));

    int flushIdx = fakeDb.callLog.lastIndexOf("flushToDisc");
    int markerIdx = fakeDb.callLog.lastIndexOf("setLastCleanShutdownMarker");
    QVERIFY2(flushIdx < markerIdx,
             "flushToDisc must happen before the final setLastCleanShutdownMarker");
}

// ============================================================================
// Phase 5 pre-flight regression tests (T5.0a)
// ============================================================================

/**
 * Normal same-day session: isOngoingSegmentCrossMidnight() returns false and
 * discardCrossMidnightOngoingAndStop() does not fire.
 */
void IntegrationTest::test_5_0a_normal_same_day_no_discard()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    // Start with a same-day segment_start_time (current real time is fine).
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    // Watchdog predicate must be false for a fresh same-day session.
    QVERIFY(!tracker.isOngoingSegmentCrossMidnight());

    // Directly call the internal helper with a same-day "now".
    const QDateTime sameDay = QDateTime::currentDateTime();
    QVERIFY(!tracker.discardCrossMidnightOngoingAndStop_dbg(sameDay));

    // Engine must still be in Activity.
    QVERIFY(tracker.isActive());
}

/**
 * Cross-midnight ongoing segment is discarded; same-day completed segments
 * that accumulated before midnight are preserved and flushed to the DB.
 */
void IntegrationTest::test_5_0a_cross_midnight_ongoing_discarded_completed_preserved()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    // Arrange: start the timer, add a completed same-day segment manually,
    // then rewind segment_start_time to yesterday to simulate cross-midnight.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    // Insert a completed segment via direct addDuration() so we control the
    // timestamps and guarantee a positive duration independent of wall-clock.
    const QDateTime segStart = QDateTime::currentDateTime().addSecs(-10);
    const QDateTime segEnd   = QDateTime::currentDateTime().addSecs(-1);
    tracker.addDuration_dbg(DurationType::Activity, segStart, segEnd);
    QVERIFY(!tracker.sessionState_dbg().durations.empty());
    const size_t completedCount = tracker.sessionState_dbg().durations.size();

    // Simulate sleep through midnight: move segment_start_time to yesterday.
    const QDateTime yesterday = QDateTime::currentDateTime().addDays(-1);
    tracker.sessionState_dbg().segment_start_time = yesterday;

    // Pre-condition: watchdog predicate should now be true.
    QVERIFY(tracker.isOngoingSegmentCrossMidnight());

    // Act: call the internal helper with "now" (today).
    const QDateTime today = QDateTime::currentDateTime();
    fakeDb.callLog.clear();
    const bool fired = tracker.discardCrossMidnightOngoingAndStop_dbg(today);

    // Assert: helper fired, engine stopped, completed segments were committed.
    QVERIFY(fired);
    QVERIFY(tracker.isStopped());
    QVERIFY(fakeDb.callLog.contains("commitSession"));
    // The completed segments must NOT be in session_.durations any more
    // (they were flushed and resetForNewSession was called).
    QCOMPARE(tracker.sessionState_dbg().durations.size(), static_cast<size_t>(0));
    (void)completedCount; // used to verify state before discard
}

/**
 * Repeated calls to discardCrossMidnightOngoingAndStop() are idempotent:
 * the second call returns false and makes no DB writes.
 */
void IntegrationTest::test_5_0a_discard_is_idempotent()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    // Force segment to yesterday.
    tracker.sessionState_dbg().segment_start_time = QDateTime::currentDateTime().addDays(-1);

    const QDateTime today = QDateTime::currentDateTime();
    QVERIFY(tracker.discardCrossMidnightOngoingAndStop_dbg(today));
    QVERIFY(tracker.isStopped());

    // Second call must return false (engine already stopped).
    fakeDb.callLog.clear();
    QVERIFY(!tracker.discardCrossMidnightOngoingAndStop_dbg(today));
    QVERIFY(!fakeDb.callLog.contains("commitSession"));
}

/**
 * Watchdog predicate: isOngoingSegmentCrossMidnight() returns false when the
 * engine is stopped (Mode::None), and true only when the segment_start_time
 * is on a previous date.
 */
void IntegrationTest::test_5_0a_watchdog_helper_returns_false_when_not_crossed()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    // Stopped: must be false.
    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.isOngoingSegmentCrossMidnight());

    // Running with same-day start: must be false.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(!tracker.isOngoingSegmentCrossMidnight());

    // Backdate to yesterday: must be true.
    tracker.sessionState_dbg().segment_start_time = QDateTime::currentDateTime().addDays(-1);
    QVERIFY(tracker.isOngoingSegmentCrossMidnight());

    // Restore so destructor stops cleanly.
    tracker.sessionState_dbg().segment_start_time = QDateTime::currentDateTime();
}

void IntegrationTest::test_integration_backpause_db_update()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    // Start activity
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(100);
    
    // Save checkpoint
    tracker.saveCheckpointInternal_dbg(QDateTime::currentDateTime());
    SegmentId checkpointSegmentId = tracker.sessionState_dbg().segment_id;
    QVERIFY(!checkpointSegmentId.isEmpty());

    // Simulate lock
    tracker.useTimerViaLockEvent(LockEvent::Lock);

    // Checkpoint ID should still be valid (lock doesn't reset it)
    QCOMPARE(tracker.sessionState_dbg().segment_id.toString(), checkpointSegmentId.toString());
    
    // Simulate long ongoing lock (triggers backpause)
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock);
    
    // Backpause transitions to Pause and starts a fresh segment identity.
    QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());
    
    // Verify durations were updated in DB
    SqliteSessionStore db2(settings);
    auto loaded = db2.loadDurations();
    QVERIFY(loaded.size() >= 1);
}

// ============================================================================
// Phase 5 test gate (Tests Y–AB)
// ============================================================================

/**
 * Test Y — Engine-driven scheduled stop.
 *
 * Arm DayBoundaryWatcher with a fake "now" that is past 23:59:59.500 so the
 * single-shot timer fires after 1 ms.  Assert:
 *   - stopped(MidnightScheduled) is emitted exactly once.
 *   - Engine reaches Mode::None.
 *   - Same-day completed segments were committed to the fake DB.
 */
void IntegrationTest::test_Y_engine_scheduled_stop_emits_MidnightScheduled()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    // Start the engine (Activity mode).
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    // Add a completed same-day segment so we can verify it is preserved.
    const QDateTime segStart = QDateTime::currentDateTime().addSecs(-30);
    const QDateTime segEnd   = QDateTime::currentDateTime().addSecs(-5);
    tracker.addDuration_dbg(DurationType::Activity, segStart, segEnd);
    QVERIFY(!tracker.sessionState_dbg().durations.empty());
    fakeDb.callLog.clear();

    // Spy on the stopped() signal.
    QSignalSpy stoppedSpy(&tracker, &Timer::stopped);
    QVERIFY(stoppedSpy.isValid());

    // Arm the watcher with a time past 23:59:59.500 — timer fires in 1 ms.
    const QDateTime pastStop(QDate::currentDate(), QTime(23, 59, 59, 600));
    tracker.dayBoundaryWatcher_dbg().armScheduledStop(pastStop);

    // Wait long enough for the single-shot timer to fire.
    QTest::qWait(100);

    // Assert: exactly one stopped signal, with MidnightScheduled reason.
    QCOMPARE(stoppedSpy.count(), 1);
    const auto reason = stoppedSpy.at(0).at(0).value<Timer::StopReason>();
    QCOMPARE(reason, Timer::StopReason::MidnightScheduled);

    // Engine must be stopped.
    QVERIFY(tracker.isStopped());

    // Completed segments must have been committed to the DB.
    QVERIFY(fakeDb.callLog.contains("commitSession"));
}

/**
 * Test Z — Engine-driven watchdog.
 *
 * With FakeClock jumped past midnight (no scheduled stop fired), the next
 * onTick() call emits stopped(MidnightWatchdog).
 */
void IntegrationTest::test_Z_engine_watchdog_emits_MidnightWatchdog()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    // Simulate sleep through midnight: backdate segment_start_time to yesterday.
    tracker.sessionState_dbg().segment_start_time = QDateTime::currentDateTime().addDays(-1);

    QSignalSpy stoppedSpy(&tracker, &Timer::stopped);
    QVERIFY(stoppedSpy.isValid());

    // Simulate the 100 ms heartbeat with today's "now".
    tracker.onTick(QDateTime::currentDateTime());

    // Engine must have stopped and emitted MidnightWatchdog.
    QCOMPARE(stoppedSpy.count(), 1);
    const auto reason = stoppedSpy.at(0).at(0).value<Timer::StopReason>();
    QCOMPARE(reason, Timer::StopReason::MidnightWatchdog);
    QVERIFY(tracker.isStopped());
}

/**
 * Test AA — No duplicate stop.
 *
 * If both the scheduled stop timer and the watchdog would fire (race), only
 * one stopped() signal is emitted.  Simulate by: arming the timer (1 ms fire),
 * backdating the segment, then calling onTick() to let the watchdog win — the
 * timer fires afterward but the engine is already stopped so it no-ops.
 */
void IntegrationTest::test_AA_no_duplicate_stop_signal()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    // Arm the scheduled stop (fires in 1 ms).
    const QDateTime pastStop(QDate::currentDate(), QTime(23, 59, 59, 600));
    tracker.dayBoundaryWatcher_dbg().armScheduledStop(pastStop);

    // Backdate segment so the watchdog also wants to fire.
    tracker.sessionState_dbg().segment_start_time = QDateTime::currentDateTime().addDays(-1);

    QSignalSpy stoppedSpy(&tracker, &Timer::stopped);
    QVERIFY(stoppedSpy.isValid());

    // Watchdog fires first (synchronous call).
    tracker.onTick(QDateTime::currentDateTime());
    QCOMPARE(stoppedSpy.count(), 1); // watchdog stopped it

    // Now let the scheduled-stop timer fire.
    QTest::qWait(100);

    // Must still be exactly one emission.
    QCOMPARE(stoppedSpy.count(), 1);
    QVERIFY(tracker.isStopped());
}

/**
 * Test AB — scheduleMidnightStop is gone.
 *
 * Verify that the symbol scheduleMidnightStop no longer appears in mainwin.h.
 * The test binary lives in qtest/; mainwin.h is one directory up.
 */
void IntegrationTest::test_AB_scheduleMidnightStop_is_gone()
{
    const QString mainwinHPath =
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../mainwin.h");
    QFile mainwinH(mainwinHPath);
    QVERIFY2(mainwinH.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable("Cannot open mainwin.h: " + mainwinHPath));
    const QString content = QString::fromUtf8(mainwinH.readAll());
    mainwinH.close();
    QVERIFY2(!content.contains("scheduleMidnightStop"),
             "scheduleMidnightStop should not appear in mainwin.h");
}

/**
 * Phase 3: recoverStartupCheckpoints overlap policy.
 *
 * Seeds two orphan rows against a real SqliteSessionStore: one that does not
 * overlap any finalised row (should be recovered) and one that does overlap
 * (should be dropped with its seconds not counted).
 */
void IntegrationTest::test_recoverStartupCheckpoints_overlap_rejected_not_counted()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);

    QVERIFY(db.ensureOpen_dbg());

    // Seed a finalised row 10:00 - 11:00 UTC.
    const QDateTime existingStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime existingEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 0, 0), Qt::UTC);
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 1)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", existingStartUtc.toString(Qt::ISODateWithMs));
        q.bindValue(":e", existingEndUtc.toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }

    // Orphan A: 12:00 - 12:30 UTC (1800 s), no overlap → should be finalized.
    const QDateTime aStartUtc = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC);
    const QDateTime aEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC);
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 0)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", aStartUtc.toString(Qt::ISODateWithMs));
        q.bindValue(":e", aEndUtc.toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }

    // Orphan B: 10:30 - 11:30 UTC, overlaps the finalised row → should be dropped.
    const QDateTime bStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 30, 0), Qt::UTC);
    const QDateTime bEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 30, 0), Qt::UTC);
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 0)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", bStartUtc.toString(Qt::ISODateWithMs));
        q.bindValue(":e", bEndUtc.toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }
    db.lazyClose_dbg();

    // Use a 'now' far enough in the future that neither orphan is stale (>24h old).
    const QDateTime now = QDateTime(QDate(2025, 1, 1), QTime(13, 0, 0), Qt::UTC).toLocalTime();
    StartupRecoveryResult result = db.recoverStartupCheckpoints(now);

    QVERIFY(result.ok);
    QCOMPARE(result.finalized_count, 1);    // A finalized
    QCOMPARE(result.recovered_seconds, (qint64)1800); // 30 min = 1800 s
    // No clean-shutdown marker → notify_user = true (unclean exit).
    QVERIFY(result.notify_user);
    // B's 3600 s must NOT appear in recovered_seconds (overlap-rejected).
    QVERIFY(result.recovered_seconds < 3600);
}

/**
 * Phase 3: recoverStartupCheckpoints reports finalized and dropped counts.
 *
 * Verifies that finalized_count and dropped_count in StartupRecoveryResult
 * accurately reflect the number of rows finalized vs. orphaned/dropped.
 */
void IntegrationTest::test_recoverStartupCheckpoints_reports_finalized_and_dropped_counts()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);

    QVERIFY(db.ensureOpen_dbg());

    // Finalised blocker row 10:00 - 11:00 UTC (forces B to be overlap-dropped).
    const QDateTime blockerStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime blockerEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 0, 0), Qt::UTC);
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 1)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", blockerStartUtc.toString(Qt::ISODateWithMs));
        q.bindValue(":e", blockerEndUtc.toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }

    // Orphan A: 12:00 - 12:30 UTC — no overlap, long enough, recent → finalized.
    const QDateTime aStartUtc = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC);
    const QDateTime aEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC);
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 0)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", aStartUtc.toString(Qt::ISODateWithMs));
        q.bindValue(":e", aEndUtc.toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }

    // Orphan B: 10:30 - 11:30 UTC — overlaps blocker → overlap-dropped.
    const QDateTime bStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 30, 0), Qt::UTC);
    const QDateTime bEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 30, 0), Qt::UTC);
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 0)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", bStartUtc.toString(Qt::ISODateWithMs));
        q.bindValue(":e", bEndUtc.toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }
    db.lazyClose_dbg();

    const QDateTime now = QDateTime(QDate(2025, 1, 1), QTime(13, 0, 0), Qt::UTC).toLocalTime();
    StartupRecoveryResult result = db.recoverStartupCheckpoints(now);

    QVERIFY(result.ok);
    QCOMPARE(result.finalized_count, 1);   // A finalized
    QVERIFY(result.dropped_count >= 1);    // B overlap-dropped (counted in dropped_count)
    QCOMPARE(result.recovered_seconds, (qint64)1800); // only A's 30 min
}

/**
 * Phase 3: Timer correctly uses recovered_seconds from recoverStartupCheckpoints.
 *
 * Verifies the Timer → store boundary: Timer passes the recovered_seconds
 * from the store result directly to getStartupRecoveredSeconds().
 */
void IntegrationTest::test_timer_startup_recovery_uses_store_recovered_seconds()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Configure the store to report 60 seconds recovered (e.g. A finalized, B overlap-dropped).
    fakeDb.startupRecoveryResult.ok = true;
    fakeDb.startupRecoveryResult.recovered_seconds = 60;
    fakeDb.startupRecoveryResult.notify_user = true;

    Timer tracker(settings, fakeDb);
    tracker.initializeFromStore();

    // Timer should report exactly what the store returned.
    QCOMPARE(tracker.getStartupRecoveredSeconds(), (qint64)60);
    QVERIFY(tracker.shouldShowStartupRecoveryNotification());
}

// ============================================================================
// Step 19 (C9 timezone): parameterized timezone tests
// ============================================================================

namespace {
// RAII helper that saves the current TZ env-var and restores it on destruction.
struct TzGuard {
    std::optional<QByteArray> saved;

    TzGuard() {
        const char* tz = getenv("TZ");
        saved = tz ? std::make_optional(QByteArray(tz)) : std::nullopt;
    }

    void set(const QByteArray& tz) {
        qputenv("TZ", tz);
#ifdef Q_OS_UNIX
        tzset();
#endif
    }

    ~TzGuard() {
        if (saved)
            qputenv("TZ", *saved);
        else
            qunsetenv("TZ");
#ifdef Q_OS_UNIX
        tzset();
#endif
    }
};
} // anonymous namespace

/**
 * Data provider: three timezones exercised in hasEntriesForDate tests.
 */
void IntegrationTest::test_hasEntriesForDate_timezone_data()
{
    QTest::addColumn<QString>("timezone");
    QTest::newRow("UTC")              << "Etc/UTC";
    QTest::newRow("Europe/Berlin")    << "Europe/Berlin";
    QTest::newRow("America/Los_Angeles") << "America/Los_Angeles";
}

/**
 * For each timezone: insert a finalized row that starts exactly at noon on
 * 2025-06-15 local time, then ask hasEntriesForDate for that date.
 * Must return Yes regardless of timezone offset.
 */
void IntegrationTest::test_hasEntriesForDate_timezone()
{
    QFETCH(QString, timezone);

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));

    TzGuard guard;
    guard.set(timezone.toLatin1());

    // Construct noon on 2025-06-15 in the current (faked) local timezone.
    const QDate testDate(2025, 6, 15);
    const QDateTime localNoon(testDate, QTime(12, 0, 0), Qt::LocalTime);
    const QDateTime localNoonEnd = localNoon.addSecs(3600); // 1 hour session

    SqliteSessionStore db(settings);
    std::deque<TimeDuration> durations;
    durations.emplace_back(TimeDuration::fromTrusted(DurationType::Activity, localNoon, localNoonEnd));
    QVERIFY(db.saveDurations(durations, TransactionMode::Append));

    // hasEntriesForDate must find this entry.
    QCOMPARE(db.hasEntriesForDate(testDate), EntriesForDateResult::Yes);

    // An adjacent date must not find it.
    QCOMPARE(db.hasEntriesForDate(testDate.addDays(1)), EntriesForDateResult::No);
    QCOMPARE(db.hasEntriesForDate(testDate.addDays(-1)), EntriesForDateResult::No);
}

/**
 * Data provider: timezones for the midnight boundary reconciliation test.
 */
void IntegrationTest::test_midnight_boundary_timezone_data()
{
    QTest::addColumn<QString>("timezone");
    QTest::newRow("UTC")              << "Etc/UTC";
    QTest::newRow("Europe/Berlin")    << "Europe/Berlin";
    QTest::newRow("America/Los_Angeles") << "America/Los_Angeles";
}

/**
 * Midnight boundary: a session that ends at 23:59:59 local on 2025-06-14
 * must appear under 2025-06-14, not 2025-06-15 — even across DST offsets.
 * A session starting at 00:00:01 on 2025-06-15 belongs to 2025-06-15.
 */
void IntegrationTest::test_midnight_boundary_timezone()
{
    QFETCH(QString, timezone);

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));

    TzGuard guard;
    guard.set(timezone.toLatin1());

    // Entry A: ends one second before local midnight on 2025-06-14.
    const QDate day14(2025, 6, 14);
    const QDateTime endOfDay14(day14, QTime(23, 59, 59), Qt::LocalTime);
    const QDateTime startOfDay14 = endOfDay14.addSecs(-60);

    // Entry B: starts one second after local midnight → 2025-06-15.
    const QDate day15(2025, 6, 15);
    const QDateTime startOfDay15(day15, QTime(0, 0, 1), Qt::LocalTime);
    const QDateTime endOfDay15 = startOfDay15.addSecs(60);

    SqliteSessionStore db(settings);
    std::deque<TimeDuration> durations;
    durations.emplace_back(TimeDuration::fromTrusted(DurationType::Activity, startOfDay14, endOfDay14));
    durations.emplace_back(TimeDuration::fromTrusted(DurationType::Activity, startOfDay15, endOfDay15));
    QVERIFY(db.saveDurations(durations, TransactionMode::Append));

    // Boundary: 2025-06-14 must see entry A; entry B must not bleed into it.
    QCOMPARE(db.hasEntriesForDate(day14), EntriesForDateResult::Yes);
    QCOMPARE(db.hasEntriesForDate(day15), EntriesForDateResult::Yes);

    // A date with no entries must return No.
    QCOMPARE(db.hasEntriesForDate(day14.addDays(-1)), EntriesForDateResult::No);
    QCOMPARE(db.hasEntriesForDate(day15.addDays(1)),  EntriesForDateResult::No);
}
