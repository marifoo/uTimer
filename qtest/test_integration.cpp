#include "test_integration.h"
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
    
    QDateTime start = QDateTime::currentDateTime();
    
    {
        // First session: Start timer and save checkpoint
        Settings settings(settingsPath);
        TimeTracker tracker(settings);
        
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(100);
        
        // Manually save checkpoint
        tracker.saveCheckpointInternal();
        QVERIFY(tracker.current_checkpoint_id_ != -1);
        
        // Simulate crash (tracker destroyed without stopping)
    }
    
    {
        // Second session: Load from database (simulates app restart)
        Settings settings(settingsPath);
        DatabaseManager db(settings);
        
        auto loaded = db.loadDurations();
        QCOMPARE(loaded.size(), (size_t)1); // Checkpoint recovered
        QCOMPARE(loaded[0].type, DurationType::Activity);
    }
}

void IntegrationTest::test_integration_memory_db_consistency()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

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
    tracker.saveCheckpointInternal();
    QVERIFY(tracker.current_checkpoint_id_ != -1);
    
    // Stop
    tracker.useTimerViaButton(Button::Stop);
    
    // Load from DB
    DatabaseManager db(settings);
    auto dbDurations = db.loadDurations();
    
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
    
    // Insert same duration twice using saveDurations
    std::deque<TimeDuration> durations;
    durations.emplace_back(DurationType::Activity, start, end);
    
    QVERIFY(manager.saveDurations(durations, TransactionMode::Append));
    QVERIFY(manager.saveDurations(durations, TransactionMode::Append));
    
    // Load - due to UNIQUE constraint, should have only 1 entry
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
    
    QVERIFY(!manager.hasEntriesForDate(QDate::currentDate()));
    
    std::deque<TimeDuration> empty;
    QVERIFY(manager.updateDurationsByStartTime(empty));
}

void IntegrationTest::test_integration_backpause_db_update()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

    // Start activity
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(100);
    
    // Save checkpoint
    tracker.saveCheckpointInternal();
    long long checkpointId = tracker.current_checkpoint_id_;
    QVERIFY(checkpointId != -1);
    
    // Simulate lock
    tracker.useTimerViaLockEvent(LockEvent::Lock);
    
    // Checkpoint ID should still be valid (lock doesn't reset it)
    QCOMPARE(tracker.current_checkpoint_id_, checkpointId);
    
    // Simulate long ongoing lock (triggers backpause)
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock);
    
    // Verify checkpoint ID was reset by backpause
    QCOMPARE(tracker.current_checkpoint_id_, (long long)-1);
    
    // Verify durations were updated in DB
    DatabaseManager db(settings);
    auto loaded = db.loadDurations();
    QVERIFY(loaded.size() >= 1);
}
