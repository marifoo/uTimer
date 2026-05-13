#include "test_timetracker.h"
#include "fakedatabasemanager.h"
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
    DatabaseManager db(settings);
    TimeTracker tracker(settings, db);

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
    tracker.saveCheckpointInternal(QDateTime::currentDateTime());
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
    DatabaseManager db(settings);
    TimeTracker tracker(settings, db);
    tracker.useTimerViaButton(Button::Start);

    tracker.session_.segment_start_time = QDateTime::currentDateTime().addSecs(-120);
    tracker.timer_.invalidate();
    tracker.timer_.start();

    tracker.backpauseTimer(QDateTime::currentDateTime());
    QCOMPARE(tracker.mode_, TimeTracker::Mode::Pause);
    QVERIFY(!tracker.session_.current_checkpoint_segment_id.isEmpty());
    QVERIFY(tracker.session_.durations.size() >= 2);
    // Sum should equal ~120s
    qint64 total = sumDurations(tracker.session_.durations, DurationType::Activity) +
                   sumDurations(tracker.session_.durations, DurationType::Pause);
    QVERIFY(total >= 120000 - 2000); // allow minor tolerance
}


void TimeTrackerTest::test_timetracker_lock_events_checkpoint_and_resume()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    DatabaseManager db(settings);
    TimeTracker tracker(settings, db);

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
    DatabaseManager db(settings);
    TimeTracker tracker(settings, db);

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
    DatabaseManager db(settings);
    TimeTracker tracker(settings, db);

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
    DatabaseManager db(settings);
    TimeTracker tracker(settings, db);
    
    // Must be in Activity to save checkpoints
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(100); // Ensure elapsed > 0

    // Pause checkpoints
    tracker.pauseCheckpoints();
    tracker.saveCheckpoint(); // Should be ignored
    
    // Check DB: no unfinalized checkpoint entries yet
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
    DatabaseManager db(settings);
    TimeTracker tracker(settings, db);
    DatabaseManager db2(settings);
    QVERIFY(db2.saveDurations({}, TransactionMode::Append));

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

    auto loaded = db2.loadDurations();

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
    DatabaseManager db(settings);
    TimeTracker tracker(settings, db);
    DatabaseManager db2(settings);
    QVERIFY(db2.saveDurations({}, TransactionMode::Append));

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
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);

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
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);
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
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);
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
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);
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
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);
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
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);

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
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);
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
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);
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

// ============================================================================
// Boot time + hasEntriesForDate tri-state tests (T18)
// ============================================================================

void TimeTrackerTest::test_boot_time_not_added_when_history_disabled()
{
    // When history is disabled (history_days_to_keep = 0), hasEntriesForDate
    // returns Unknown. startTimer must NOT add boot time in this case,
    // because we can't confirm whether entries already exist for today.
    // Adding boot time unconditionally would cause double-counting on every
    // subsequent start of the day.

    // Arrange
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 0);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/boot_time_seconds", 30);
    writer.sync();

    Settings settings(settingsPath);
    DatabaseManager db(settings);
    TimeTracker tracker(settings, db);

    // Act: start timer twice (simulating a second start on the same day)
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Stop);
    QTest::qWait(10);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: no boot time was added on either start, because history is
    // disabled and hasEntriesForDate returns Unknown.
    // The only durations should be from actual timer running, not boot time.
    qint64 totalActivity = sumDurations(tracker.session_.durations, DurationType::Activity);
    // Boot time would be 30000ms. If it were added, total would be >= 30000.
    // Since we only ran for ~10ms, it should be much less.
    QVERIFY2(totalActivity < 5000,
             "Boot time should not be added when history is disabled (DB returns Unknown)");

    tracker.useTimerViaButton(Button::Stop);
}

void TimeTrackerTest::test_boot_time_added_once_on_empty_db()
{
    // When history is enabled and the DB is empty for today, boot time
    // should be added on the first startTimer of the day.

    // Arrange
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/boot_time_seconds", 30);
    writer.sync();

    Settings settings(settingsPath);
    DatabaseManager db(settings);
    TimeTracker tracker(settings, db);

    // Act: start the timer (first session today, empty DB)
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: boot time (30s = 30000ms) was added as an Activity segment
    qint64 totalActivity = sumDurations(tracker.session_.durations, DurationType::Activity);
    QVERIFY2(totalActivity >= 29000,
             "Boot time should be added when DB is empty for today");

    tracker.useTimerViaButton(Button::Stop);
}

void TimeTrackerTest::test_boot_time_not_added_when_db_has_entries_for_today()
{
    // When history is enabled and the DB already has entries for today,
    // boot time must NOT be added (to avoid double-counting).

    // Arrange: seed the DB with an entry for today
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/boot_time_seconds", 30);
    writer.sync();

    Settings settings(settingsPath);

    // Use a separate DatabaseManager to seed an entry for today
    {
        DatabaseManager seeder(settings);
        QDateTime now = QDateTime::currentDateTimeUtc();
        std::deque<TimeDuration> durations;
        durations.emplace_back(DurationType::Activity, now.addSecs(-3600), now.addSecs(-3540));
        QVERIFY(seeder.saveDurations(durations, TransactionMode::Append));
    }

    DatabaseManager db(settings);
    TimeTracker tracker(settings, db);

    // Act: start the timer (DB already has entries for today)
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: no boot time added. Only the in-progress segment should exist.
    qint64 totalActivity = sumDurations(tracker.session_.durations, DurationType::Activity);
    QVERIFY2(totalActivity < 5000,
             "Boot time should not be added when DB already has entries for today");

    tracker.useTimerViaButton(Button::Stop);
}

// ============================================================================
// Pause persistence crash safety (T19)
// ============================================================================

void TimeTrackerTest::test_pause_row_persisted_immediately_on_resume()
{
    // Verify that when the user resumes from Pause (startTimer from Pause),
    // the completed Pause duration is immediately persisted to the DB as a
    // finalized row. A simulated crash (skipping stopTimer) should not lose
    // the Pause entry — the reloaded history must show the Pause row.

    // Arrange
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);

    {
        DatabaseManager db(settings);
        TimeTracker tracker(settings, db);

        // Start -> Activity
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(20);

        // Pause -> records Activity segment, transitions to Pause
        tracker.useTimerViaButton(Button::Pause);
        QTest::qWait(50);

        // Resume -> completes Pause segment and persists it immediately (T19)
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(20);

        // Simulate crash: destroy tracker without calling stopTimer.
        // Set mode to None to prevent destructor from saving a clean stop.
        tracker.mode_ = TimeTracker::Mode::None;
        tracker.session_.current_checkpoint_segment_id.clear();
    }

    // Assert: re-read the DB and verify the Pause row survived the crash.
    DatabaseManager db(settings);
    auto loaded = db.loadDurations();

    // We expect at least one Activity row (from the finalizeActivityToPause
    // call in pauseTimer) and one Pause row (from startTimer's immediate
    // persist). The second Activity segment may or may not be present
    // depending on checkpoint timing, but the Pause must be there.
    bool hasPause = false;
    bool hasActivity = false;
    for (const auto& d : loaded.durations) {
        if (d.type == DurationType::Pause) {
            hasPause = true;
        }
        if (d.type == DurationType::Activity) {
            hasActivity = true;
        }
    }
    QVERIFY2(hasPause, "Pause row must survive a crash after resume (T19)");
    QVERIFY2(hasActivity, "Activity row must be present in history after pause+resume");
}

// ============================================================================
// Midnight forced-stop and cross-midnight discard tests
// ============================================================================

void TimeTrackerTest::test_addduration_appends_single_same_day_segment()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);

    QDateTime start = QDateTime::currentDateTime().addSecs(-60);
    QDateTime end = QDateTime::currentDateTime();

    // Act
    tracker.addDuration(DurationType::Activity, start, end);

    // Assert
    QCOMPARE(tracker.session_.durations.size(), static_cast<size_t>(1));
    QCOMPARE(tracker.session_.durations[0].type, DurationType::Activity);
    QCOMPARE(tracker.session_.durations[0].startTime, start);
    QCOMPARE(tracker.session_.durations[0].endTime, end);
}

void TimeTrackerTest::test_addduration_discards_cross_midnight_segment()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);

    QDate yesterday = QDate::currentDate().addDays(-1);
    QDateTime start(yesterday, QTime(23, 59, 58, 0));
    QDateTime end = start.addSecs(5); // crosses midnight

    // Act
    tracker.addDuration(DurationType::Activity, start, end);

    // Assert: cross-midnight segment was silently discarded
    QVERIFY(tracker.session_.durations.empty());
}

void TimeTrackerTest::test_addduration_discards_zero_and_negative_duration()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);
    QDateTime now = QDateTime::currentDateTime();

    // Act: zero duration
    tracker.addDuration(DurationType::Activity, now, now);
    // Act: negative duration
    tracker.addDuration(DurationType::Activity, now, now.addSecs(-10));

    // Assert
    QVERIFY(tracker.session_.durations.empty());
}

void TimeTrackerTest::test_isOngoingSegmentCrossMidnight_returns_false_when_stopped()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // mode_ == None by default
    QVERIFY(!tracker.isOngoingSegmentCrossMidnight());
}

void TimeTrackerTest::test_isOngoingSegmentCrossMidnight_returns_false_for_same_day()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.mode_ = TimeTracker::Mode::Activity;
    tracker.session_.segment_start_time = QDateTime::currentDateTime().addSecs(-60);

    // Assert
    QVERIFY(!tracker.isOngoingSegmentCrossMidnight());
}

void TimeTrackerTest::test_isOngoingSegmentCrossMidnight_returns_true_when_segment_started_yesterday()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.mode_ = TimeTracker::Mode::Activity;
    tracker.session_.segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));

    // Assert
    QVERIFY(tracker.isOngoingSegmentCrossMidnight());
}

void TimeTrackerTest::test_useTimerViaButton_force_stops_on_cross_midnight_ongoing()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.mode_ = TimeTracker::Mode::Activity;
    tracker.session_.segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));
    tracker.timer_.start();

    // Act: try to stop — discardCrossMidnightOngoingAndStop should fire first
    tracker.useTimerViaButton(Button::Stop);

    // Assert: engine is in None, no cross-midnight row in DB
    QCOMPARE(tracker.mode_, TimeTracker::Mode::None);
    QVERIFY(!tracker.session_.segment_start_time.isValid());
    QVERIFY(!tracker.was_active_before_autopause_);
    // FakeDatabaseManager captures writes; no duration should have been written
    // with a cross-midnight segment (the in-flight segment is discarded)
    for (const auto& d : fakeDb.storedDurations) {
        QVERIFY(d.startTime.date() == d.endTime.date());
    }
}

void TimeTrackerTest::test_useTimerViaButton_pause_event_is_dropped_when_cross_midnight()
{
    // Arrange: simulate cross-midnight state, user tries to pause
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.mode_ = TimeTracker::Mode::Activity;
    tracker.session_.segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));
    tracker.timer_.start();

    // Act: user presses Pause — but the guard should intercept and force-stop
    tracker.useTimerViaButton(Button::Pause);

    // Assert: engine is in None (not Pause)
    QCOMPARE(tracker.mode_, TimeTracker::Mode::None);
    QVERIFY(!tracker.session_.segment_start_time.isValid());
}

void TimeTrackerTest::test_useTimerViaLockEvent_unlock_does_not_restart_after_cross_midnight_discard()
{
    // Arrange: simulate cross-midnight with was_active_before_autopause_ set
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.sync();

    Settings settings(settingsPath);
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.mode_ = TimeTracker::Mode::Pause;
    tracker.session_.segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));
    tracker.was_active_before_autopause_ = true;
    tracker.timer_.start();

    // Act: Unlock event arrives — guard should discard and not restart
    tracker.useTimerViaLockEvent(LockEvent::Unlock);

    // Assert: stayed in None, did not auto-restart
    QCOMPARE(tracker.mode_, TimeTracker::Mode::None);
    QVERIFY(!tracker.was_active_before_autopause_);
}

void TimeTrackerTest::test_saveCheckpointInternal_does_not_write_cross_midnight_row()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.mode_ = TimeTracker::Mode::Activity;
    tracker.session_.segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));
    tracker.timer_.start();

    // Act: checkpoint fires (simulating late checkpoint after sleep)
    QDateTime now = QDateTime::currentDateTime();
    tracker.saveCheckpointInternal(now);

    // Assert: engine stopped, no checkpoint written to DB
    QCOMPARE(tracker.mode_, TimeTracker::Mode::None);
    QCOMPARE(fakeDb.savedCheckpoints.size(), static_cast<size_t>(0));
}

void TimeTrackerTest::test_stop_clears_was_active_before_autopause()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.was_active_before_autopause_ = true;

    // Act
    tracker.useTimerViaButton(Button::Stop);

    // Assert
    QVERIFY(!tracker.was_active_before_autopause_);
    QCOMPARE(tracker.mode_, TimeTracker::Mode::None);
}

void TimeTrackerTest::test_startTimer_drops_cross_midnight_boot_time_entry()
{
    // A boot_time_sec large enough that bootStart falls on the previous day.
    // addDuration should silently discard it and the user still enters Activity.

    // Arrange
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    // Use a large boot time that is guaranteed to reach back across midnight
    writer.setValue("uTimer/boot_time_seconds", 90000); // 25 hours
    writer.sync();

    Settings settings(settingsPath);
    DatabaseManager db(settings);
    TimeTracker tracker(settings, db);

    // Act
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: cross-midnight boot entry was dropped; no completed durations
    // (the ongoing segment is fresh from startTimer)
    QCOMPARE(tracker.session_.durations.size(), static_cast<size_t>(0));
    QCOMPARE(tracker.mode_, TimeTracker::Mode::Activity);

    tracker.useTimerViaButton(Button::Stop);
}

void TimeTrackerTest::test_getOngoingDuration_returns_nullopt_when_cross_midnight()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.mode_ = TimeTracker::Mode::Activity;
    tracker.session_.segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));

    // Act
    auto result = tracker.getOngoingDuration();

    // Assert: cross-midnight transient is hidden from observers
    QVERIFY(!result.has_value());
}

// ============================================================================
// Phase 1 test gate (T1)
// ============================================================================

void TimeTrackerTest::test_D_timetracker_public_surface_regression()
{
    // Test D: Construct a TimeTracker and exercise every remaining public
    // method to confirm none were accidentally removed during Phase 1.
    // This is a regression guard — it would fail to compile if a method
    // disappeared from the header.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);

    QDateTime now = QDateTime::currentDateTime();

    // Exercise every public method (slots + regular)
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Accessors
    qint64 active = tracker.getActiveTime();
    QVERIFY(active >= 0);

    qint64 pause = tracker.getPauseTime();
    QVERIFY(pause >= 0);

    const auto& durations = tracker.getCurrentDurations();
    (void)durations;

    auto ongoing = tracker.getOngoingDuration();
    QVERIFY(ongoing.has_value()); // timer is running

    bool crossMidnight = tracker.isOngoingSegmentCrossMidnight();
    QVERIFY(!crossMidnight); // started now, so not cross-midnight

    qint64 recovered = tracker.getStartupRecoveredSeconds();
    QCOMPARE(recovered, static_cast<qint64>(0)); // fresh db, nothing to recover

    bool showNotification = tracker.shouldShowStartupRecoveryNotification();
    (void)showNotification;

    auto histStats = tracker.getLastHistoryLoadStats();
    (void)histStats;

    bool canMark = tracker.canMarkCleanShutdown();
    QVERIFY(!canMark); // timer is running

    // Mutators
    tracker.pauseCheckpoints();
    tracker.resumeCheckpoints();

    tracker.useTimerViaButton(Button::Pause);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // setDurationType: index 0 should exist after Activity+Pause cycle
    if (!tracker.session_.durations.empty()) {
        tracker.setDurationType(0, DurationType::Pause);
        tracker.setDurationType(0, DurationType::Activity);
    }

    tracker.useTimerViaButton(Button::Stop);

    // DB write helpers
    fakeDb.callLog.clear();
    tracker.appendDurationsToDB();
    tracker.updateDurationsInDB();
    tracker.replaceDurationsInDB({}, {});

    // replaceCurrentDurations
    tracker.replaceCurrentDurations({}, std::nullopt);

    // getDurationsHistory
    auto history = tracker.getDurationsHistory();
    (void)history;

    // Confirm the engine predicate is still there
    QVERIFY(tracker.canMarkCleanShutdown()); // stopped, no unsaved data

    // lock event path
    tracker.useTimerViaLockEvent(LockEvent::Lock);
    tracker.useTimerViaLockEvent(LockEvent::Unlock);
}

void TimeTrackerTest::test_E_boot_time_gate_entries_yes_skips_boot_time()
{
    // Test E (Yes branch): DB says entries exist for today → boot time skipped.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/boot_time_seconds", 30);
    writer.sync();

    Settings settings(settingsPath);
    FakeDatabaseManager fakeDb;
    fakeDb.entriesForDateResult = EntriesForDateResult::Yes;
    TimeTracker tracker(settings, fakeDb);

    // Act
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: no boot-time segment was added (session_.durations is empty
    // at start because there are no completed segments yet)
    qint64 totalActivity = 0;
    for (const auto& d : tracker.session_.durations)
        if (d.type == DurationType::Activity)
            totalActivity += d.duration;
    QVERIFY2(totalActivity < 5000,
             "Boot time must not be added when DB reports Yes for today");

    tracker.useTimerViaButton(Button::Stop);
}

void TimeTrackerTest::test_E_boot_time_gate_entries_no_adds_boot_time()
{
    // Test E (No branch): DB says no entries for today → boot time added.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/boot_time_seconds", 30);
    writer.sync();

    Settings settings(settingsPath);
    FakeDatabaseManager fakeDb;
    fakeDb.entriesForDateResult = EntriesForDateResult::No;
    TimeTracker tracker(settings, fakeDb);

    // Act
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: a 30-second boot-time Activity segment was prepended
    qint64 totalActivity = 0;
    for (const auto& d : tracker.session_.durations)
        if (d.type == DurationType::Activity)
            totalActivity += d.duration;
    QVERIFY2(totalActivity >= 29000,
             "Boot time must be added when DB reports No entries for today");

    tracker.useTimerViaButton(Button::Stop);
}

void TimeTrackerTest::test_E_boot_time_gate_entries_unknown_skips_boot_time()
{
    // Test E (Unknown branch): DB result is Unknown (e.g., history disabled)
    // → boot time skipped to avoid double-counting.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/boot_time_seconds", 30);
    writer.sync();

    Settings settings(settingsPath);
    FakeDatabaseManager fakeDb;
    fakeDb.entriesForDateResult = EntriesForDateResult::Unknown;
    TimeTracker tracker(settings, fakeDb);

    // Act
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: no boot-time segment added
    qint64 totalActivity = 0;
    for (const auto& d : tracker.session_.durations)
        if (d.type == DurationType::Activity)
            totalActivity += d.duration;
    QVERIFY2(totalActivity < 5000,
             "Boot time must not be added when DB reports Unknown for today");

    tracker.useTimerViaButton(Button::Stop);
}
