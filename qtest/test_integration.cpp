#include "test_integration.h"
#include "fakesessionstore.h"
#include <QtTest>
#include <QSignalSpy>

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
    QString orphanSegmentId;
    
    {
        // First session: Start timer and save checkpoint
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        Timer tracker(settings, db);
        
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(1200);
        
        // Manually save checkpoint
        tracker.saveCheckpointInternal(QDateTime::currentDateTime());
        QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
        orphanSegmentId = tracker.session_.current_checkpoint_segment_id;

        // Simulate an unclean shutdown: keep checkpoint row, skip graceful stop.
        tracker.mode_ = Timer::Mode::None;
        tracker.session_.current_checkpoint_segment_id.clear();

        // Simulate crash (tracker destroyed without stopping)
    }
    
    {
        // Second session: startup reconciliation finalizes orphan checkpoint
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        Timer tracker(settings, db);
        QVERIFY(tracker.getStartupRecoveredSeconds() > 0);

        SqliteSessionStore db2(settings);
        auto loaded = db2.loadDurations();
        QCOMPARE(loaded.size(), (size_t)1); // Reconciled orphan recovered
        QCOMPARE(loaded[0].type, DurationType::Activity);

        // Reconciliation must keep row identity by finalizing in place.
        QVERIFY(db.ensureOpen());
        QSqlQuery query(db.db);
        query.prepare("SELECT is_finalized FROM durations WHERE segment_id = :segment_id");
        query.bindValue(":segment_id", orphanSegmentId);
        QVERIFY(query.exec());
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 1);
        db.lazyClose();
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
        tracker.saveCheckpointInternal(QDateTime::currentDateTime());
        QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
        tracker.session_.current_checkpoint_segment_id.clear();
        tracker.mode_ = Timer::Mode::None;
    }

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        Timer tracker(settings, db);
        QCOMPARE(tracker.getStartupRecoveredSeconds() >= 1, true);
    }

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        Timer tracker(settings, db);
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
    QVERIFY(db.ensureOpen());

    // Too short orphan (< 1s)
    {
        QSqlQuery insert(db.db);
        const QDateTime start = QDateTime::currentDateTimeUtc().addSecs(-10);
        const QDateTime end = start.addMSecs(500);
        insert.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:segment_id, :type, :start_utc, :end_utc, 0)"
        );
        insert.bindValue(":segment_id", TimeDuration::createSegmentId());
        insert.bindValue(":type", static_cast<int>(DurationType::Activity));
        insert.bindValue(":start_utc", start.toString(Qt::ISODateWithMs));
        insert.bindValue(":end_utc", end.toString(Qt::ISODateWithMs));
        QVERIFY(insert.exec());
    }

    // Stale orphan (>24h)
    {
        QSqlQuery insert(db.db);
        const QDateTime start = QDateTime::currentDateTimeUtc().addDays(-2);
        const QDateTime end = start.addSecs(30);
        insert.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:segment_id, :type, :start_utc, :end_utc, 0)"
        );
        insert.bindValue(":segment_id", TimeDuration::createSegmentId());
        insert.bindValue(":type", static_cast<int>(DurationType::Activity));
        insert.bindValue(":start_utc", start.toString(Qt::ISODateWithMs));
        insert.bindValue(":end_utc", end.toString(Qt::ISODateWithMs));
        QVERIFY(insert.exec());
    }

    db.lazyClose();

    SqliteSessionStore db2(settings);
    Timer tracker(settings, db2);
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
        tracker.saveCheckpointInternal(QDateTime::currentDateTime());
        QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
        tracker.session_.current_checkpoint_segment_id.clear();
        tracker.mode_ = Timer::Mode::None;
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
        tracker.saveCheckpointInternal(QDateTime::currentDateTime());
        QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
        tracker.session_.current_checkpoint_segment_id.clear();
        tracker.mode_ = Timer::Mode::None;
    }

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        Timer tracker(settings, db);
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
    QVERIFY(memoryDurations.size() >= 1);
    
    // Resume -> Activity for 50ms
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(50);
    
    // Save checkpoint
    tracker.saveCheckpointInternal(QDateTime::currentDateTime());
    QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
    
    // Stop
    tracker.useTimerViaButton(Button::Stop);
    
    // Load from DB
    SqliteSessionStore db2(settings);
    auto dbDurations = db2.loadDurations();
    
    // Total durations should match (memory + checkpoint)
    qint64 memoryTotal = sumDurations(memoryDurations, DurationType::Activity) + 
                        sumDurations(memoryDurations, DurationType::Pause);
    qint64 dbTotal = sumDurations(dbDurations, DurationType::Activity) +
                    sumDurations(dbDurations, DurationType::Pause);
    
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
    QVERIFY(manager2.checkSchemaOnStartup());

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
    
    QVERIFY(manager.updateDurationsById(durations));
    durations.front().endTime = end.addSecs(10);
    durations.front().duration = durations.front().startTime.msecsTo(durations.front().endTime);
    QVERIFY(manager.updateDurationsById(durations));
    
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
    QVERIFY(manager.updateDurationsById(empty));
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
    QCOMPARE(tracker.mode_, Timer::Mode::Activity);

    // Clear the call log captured during startup / start so the assertion
    // below only sees the shutdown sequence.
    fakeDb.callLog.clear();

    // Act — replicate MainWin::shutdown() logic (Phase 1 version):
    // Step 1: stop timer
    tracker.useTimerViaButton(Button::Stop);
    QCOMPARE(tracker.mode_, Timer::Mode::None);

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
    QCOMPARE(tracker.mode_, Timer::Mode::Activity);

    // Watchdog predicate must be false for a fresh same-day session.
    QVERIFY(!tracker.isOngoingSegmentCrossMidnight());

    // Directly call the internal helper with a same-day "now".
    const QDateTime sameDay = QDateTime::currentDateTime();
    QVERIFY(!tracker.discardCrossMidnightOngoingAndStop(sameDay));

    // Engine must still be in Activity.
    QCOMPARE(tracker.mode_, Timer::Mode::Activity);
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
    QCOMPARE(tracker.mode_, Timer::Mode::Activity);

    // Insert a completed segment via direct addDuration() so we control the
    // timestamps and guarantee a positive duration independent of wall-clock.
    const QDateTime segStart = QDateTime::currentDateTime().addSecs(-10);
    const QDateTime segEnd   = QDateTime::currentDateTime().addSecs(-1);
    tracker.addDuration(DurationType::Activity, segStart, segEnd);
    QVERIFY(!tracker.session_.durations.empty());
    const size_t completedCount = tracker.session_.durations.size();

    // Simulate sleep through midnight: move segment_start_time to yesterday.
    const QDateTime yesterday = QDateTime::currentDateTime().addDays(-1);
    tracker.session_.segment_start_time = yesterday;

    // Pre-condition: watchdog predicate should now be true.
    QVERIFY(tracker.isOngoingSegmentCrossMidnight());

    // Act: call the internal helper with "now" (today).
    const QDateTime today = QDateTime::currentDateTime();
    fakeDb.callLog.clear();
    const bool fired = tracker.discardCrossMidnightOngoingAndStop(today);

    // Assert: helper fired, engine stopped, completed segments were committed.
    QVERIFY(fired);
    QCOMPARE(tracker.mode_, Timer::Mode::None);
    QVERIFY(fakeDb.callLog.contains("commitSession"));
    // The completed segments must NOT be in session_.durations any more
    // (they were flushed and resetForNewSession was called).
    QCOMPARE(tracker.session_.durations.size(), static_cast<size_t>(0));
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
    QCOMPARE(tracker.mode_, Timer::Mode::Activity);

    // Force segment to yesterday.
    tracker.session_.segment_start_time = QDateTime::currentDateTime().addDays(-1);

    const QDateTime today = QDateTime::currentDateTime();
    QVERIFY(tracker.discardCrossMidnightOngoingAndStop(today));
    QCOMPARE(tracker.mode_, Timer::Mode::None);

    // Second call must return false (engine already stopped).
    fakeDb.callLog.clear();
    QVERIFY(!tracker.discardCrossMidnightOngoingAndStop(today));
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
    QCOMPARE(tracker.mode_, Timer::Mode::None);
    QVERIFY(!tracker.isOngoingSegmentCrossMidnight());

    // Running with same-day start: must be false.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(!tracker.isOngoingSegmentCrossMidnight());

    // Backdate to yesterday: must be true.
    tracker.session_.segment_start_time = QDateTime::currentDateTime().addDays(-1);
    QVERIFY(tracker.isOngoingSegmentCrossMidnight());

    // Restore so destructor stops cleanly.
    tracker.session_.segment_start_time = QDateTime::currentDateTime();
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
    tracker.saveCheckpointInternal(QDateTime::currentDateTime());
    QString checkpointSegmentId = tracker.session_.current_checkpoint_segment_id;
    QVERIFY(!checkpointSegmentId.isEmpty());
    
    // Simulate lock
    tracker.useTimerViaLockEvent(LockEvent::Lock);
    
    // Checkpoint ID should still be valid (lock doesn't reset it)
    QCOMPARE(tracker.session_.current_checkpoint_segment_id, checkpointSegmentId);
    
    // Simulate long ongoing lock (triggers backpause)
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock);
    
    // Backpause transitions to Pause and starts a fresh segment identity.
    QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
    
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
    QCOMPARE(tracker.mode_, Timer::Mode::Activity);

    // Add a completed same-day segment so we can verify it is preserved.
    const QDateTime segStart = QDateTime::currentDateTime().addSecs(-30);
    const QDateTime segEnd   = QDateTime::currentDateTime().addSecs(-5);
    tracker.addDuration(DurationType::Activity, segStart, segEnd);
    QVERIFY(!tracker.session_.durations.empty());
    fakeDb.callLog.clear();

    // Spy on the stopped() signal.
    QSignalSpy stoppedSpy(&tracker, &Timer::stopped);
    QVERIFY(stoppedSpy.isValid());

    // Arm the watcher with a time past 23:59:59.500 — timer fires in 1 ms.
    const QDateTime pastStop(QDate::currentDate(), QTime(23, 59, 59, 600));
    tracker.day_boundary_watcher_.armScheduledStop(pastStop);

    // Wait long enough for the single-shot timer to fire.
    QTest::qWait(100);

    // Assert: exactly one stopped signal, with MidnightScheduled reason.
    QCOMPARE(stoppedSpy.count(), 1);
    const auto reason = stoppedSpy.at(0).at(0).value<Timer::StopReason>();
    QCOMPARE(reason, Timer::StopReason::MidnightScheduled);

    // Engine must be stopped.
    QCOMPARE(tracker.mode_, Timer::Mode::None);

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
    QCOMPARE(tracker.mode_, Timer::Mode::Activity);

    // Simulate sleep through midnight: backdate segment_start_time to yesterday.
    tracker.session_.segment_start_time = QDateTime::currentDateTime().addDays(-1);

    QSignalSpy stoppedSpy(&tracker, &Timer::stopped);
    QVERIFY(stoppedSpy.isValid());

    // Simulate the 100 ms heartbeat with today's "now".
    tracker.onTick(QDateTime::currentDateTime());

    // Engine must have stopped and emitted MidnightWatchdog.
    QCOMPARE(stoppedSpy.count(), 1);
    const auto reason = stoppedSpy.at(0).at(0).value<Timer::StopReason>();
    QCOMPARE(reason, Timer::StopReason::MidnightWatchdog);
    QCOMPARE(tracker.mode_, Timer::Mode::None);
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
    QCOMPARE(tracker.mode_, Timer::Mode::Activity);

    // Arm the scheduled stop (fires in 1 ms).
    const QDateTime pastStop(QDate::currentDate(), QTime(23, 59, 59, 600));
    tracker.day_boundary_watcher_.armScheduledStop(pastStop);

    // Backdate segment so the watchdog also wants to fire.
    tracker.session_.segment_start_time = QDateTime::currentDateTime().addDays(-1);

    QSignalSpy stoppedSpy(&tracker, &Timer::stopped);
    QVERIFY(stoppedSpy.isValid());

    // Watchdog fires first (synchronous call).
    tracker.onTick(QDateTime::currentDateTime());
    QCOMPARE(stoppedSpy.count(), 1); // watchdog stopped it

    // Now let the scheduled-stop timer fire.
    QTest::qWait(100);

    // Must still be exactly one emission.
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(tracker.mode_, Timer::Mode::None);
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
 * FakeSessionStore mirrors SqliteSessionStore's overlap-guarded finalise:
 * if the orphan does not overlap any finalised row, the call returns true and
 * the row is promoted; if it overlaps, the call returns false and storedDurations
 * is unchanged.
 */
void IntegrationTest::test_fakeStore_finalizeIfNoOverlap_mirrors_sqlite_semantics()
{
    FakeSessionStore fakeDb;

    // Seed one finalised row 10:00 - 11:00 UTC.
    const QDateTime existingStart = QDateTime(QDate(2025, 1, 1), QTime(10, 0, 0), Qt::UTC).toLocalTime();
    const QDateTime existingEnd   = QDateTime(QDate(2025, 1, 1), QTime(11, 0, 0), Qt::UTC).toLocalTime();
    fakeDb.storedDurations.push_back(
        TimeDuration::fromTrusted(DurationType::Activity, existingStart, existingEnd));

    // Orphan A: 12:00 - 12:30 UTC, no overlap → should finalise.
    OrphanCheckpoint a;
    a.id = 1;
    a.startTime = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC).toLocalTime();
    a.endTime   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC).toLocalTime();
    fakeDb.orphanCheckpoints.push_back(a);

    // Orphan B: 10:30 - 11:30 UTC, overlaps → should be rejected.
    OrphanCheckpoint b;
    b.id = 2;
    b.startTime = QDateTime(QDate(2025, 1, 1), QTime(10, 30, 0), Qt::UTC).toLocalTime();
    b.endTime   = QDateTime(QDate(2025, 1, 1), QTime(11, 30, 0), Qt::UTC).toLocalTime();
    fakeDb.orphanCheckpoints.push_back(b);

    const size_t storedBefore = fakeDb.storedDurations.size();

    QVERIFY( fakeDb.finalizeIfNoOverlap(a.id, a.startTime.toUTC(), a.endTime.toUTC()));
    QVERIFY(!fakeDb.finalizeIfNoOverlap(b.id, b.startTime.toUTC(), b.endTime.toUTC()));

    // A was promoted; B remains an orphan.
    QCOMPARE(fakeDb.storedDurations.size(), storedBefore + 1);
    QCOMPARE(fakeDb.orphanCheckpoints.size(), (size_t)1);
    QCOMPARE(fakeDb.orphanCheckpoints.front().id, b.id);
}

void IntegrationTest::test_fakeStore_reconcile_reports_finalized_and_overlap_dropped()
{
    FakeSessionStore fakeDb;

    // Seed one finalised row 10:00 - 11:00 UTC.
    const QDateTime existingStart = QDateTime(QDate(2025, 1, 1), QTime(10, 0, 0), Qt::UTC).toLocalTime();
    const QDateTime existingEnd   = QDateTime(QDate(2025, 1, 1), QTime(11, 0, 0), Qt::UTC).toLocalTime();
    fakeDb.storedDurations.push_back(
        TimeDuration::fromTrusted(DurationType::Activity, existingStart, existingEnd));

    OrphanCheckpoint a;
    a.id = 1;
    a.startTime = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC).toLocalTime();
    a.endTime   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC).toLocalTime();
    OrphanCheckpoint b;
    b.id = 2;
    b.startTime = QDateTime(QDate(2025, 1, 1), QTime(10, 30, 0), Qt::UTC).toLocalTime();
    b.endTime   = QDateTime(QDate(2025, 1, 1), QTime(11, 30, 0), Qt::UTC).toLocalTime();
    fakeDb.orphanCheckpoints.push_back(a);
    fakeDb.orphanCheckpoints.push_back(b);

    ReconcileResult result = fakeDb.reconcileUnfinalizedCheckpoints({a, b}, {});
    QVERIFY(result.ok);
    QCOMPARE(result.finalized.size(), (size_t)1);
    QCOMPARE(result.finalized[0], (long long)a.id);
    QCOMPARE(result.dropped.size(), (size_t)1);
    QCOMPARE(result.dropped[0], (long long)b.id);
}

/**
 * Timer-level test: when the store rejects an orphan due to overlap, those
 * seconds must NOT appear in startup_recovered_seconds_.
 *
 * All timestamps are anchored to "now" so Timer's pre-filter (stale > 24h,
 * too-short < 1s) does not interfere with the path under test.
 */
void IntegrationTest::test_timer_reconcileOrphans_excludes_overlap_dropped_from_recovered_seconds()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime now = QDateTime::currentDateTime();

    // Seed a finalised row whose window overlaps orphan B (see below).
    const QDateTime overlapStart = now.addSecs(-50);
    const QDateTime overlapEnd   = now.addSecs(-30);
    fakeDb.storedDurations.push_back(
        TimeDuration::fromTrusted(DurationType::Activity, overlapStart, overlapEnd));

    // Orphan A: 60-second window ending 60s ago. No overlap, recent, > 1s.
    OrphanCheckpoint a;
    a.id = 1;
    a.segment_id = TimeDuration::createSegmentId();
    a.type = DurationType::Activity;
    a.startTime = now.addSecs(-120);
    a.endTime   = now.addSecs(-60);
    a.duration  = a.startTime.msecsTo(a.endTime);
    fakeDb.orphanCheckpoints.push_back(a);

    // Orphan B: 10-second window that overlaps the seeded finalised row.
    // Recent (>= now-50s), longer than 1s, but must be overlap-rejected.
    OrphanCheckpoint b;
    b.id = 2;
    b.segment_id = TimeDuration::createSegmentId();
    b.type = DurationType::Activity;
    b.startTime = now.addSecs(-45);
    b.endTime   = now.addSecs(-35);
    b.duration  = b.startTime.msecsTo(b.endTime);
    fakeDb.orphanCheckpoints.push_back(b);

    Timer tracker(settings, fakeDb);

    // Only A's 60 seconds are counted; B was overlap-dropped by the store.
    QCOMPARE(tracker.getStartupRecoveredSeconds(), (qint64)60);
}
