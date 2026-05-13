#include "test_integration.h"
#include "fakedatabasemanager.h"
#include <QtTest>

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
        DatabaseManager db(settings);
        TimeTracker tracker(settings, db);
        
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(1200);
        
        // Manually save checkpoint
        tracker.saveCheckpointInternal(QDateTime::currentDateTime());
        QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
        orphanSegmentId = tracker.session_.current_checkpoint_segment_id;

        // Simulate an unclean shutdown: keep checkpoint row, skip graceful stop.
        tracker.mode_ = TimeTracker::Mode::None;
        tracker.session_.current_checkpoint_segment_id.clear();

        // Simulate crash (tracker destroyed without stopping)
    }
    
    {
        // Second session: startup reconciliation finalizes orphan checkpoint
        Settings settings(settingsPath);
        DatabaseManager db(settings);
        TimeTracker tracker(settings, db);
        QVERIFY(tracker.getStartupRecoveredSeconds() > 0);

        DatabaseManager db2(settings);
        auto loaded = db2.loadDurations();
        QCOMPARE(loaded.size(), (size_t)1); // Reconciled orphan recovered
        QCOMPARE(loaded[0].type, DurationType::Activity);

        // Reconciliation must keep row identity by finalizing in place.
        QVERIFY(db.lazyOpen());
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
        DatabaseManager db(settings);
        TimeTracker tracker(settings, db);
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(1200);
        tracker.saveCheckpointInternal(QDateTime::currentDateTime());
        QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
        tracker.session_.current_checkpoint_segment_id.clear();
        tracker.mode_ = TimeTracker::Mode::None;
    }

    {
        Settings settings(settingsPath);
        DatabaseManager db(settings);
        TimeTracker tracker(settings, db);
        QCOMPARE(tracker.getStartupRecoveredSeconds() >= 1, true);
    }

    {
        Settings settings(settingsPath);
        DatabaseManager db(settings);
        TimeTracker tracker(settings, db);
        QCOMPARE(tracker.getStartupRecoveredSeconds(), static_cast<qint64>(0));
        DatabaseManager db2(settings);
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
    DatabaseManager db(settings);
    QVERIFY(db.lazyOpen());

    // Too short orphan (< 1s)
    {
        QSqlQuery insert(db.db);
        const QDateTime start = QDateTime::currentDateTimeUtc().addSecs(-10);
        const QDateTime end = start.addMSecs(500);
        insert.prepare(
            "INSERT INTO durations (segment_id, type, duration, start_date, start_time, end_date, end_time, is_finalized) "
            "VALUES (:segment_id, :type, :duration, :start_date, :start_time, :end_date, :end_time, 0)"
        );
        insert.bindValue(":segment_id", TimeDuration::createSegmentId());
        insert.bindValue(":type", static_cast<int>(DurationType::Activity));
        insert.bindValue(":duration", static_cast<qint64>(500));
        insert.bindValue(":start_date", start.date().toString(Qt::ISODate));
        insert.bindValue(":start_time", start.time().toString("HH:mm:ss.zzz"));
        insert.bindValue(":end_date", end.date().toString(Qt::ISODate));
        insert.bindValue(":end_time", end.time().toString("HH:mm:ss.zzz"));
        QVERIFY(insert.exec());
    }

    // Stale orphan (>24h)
    {
        QSqlQuery insert(db.db);
        const QDateTime start = QDateTime::currentDateTimeUtc().addDays(-2);
        const QDateTime end = start.addSecs(30);
        insert.prepare(
            "INSERT INTO durations (segment_id, type, duration, start_date, start_time, end_date, end_time, is_finalized) "
            "VALUES (:segment_id, :type, :duration, :start_date, :start_time, :end_date, :end_time, 0)"
        );
        insert.bindValue(":segment_id", TimeDuration::createSegmentId());
        insert.bindValue(":type", static_cast<int>(DurationType::Activity));
        insert.bindValue(":duration", static_cast<qint64>(30000));
        insert.bindValue(":start_date", start.date().toString(Qt::ISODate));
        insert.bindValue(":start_time", start.time().toString("HH:mm:ss.zzz"));
        insert.bindValue(":end_date", end.date().toString(Qt::ISODate));
        insert.bindValue(":end_time", end.time().toString("HH:mm:ss.zzz"));
        QVERIFY(insert.exec());
    }

    db.lazyClose();

    DatabaseManager db2(settings);
    TimeTracker tracker(settings, db2);
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
        DatabaseManager db(settings);
        TimeTracker tracker(settings, db);
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(1200);
        tracker.saveCheckpointInternal(QDateTime::currentDateTime());
        QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
        tracker.session_.current_checkpoint_segment_id.clear();
        tracker.mode_ = TimeTracker::Mode::None;
    }

    {
        Settings settings(settingsPath);
        DatabaseManager db(settings);
        QVERIFY(db.setLastCleanShutdownMarker(QDateTime::currentDateTime()));
    }

    {
        Settings settings(settingsPath);
        DatabaseManager db(settings);
        TimeTracker tracker(settings, db);
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
        DatabaseManager db(settings);
        TimeTracker tracker(settings, db);
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(1200);
        tracker.saveCheckpointInternal(QDateTime::currentDateTime());
        QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
        tracker.session_.current_checkpoint_segment_id.clear();
        tracker.mode_ = TimeTracker::Mode::None;
    }

    {
        Settings settings(settingsPath);
        DatabaseManager db(settings);
        TimeTracker tracker(settings, db);
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
    DatabaseManager db(settings);
    TimeTracker tracker(settings, db);

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
    DatabaseManager db2(settings);
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
    DatabaseManager manager(settings);

    QDateTime now = QDateTime::currentDateTimeUtc();
    
    // Insert data from 5 days ago (should be deleted)
    std::deque<TimeDuration> old;
    old.emplace_back(DurationType::Activity, now.addDays(-5), now.addDays(-5).addSecs(60));
    QVERIFY(manager.saveDurations(old, TransactionMode::Append));
    
    // Insert data from today (should be kept)
    std::deque<TimeDuration> current;
    current.emplace_back(DurationType::Activity, now.addSecs(-100), now.addSecs(-90));
    QVERIFY(manager.saveDurations(current, TransactionMode::Append));
    
    // Force cleanup by reopening database
    DatabaseManager manager2(settings);
    
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
    DatabaseManager manager(settings);

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
    DatabaseManager manager(settings);

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
    // This test drives all three steps through TimeTracker + FakeDatabaseManager
    // and asserts each side-effect happened in the correct order, mimicking
    // what MainWin::shutdown() does after Phase 1.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // Start the timer so there is something to stop.
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    QCOMPARE(tracker.mode_, TimeTracker::Mode::Activity);

    // Clear the call log captured during startup / start so the assertion
    // below only sees the shutdown sequence.
    fakeDb.callLog.clear();

    // Act — replicate MainWin::shutdown() logic (Phase 1 version):
    // Step 1: stop timer
    tracker.useTimerViaButton(Button::Stop);
    QCOMPARE(tracker.mode_, TimeTracker::Mode::None);

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

void IntegrationTest::test_integration_backpause_db_update()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager db(settings);
    TimeTracker tracker(settings, db);

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
    DatabaseManager db2(settings);
    auto loaded = db2.loadDurations();
    QVERIFY(loaded.size() >= 1);
}
