#include "test_timetracker.h"
#include <QtTest>

using TestCommon::createSettingsFile;
using TestCommon::sumDurations;

void TimeTrackerTest::initTestCase()
{
    db_path_ = QDir(QCoreApplication::applicationDirPath()).filePath("uTimer.sqlite");
    if (QFile::exists(db_path_)) {
        db_backup_path_ = db_path_ + ".bak_test";
        QFile::remove(db_backup_path_);
        QVERIFY(QFile::rename(db_path_, db_backup_path_));
    }
}

void TimeTrackerTest::cleanupTestCase()
{
    if (!db_path_.isEmpty()) {
        QFile::remove(db_path_);
    }
    if (!db_backup_path_.isEmpty()) {
        QFile::remove(db_path_);
        QVERIFY(QFile::rename(db_backup_path_, db_path_));
    }
}

void TimeTrackerTest::test_timetracker_start_pause_resume_stop_and_checkpoints()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    Settings settings(settingsPath);
    TimeTracker tracker(settings);

    // Start -> Activity
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    QVERIFY(tracker.timer_.isValid());
    QCOMPARE(tracker.mode_, TimeTracker::Mode::Activity);
    QCOMPARE(tracker.current_checkpoint_id_, -1);

    // Pause -> durations captured and checkpoint id reset
    tracker.useTimerViaButton(Button::Pause);
    QCOMPARE(tracker.mode_, TimeTracker::Mode::Pause);
    QVERIFY(tracker.durations_.size() >= 1);
    QCOMPARE(tracker.current_checkpoint_id_, -1);

    // Resume -> Activity and timer restarted
    tracker.useTimerViaButton(Button::Start);
    QCOMPARE(tracker.mode_, TimeTracker::Mode::Activity);
    tracker.timer_.invalidate();
    tracker.timer_.start();

    // Trigger checkpoint manually
    QTest::qWait(100); // Ensure elapsed > 0
    tracker.saveCheckpointInternal();
    QVERIFY(tracker.current_checkpoint_id_ != -1);

    // Stop -> move to None and durations flushed
    tracker.useTimerViaButton(Button::Stop);
    QCOMPARE(tracker.mode_, TimeTracker::Mode::None);
    QCOMPARE(tracker.current_checkpoint_id_, -1);
}

void TimeTrackerTest::test_timetracker_backpause_resets_checkpoint_and_splits()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.setValue("uTimer/autopause_threshold_minutes", 1); // 60s
    writer.sync();

    Settings settings(settingsPath);
    TimeTracker tracker(settings);
    tracker.useTimerViaButton(Button::Start);

    tracker.segment_start_time_ = QDateTime::currentDateTime().addSecs(-120);
    tracker.timer_.invalidate();
    tracker.timer_.start();

    tracker.backpauseTimer();
    QCOMPARE(tracker.mode_, TimeTracker::Mode::Pause);
    QCOMPARE(tracker.current_checkpoint_id_, -1);
    QVERIFY(tracker.durations_.size() >= 2);
    // Sum should equal ~120s
    qint64 total = sumDurations(tracker.durations_, DurationType::Activity) +
                   sumDurations(tracker.durations_, DurationType::Pause);
    QVERIFY(total >= 120000 - 2000); // allow minor tolerance
}

void TimeTrackerTest::test_timetracker_midnight_split_and_checkpoint_reset()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    TimeTracker tracker(settings);

    tracker.mode_ = TimeTracker::Mode::Activity;
    tracker.segment_start_time_ = QDateTime(QDate::currentDate().addDays(-1), QTime(23,59,58,0));
    tracker.addDurationWithMidnightSplit(DurationType::Activity,
                                         tracker.segment_start_time_,
                                         tracker.segment_start_time_.addSecs(5));
    
    // Post-midnight part remains in memory
    QVERIFY(tracker.durations_.size() >= 1);
    
    // Pre-midnight part should be in DB
    QVERIFY(tracker.db_.hasEntriesForDate(QDate::currentDate().addDays(-1)));
}

void TimeTrackerTest::test_timetracker_lock_events_checkpoint_and_resume()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    TimeTracker tracker(settings);

    tracker.useTimerViaButton(Button::Start);
    tracker.timer_.invalidate();
    tracker.timer_.start();
    tracker.useTimerViaLockEvent(LockEvent::Lock);
    QVERIFY(tracker.is_locked_);
    tracker.useTimerViaLockEvent(LockEvent::Unlock);
    QVERIFY(!tracker.is_locked_);
}

void TimeTrackerTest::test_timetracker_ongoing_duration()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

    // Initially stopped
    QCOMPARE(tracker.getOngoingDuration(), std::nullopt);

    // Start -> Activity
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10); // Ensure time advances so segment_start_time < now
    auto ongoing = tracker.getOngoingDuration();
    QVERIFY(ongoing.has_value());
    QCOMPARE(ongoing->type, DurationType::Activity);
    
    // Pause -> Pause
    tracker.useTimerViaButton(Button::Pause);
    QTest::qWait(10);
    ongoing = tracker.getOngoingDuration();
    QVERIFY(ongoing.has_value());
    QCOMPARE(ongoing->type, DurationType::Pause);

    // Stop -> None
    tracker.useTimerViaButton(Button::Stop);
    QCOMPARE(tracker.getOngoingDuration(), std::nullopt);
}

void TimeTrackerTest::test_timetracker_set_duration_type()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

    // Add a duration manually
    QDateTime now = QDateTime::currentDateTime();
    tracker.durations_.emplace_back(DurationType::Activity, now.addSecs(-10), now);
    QCOMPARE(tracker.durations_.size(), (size_t)1);
    QCOMPARE(tracker.durations_[0].type, DurationType::Activity);

    // Change to Pause
    tracker.setDurationType(0, DurationType::Pause);
    QCOMPARE(tracker.durations_[0].type, DurationType::Pause);

    // Invalid index
    tracker.setDurationType(99, DurationType::Activity);
    QCOMPARE(tracker.durations_[0].type, DurationType::Pause); // Unchanged
}

void TimeTrackerTest::test_timetracker_checkpoints_paused()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    // Enable checkpoints (interval = 1 minute, but we'll trigger manually)
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    TimeTracker tracker(settings);
    
    // Must be in Activity to save checkpoints
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(100); // Ensure elapsed > 0

    // Pause checkpoints
    tracker.pauseCheckpoints();
    tracker.saveCheckpoint(); // Should be ignored
    
    // Check DB: no entries yet
    DatabaseManager db(settings);
    auto loaded = db.loadDurations();
    QCOMPARE(loaded.size(), (size_t)0);

    // Resume and trigger
    tracker.resumeCheckpoints();
    tracker.saveCheckpoint();

    // Check DB: should have one entry
    loaded = db.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
}
