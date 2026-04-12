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
    QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());

    // Pause -> durations captured and checkpoint id reset
    tracker.useTimerViaButton(Button::Pause);
    QCOMPARE(tracker.mode_, TimeTracker::Mode::Pause);
    QVERIFY(tracker.session_.durations.size() >= 1);
    QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());

    // Resume -> Activity and timer restarted
    tracker.useTimerViaButton(Button::Start);
    QCOMPARE(tracker.mode_, TimeTracker::Mode::Activity);
    tracker.timer_.invalidate();
    tracker.timer_.start();

    // Trigger checkpoint manually
    QTest::qWait(100); // Ensure elapsed > 0
    tracker.saveCheckpointInternal();
    QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());

    // Stop -> move to None and durations flushed
    tracker.useTimerViaButton(Button::Stop);
    QCOMPARE(tracker.mode_, TimeTracker::Mode::None);
    QVERIFY(tracker.session_.current_checkpoint_segment_id.isEmpty());
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

    tracker.session_.segment_start_time = QDateTime::currentDateTime().addSecs(-120);
    tracker.timer_.invalidate();
    tracker.timer_.start();

    tracker.backpauseTimer();
    QCOMPARE(tracker.mode_, TimeTracker::Mode::Pause);
    QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
    QVERIFY(tracker.session_.durations.size() >= 2);
    // Sum should equal ~120s
    qint64 total = sumDurations(tracker.session_.durations, DurationType::Activity) +
                   sumDurations(tracker.session_.durations, DurationType::Pause);
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
    tracker.session_.segment_start_time = QDateTime(QDate::currentDate().addDays(-1), QTime(23,59,58,0));
    tracker.addDurationWithMidnightSplit(DurationType::Activity,
                                         tracker.session_.segment_start_time,
                                         tracker.session_.segment_start_time.addSecs(5));
    
    // Post-midnight part remains in memory
    QVERIFY(tracker.session_.durations.size() >= 1);
    
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
    tracker.session_.durations.emplace_back(DurationType::Activity, now.addSecs(-10), now);
    QCOMPARE(tracker.session_.durations.size(), (size_t)1);
    QCOMPARE(tracker.session_.durations[0].type, DurationType::Activity);

    // Change to Pause
    tracker.setDurationType(0, DurationType::Pause);
    QCOMPARE(tracker.session_.durations[0].type, DurationType::Pause);

    // Invalid index
    tracker.setDurationType(99, DurationType::Activity);
    QCOMPARE(tracker.session_.durations[0].type, DurationType::Pause); // Unchanged
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
    
    // Check DB: no unfinalized checkpoint entries yet
    DatabaseManager db(settings);
    QVERIFY(db.lazyOpen());
    QSqlQuery query(db.db);
    QVERIFY(query.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 0);
    db.lazyClose();

    // Resume and trigger
    tracker.resumeCheckpoints();
    tracker.saveCheckpoint();

    // Check DB: should now have one unfinalized checkpoint entry
    QVERIFY(db.lazyOpen());
    QVERIFY(query.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
    db.lazyClose();
}

void TimeTrackerTest::test_timetracker_retry_append_failure_then_success_preserves_segments()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    TimeTracker tracker(settings);
    DatabaseManager db(settings);
    QVERIFY(db.saveDurations({}, TransactionMode::Append));

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    // Force stop save to fail and keep data in memory for retry.
    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));
    tracker.useTimerViaButton(Button::Stop);

    QVERIFY(tracker.session_.has_unsaved_data);
    QVERIFY(!tracker.session_.durations.empty());
    std::deque<TimeDuration> unsavedCopy = tracker.session_.durations;

    // Retry while DB is still blocked -> must stay unsaved.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.session_.has_unsaved_data);
    QVERIFY(!tracker.session_.durations.empty());

    // Restore DB and retry again -> previously unsaved rows should be appended.
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));
    tracker.useTimerViaButton(Button::Stop);
    tracker.useTimerViaButton(Button::Start);

    auto loaded = db.loadDurations();

    for (const auto& d : unsavedCopy) {
        bool found = false;
        for (const auto& row : loaded) {
            if (row.type == d.type && row.startTime == d.startTime && row.endTime == d.endTime && row.duration == d.duration) {
                found = true;
                break;
            }
        }
        QVERIFY(found);
    }
}

void TimeTrackerTest::test_timetracker_retry_failure_keeps_unsaved_state_and_durations()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    TimeTracker tracker(settings);
    DatabaseManager db(settings);
    QVERIFY(db.saveDurations({}, TransactionMode::Append));

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    // Force first stop save to fail.
    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));
    tracker.useTimerViaButton(Button::Stop);

    QVERIFY(tracker.session_.has_unsaved_data);
    QVERIFY(!tracker.session_.durations.empty());
    const size_t sizeBeforeRetry = tracker.session_.durations.size();

    // Retry still fails.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.session_.has_unsaved_data);
    QVERIFY(!tracker.session_.durations.empty());
    QVERIFY(tracker.session_.durations.size() >= sizeBeforeRetry);

    // Cleanup permissions for following tests.
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));
}

// ============================================================================
// SessionState transition tests
// ============================================================================

void TimeTrackerTest::test_session_state_begin_new_segment()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

    QDateTime startTime = QDateTime::currentDateTime();
    QVERIFY(tracker.session_.current_checkpoint_segment_id.isEmpty());
    QVERIFY(!tracker.session_.segment_start_time.isValid());

    // Act
    tracker.session_.beginNewSegment(startTime, settings);

    // Assert
    QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
    QCOMPARE(tracker.session_.segment_start_time, startTime);
}

void TimeTrackerTest::test_session_state_clear_segment()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);
    tracker.session_.beginNewSegment(QDateTime::currentDateTime(), settings);
    QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());

    // Act
    tracker.session_.clearSegment(settings);

    // Assert
    QVERIFY(tracker.session_.current_checkpoint_segment_id.isEmpty());
    QVERIFY(!tracker.session_.segment_start_time.isValid());
}

void TimeTrackerTest::test_session_state_mark_and_clear_unsaved()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);
    QVERIFY(!tracker.session_.has_unsaved_data);

    // Act: mark unsaved
    tracker.session_.markUnsaved(settings);

    // Assert
    QVERIFY(tracker.session_.has_unsaved_data);

    // Act: clear unsaved
    tracker.session_.clearUnsaved(settings);

    // Assert
    QVERIFY(!tracker.session_.has_unsaved_data);
    QVERIFY(tracker.session_.unsaved_durations.empty());
}

void TimeTrackerTest::test_session_state_reset_for_new_session()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);
    QDateTime now = QDateTime::currentDateTime();
    tracker.session_.durations.emplace_back(DurationType::Activity, now.addSecs(-10), now);
    tracker.session_.has_unsaved_data = true;
    tracker.session_.unsaved_durations = tracker.session_.durations;

    // Act
    tracker.session_.resetForNewSession(settings);

    // Assert
    QVERIFY(tracker.session_.durations.empty());
    QVERIFY(!tracker.session_.has_unsaved_data);
    QVERIFY(tracker.session_.unsaved_durations.empty());
}

void TimeTrackerTest::test_session_state_adopt_ongoing_segment()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);
    QDateTime start = QDateTime::currentDateTime().addSecs(-30);
    QDateTime end = QDateTime::currentDateTime();
    TimeDuration ongoing(DurationType::Activity, start, end);

    // Act
    tracker.session_.adoptOngoingSegment(ongoing, settings);

    // Assert
    QCOMPARE(tracker.session_.current_checkpoint_segment_id, ongoing.segment_id);
    QCOMPARE(tracker.session_.segment_start_time, start);
}

void TimeTrackerTest::test_session_state_start_to_pause_transition()
{
    // Arrange: Start -> Activity, then pause
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

    // Act: start
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: Activity mode, valid segment
    QCOMPARE(tracker.mode_, TimeTracker::Mode::Activity);
    QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
    QVERIFY(tracker.session_.segment_start_time.isValid());
    QString activitySegId = tracker.session_.current_checkpoint_segment_id;

    // Act: pause
    tracker.useTimerViaButton(Button::Pause);

    // Assert: Pause mode, new segment id (different from Activity)
    QCOMPARE(tracker.mode_, TimeTracker::Mode::Pause);
    QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
    QVERIFY(tracker.session_.current_checkpoint_segment_id != activitySegId);
    QVERIFY(tracker.session_.segment_start_time.isValid());
    QVERIFY(tracker.session_.durations.size() >= 1);
    QCOMPARE(tracker.session_.durations.back().type, DurationType::Activity);
}

void TimeTrackerTest::test_session_state_pause_to_activity_transition()
{
    // Arrange: Start -> Pause -> Resume
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Pause);
    QTest::qWait(10);

    QString pauseSegId = tracker.session_.current_checkpoint_segment_id;
    size_t durationsBeforeResume = tracker.session_.durations.size();

    // Act: resume (Start from Pause)
    tracker.useTimerViaButton(Button::Start);

    // Assert: Activity mode, new segment id, pause duration recorded
    QCOMPARE(tracker.mode_, TimeTracker::Mode::Activity);
    QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
    QVERIFY(tracker.session_.current_checkpoint_segment_id != pauseSegId);
    QVERIFY(tracker.session_.durations.size() > durationsBeforeResume);
}

void TimeTrackerTest::test_session_state_stop_clears_segment()
{
    // Arrange: Start -> Stop
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());

    // Act
    tracker.useTimerViaButton(Button::Stop);

    // Assert: None mode, segment cleared, durations cleared (saved to DB)
    QCOMPARE(tracker.mode_, TimeTracker::Mode::None);
    QVERIFY(tracker.session_.current_checkpoint_segment_id.isEmpty());
    QVERIFY(!tracker.session_.segment_start_time.isValid());
    QVERIFY(!tracker.session_.has_unsaved_data);
}

void TimeTrackerTest::test_compute_midnight_split_no_crossing()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

    QDateTime start = QDateTime::currentDateTime().addSecs(-60);
    QDateTime end = QDateTime::currentDateTime();

    // Act
    MidnightSplitResult result = tracker.computeMidnightSplit(DurationType::Activity, start, end);

    // Assert: no midnight crossing, single entry
    QVERIFY(!result.crossed_midnight);
    QCOMPARE(result.new_entries.size(), static_cast<size_t>(1));
    QVERIFY(result.previous_day_entries.empty());
    QVERIFY(!result.updated_segment_start_time.has_value());
    QCOMPARE(result.new_entries[0].type, DurationType::Activity);
    QCOMPARE(result.new_entries[0].startTime, start);
    QCOMPARE(result.new_entries[0].endTime, end);
}

void TimeTrackerTest::test_compute_midnight_split_crossing()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

    QDate yesterday = QDate::currentDate().addDays(-1);
    QDateTime start(yesterday, QTime(23, 59, 58, 0));
    QDateTime end = start.addSecs(5); // crosses midnight

    // Act
    MidnightSplitResult result = tracker.computeMidnightSplit(DurationType::Activity, start, end);

    // Assert: midnight crossing detected
    QVERIFY(result.crossed_midnight);
    QCOMPARE(result.previous_day_entries.size(), static_cast<size_t>(1));
    QCOMPARE(result.new_entries.size(), static_cast<size_t>(1));
    QVERIFY(result.updated_segment_start_time.has_value());

    // Pre-midnight entry ends at 23:59:59.999
    QCOMPARE(result.previous_day_entries[0].endTime.time(), QTime(23, 59, 59, 999));
    QCOMPARE(result.previous_day_entries[0].startTime, start);
    QCOMPARE(result.previous_day_entries[0].type, DurationType::Activity);

    // Post-midnight entry starts at 00:00:00.000
    QCOMPARE(result.new_entries[0].startTime.time(), QTime(0, 0, 0, 0));
    QCOMPARE(result.new_entries[0].type, DurationType::Activity);
    QCOMPARE(result.updated_segment_start_time.value().time(), QTime(0, 0, 0, 0));
}

void TimeTrackerTest::test_compute_midnight_split_zero_duration()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

    QDateTime now = QDateTime::currentDateTime();

    // Act: zero duration
    MidnightSplitResult result = tracker.computeMidnightSplit(DurationType::Activity, now, now);

    // Assert: no entries produced
    QVERIFY(!result.crossed_midnight);
    QVERIFY(result.new_entries.empty());
    QVERIFY(result.previous_day_entries.empty());
}
