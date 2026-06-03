#include "test_timer.h"
#include "fakesessionstore.h"
#include <QtTest>

using TestCommon::createSettingsFile;
using TestCommon::sumDurations;

void TimerTest::initTestCase()
{
    db_path_ = QDir(QCoreApplication::applicationDirPath()).filePath("uTimer.sqlite");
    if (QFile::exists(db_path_)) {
        db_backup_path_ = db_path_ + ".bak_test";
        QFile::remove(db_backup_path_);
        QVERIFY(QFile::rename(db_path_, db_backup_path_));
    }
}

void TimerTest::cleanupTestCase()
{
    if (!db_path_.isEmpty()) {
        QFile::remove(db_path_);
    }
    if (!db_backup_path_.isEmpty()) {
        QFile::remove(db_path_);
        QVERIFY(QFile::rename(db_backup_path_, db_path_));
    }
}

void TimerTest::test_timer_start_pause_resume_stop_and_checkpoints()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    // Start -> Activity
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    QVERIFY(tracker.timer_.isValid());
    QCOMPARE(tracker.mode_, Timer::Mode::Activity);
    QVERIFY(!tracker.session_.id_tracker.current.isEmpty());

    // Pause -> durations captured and checkpoint id reset
    tracker.useTimerViaButton(Button::Pause);
    QCOMPARE(tracker.mode_, Timer::Mode::Pause);
    QVERIFY(tracker.session_.durations.size() >= 1);
    QVERIFY(!tracker.session_.id_tracker.current.isEmpty());

    // Resume -> Activity and timer restarted
    tracker.useTimerViaButton(Button::Start);
    QCOMPARE(tracker.mode_, Timer::Mode::Activity);
    tracker.timer_.invalidate();
    tracker.timer_.start();

    // Trigger checkpoint manually
    QTest::qWait(100); // Ensure elapsed > 0
    tracker.saveCheckpointInternal(QDateTime::currentDateTime());
    QVERIFY(!tracker.session_.id_tracker.current.isEmpty());

    // Stop -> move to None and durations flushed
    tracker.useTimerViaButton(Button::Stop);
    QCOMPARE(tracker.mode_, Timer::Mode::None);
    QVERIFY(tracker.session_.id_tracker.current.isEmpty());
}

void TimerTest::test_timer_backpause_resets_checkpoint_and_splits()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.setValue("uTimer/autopause_threshold_minutes", 1); // 60s
    writer.sync();

    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);
    tracker.useTimerViaButton(Button::Start);

    tracker.session_.segment_start_time = QDateTime::currentDateTime().addSecs(-120);
    tracker.timer_.invalidate();
    tracker.timer_.start();

    tracker.backpauseTimer(QDateTime::currentDateTime());
    QCOMPARE(tracker.mode_, Timer::Mode::Pause);
    QVERIFY(!tracker.session_.id_tracker.current.isEmpty());
    QVERIFY(tracker.session_.durations.size() >= 2);
    // Sum should equal ~120s
    qint64 total = sumDurations(tracker.session_.durations, DurationType::Activity) +
                   sumDurations(tracker.session_.durations, DurationType::Pause);
    QVERIFY(total >= 120000 - 2000); // allow minor tolerance
}


void TimerTest::test_timer_lock_events_checkpoint_and_resume()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);
    tracker.timer_.invalidate();
    tracker.timer_.start();
    tracker.useTimerViaLockEvent(LockEvent::Lock);
    QVERIFY(tracker.is_locked_);
    tracker.useTimerViaLockEvent(LockEvent::Unlock);
    QVERIFY(!tracker.is_locked_);
}

void TimerTest::test_timer_ongoing_duration()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

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

void TimerTest::test_timer_set_duration_type()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

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

void TimerTest::test_timer_checkpoints_paused()
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
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);
    
    // Must be in Activity to save checkpoints
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(100); // Ensure elapsed > 0

    // Pause checkpoints
    tracker.beginExclusiveEdit();
    tracker.saveCheckpoint(); // Should be ignored
    
    // Check DB: no unfinalized checkpoint entries yet
    QVERIFY(db.ensureOpen());
    QSqlQuery query(db.db);
    QVERIFY(query.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 0);
    db.lazyClose();

    // Resume and trigger
    tracker.endExclusiveEdit();
    tracker.saveCheckpoint();

    // Check DB: should now have one unfinalized checkpoint entry
    QVERIFY(db.ensureOpen());
    QVERIFY(query.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
    db.lazyClose();
}

void TimerTest::test_timer_retry_append_failure_then_success_preserves_segments()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);
    SqliteSessionStore db2(settings);
    QVERIFY(db2.saveDurations({}, TransactionMode::Append));

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    // Force stop save to fail and keep data in memory for retry.
    // Close the connection first so the next ensureOpen() re-tries the file open
    // and fails on the read-only permission.  (With a long-lived connection the
    // file descriptor is open with write permission; making the file read-only
    // afterwards doesn't revoke that permission until the connection is closed.)
    db.lazyClose();
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

void TimerTest::test_timer_retry_failure_keeps_unsaved_state_and_durations()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);
    SqliteSessionStore db2(settings);
    QVERIFY(db2.saveDurations({}, TransactionMode::Append));

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    // Force first stop save to fail.  Close the connection before making the
    // file read-only so that ensureOpen() re-tries and fails on the permission.
    db.lazyClose();
    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));
    tracker.useTimerViaButton(Button::Stop);

    QVERIFY(tracker.session_.has_unsaved_data);
    QVERIFY(!tracker.session_.durations.empty());
    const size_t sizeBeforeRetry = tracker.session_.durations.size();

    // Retry still fails -> must still be unsaved with data intact.
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

void TimerTest::test_session_state_begin_new_segment()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    QDateTime startTime = QDateTime::currentDateTime();
    QVERIFY(tracker.session_.id_tracker.current.isEmpty());
    QVERIFY(!tracker.session_.segment_start_time.isValid());

    // Act
    tracker.session_.beginNewSegment(startTime);

    // Assert
    QVERIFY(!tracker.session_.id_tracker.current.isEmpty());
    QCOMPARE(tracker.session_.segment_start_time, startTime);
}

void TimerTest::test_session_state_clear_segment()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);
    tracker.session_.beginNewSegment(QDateTime::currentDateTime());
    QVERIFY(!tracker.session_.id_tracker.current.isEmpty());

    // Act
    tracker.session_.clearSegment();

    // Assert
    QVERIFY(tracker.session_.id_tracker.current.isEmpty());
    QVERIFY(!tracker.session_.segment_start_time.isValid());
}

void TimerTest::test_session_state_mark_and_clear_unsaved()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);
    QVERIFY(!tracker.session_.has_unsaved_data);

    // Act: mark unsaved
    tracker.session_.markUnsaved();

    // Assert
    QVERIFY(tracker.session_.has_unsaved_data);

    // Act: clear unsaved
    tracker.session_.clearUnsaved();

    // Assert
    QVERIFY(!tracker.session_.has_unsaved_data);
    QVERIFY(tracker.session_.unsaved_durations.empty());
}

void TimerTest::test_session_state_reset_for_new_session()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);
    QDateTime now = QDateTime::currentDateTime();
    tracker.session_.durations.emplace_back(DurationType::Activity, now.addSecs(-10), now);
    tracker.session_.has_unsaved_data = true;
    tracker.session_.unsaved_durations = tracker.session_.durations;

    // Act
    tracker.session_.resetForNewSession();

    // Assert
    QVERIFY(tracker.session_.durations.empty());
    QVERIFY(!tracker.session_.has_unsaved_data);
    QVERIFY(tracker.session_.unsaved_durations.empty());
}

void TimerTest::test_replaceAll_success_clears_unsaved_cache()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    QDateTime now = QDateTime::currentDateTime();
    tracker.session_.durations.emplace_back(
        TimeDuration::create(DurationType::Activity, now.addSecs(-60), now).value());
    tracker.session_.unsaved_durations = tracker.session_.durations;
    tracker.session_.has_unsaved_data = true;

    const bool ok = tracker.replaceAll(Timeline({}, std::nullopt), Timeline({}, std::nullopt));

    QVERIFY(ok);
    QVERIFY(!tracker.session_.has_unsaved_data);
    QVERIFY(tracker.session_.unsaved_durations.empty());
}

void TimerTest::test_replaceAll_failure_keeps_unsaved_cache()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    fakeDb.replaceDurationsResult = false;
    Timer tracker(settings, fakeDb);

    QDateTime now = QDateTime::currentDateTime();
    tracker.session_.durations.emplace_back(
        TimeDuration::create(DurationType::Activity, now.addSecs(-60), now).value());
    tracker.session_.unsaved_durations = tracker.session_.durations;
    tracker.session_.has_unsaved_data = true;

    const bool ok = tracker.replaceAll(Timeline({}, std::nullopt), Timeline({}, std::nullopt));

    QVERIFY(!ok);
    QVERIFY(tracker.session_.has_unsaved_data);
    QVERIFY(!tracker.session_.unsaved_durations.empty());
}

void TimerTest::test_session_state_adopt_ongoing_segment()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);
    QDateTime start = QDateTime::currentDateTime().addSecs(-30);
    QDateTime end = QDateTime::currentDateTime();
    TimeDuration ongoing(DurationType::Activity, start, end);

    // Act
    tracker.session_.adoptOngoingSegment(ongoing);

    // Assert
    QCOMPARE(tracker.session_.id_tracker.current.toString(), ongoing.segment_id.toString());
    QCOMPARE(tracker.session_.segment_start_time, start);
}

void TimerTest::test_session_state_start_to_pause_transition()
{
    // Arrange: Start -> Activity, then pause
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    // Act: start
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: Activity mode, valid segment
    QCOMPARE(tracker.mode_, Timer::Mode::Activity);
    QVERIFY(!tracker.session_.id_tracker.current.isEmpty());
    QVERIFY(tracker.session_.segment_start_time.isValid());
    SegmentId activitySegId = tracker.session_.id_tracker.current;

    // Act: pause
    tracker.useTimerViaButton(Button::Pause);

    // Assert: Pause mode, new segment id (different from Activity)
    QCOMPARE(tracker.mode_, Timer::Mode::Pause);
    QVERIFY(!tracker.session_.id_tracker.current.isEmpty());
    QVERIFY(tracker.session_.id_tracker.current != activitySegId);
    QVERIFY(tracker.session_.segment_start_time.isValid());
    QVERIFY(tracker.session_.durations.size() >= 1);
    QCOMPARE(tracker.session_.durations.back().type, DurationType::Activity);
}

void TimerTest::test_session_state_pause_to_activity_transition()
{
    // Arrange: Start -> Pause -> Resume
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Pause);
    QTest::qWait(10);

    SegmentId pauseSegId = tracker.session_.id_tracker.current;
    size_t durationsBeforeResume = tracker.session_.durations.size();

    // Act: resume (Start from Pause)
    tracker.useTimerViaButton(Button::Start);

    // Assert: Activity mode, new segment id, pause duration recorded
    QCOMPARE(tracker.mode_, Timer::Mode::Activity);
    QVERIFY(!tracker.session_.id_tracker.current.isEmpty());
    QVERIFY(tracker.session_.id_tracker.current != pauseSegId);
    QVERIFY(tracker.session_.durations.size() > durationsBeforeResume);
}

void TimerTest::test_session_state_stop_clears_segment()
{
    // Arrange: Start -> Stop
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    QVERIFY(!tracker.session_.id_tracker.current.isEmpty());

    // Act
    tracker.useTimerViaButton(Button::Stop);

    // Assert: None mode, segment cleared, durations cleared (saved to DB)
    QCOMPARE(tracker.mode_, Timer::Mode::None);
    QVERIFY(tracker.session_.id_tracker.current.isEmpty());
    QVERIFY(!tracker.session_.segment_start_time.isValid());
    QVERIFY(!tracker.session_.has_unsaved_data);
}

// ============================================================================
// Boot time + hasEntriesForDate tri-state tests (T18)
// ============================================================================

void TimerTest::test_boot_time_not_added_when_history_disabled()
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
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

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

void TimerTest::test_boot_time_added_once_on_empty_db()
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
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    // Act: start the timer (first session today, empty DB)
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: boot time (30s = 30000ms) was added as an Activity segment
    qint64 totalActivity = sumDurations(tracker.session_.durations, DurationType::Activity);
    QVERIFY2(totalActivity >= 29000,
             "Boot time should be added when DB is empty for today");

    tracker.useTimerViaButton(Button::Stop);
}

void TimerTest::test_boot_time_not_added_when_db_has_entries_for_today()
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

    // Use a separate SqliteSessionStore to seed an entry for today
    {
        SqliteSessionStore seeder(settings);
        QDateTime now = QDateTime::currentDateTimeUtc();
        std::deque<TimeDuration> durations;
        durations.emplace_back(DurationType::Activity, now.addSecs(-3600), now.addSecs(-3540));
        QVERIFY(seeder.saveDurations(durations, TransactionMode::Append));
    }

    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

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

void TimerTest::test_pause_row_persisted_immediately_on_resume()
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
        SqliteSessionStore db(settings);
        Timer tracker(settings, db);

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
        tracker.mode_ = Timer::Mode::None;
        tracker.session_.id_tracker.clear();
    }

    // Assert: re-read the DB and verify the Pause row survived the crash.
    SqliteSessionStore db(settings);
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

void TimerTest::test_addduration_appends_single_same_day_segment()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

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

void TimerTest::test_addduration_discards_cross_midnight_segment()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    QDate yesterday = QDate::currentDate().addDays(-1);
    QDateTime start(yesterday, QTime(23, 59, 58, 0));
    QDateTime end = start.addSecs(5); // crosses midnight

    // Act
    tracker.addDuration(DurationType::Activity, start, end);

    // Assert: cross-midnight segment was silently discarded
    QVERIFY(tracker.session_.durations.empty());
}

void TimerTest::test_addduration_discards_zero_and_negative_duration()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);
    QDateTime now = QDateTime::currentDateTime();

    // Act: zero duration
    tracker.addDuration(DurationType::Activity, now, now);
    // Act: negative duration
    tracker.addDuration(DurationType::Activity, now, now.addSecs(-10));

    // Assert
    QVERIFY(tracker.session_.durations.empty());
}

void TimerTest::test_isOngoingSegmentCrossMidnight_returns_false_when_stopped()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    // mode_ == None by default
    QVERIFY(!tracker.isOngoingSegmentCrossMidnight());
}

void TimerTest::test_isOngoingSegmentCrossMidnight_returns_false_for_same_day()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.mode_ = Timer::Mode::Activity;
    tracker.session_.segment_start_time = QDateTime::currentDateTime().addSecs(-60);

    // Assert
    QVERIFY(!tracker.isOngoingSegmentCrossMidnight());
}

void TimerTest::test_isOngoingSegmentCrossMidnight_returns_true_when_segment_started_yesterday()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.mode_ = Timer::Mode::Activity;
    tracker.session_.segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));

    // Assert
    QVERIFY(tracker.isOngoingSegmentCrossMidnight());
}

void TimerTest::test_useTimerViaButton_force_stops_on_cross_midnight_ongoing()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.mode_ = Timer::Mode::Activity;
    tracker.session_.segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));
    tracker.timer_.start();

    // Act: try to stop — discardCrossMidnightOngoingAndStop should fire first
    tracker.useTimerViaButton(Button::Stop);

    // Assert: engine is in None, no cross-midnight row in DB
    QCOMPARE(tracker.mode_, Timer::Mode::None);
    QVERIFY(!tracker.session_.segment_start_time.isValid());
    QVERIFY(!tracker.was_active_before_autopause_);
    // FakeSessionStore captures writes; no duration should have been written
    // with a cross-midnight segment (the in-flight segment is discarded)
    for (const auto& d : fakeDb.storedDurations) {
        QVERIFY(d.startTime.date() == d.endTime.date());
    }
}

void TimerTest::test_useTimerViaButton_pause_event_is_dropped_when_cross_midnight()
{
    // Arrange: simulate cross-midnight state, user tries to pause
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.mode_ = Timer::Mode::Activity;
    tracker.session_.segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));
    tracker.timer_.start();

    // Act: user presses Pause — but the guard should intercept and force-stop
    tracker.useTimerViaButton(Button::Pause);

    // Assert: engine is in None (not Pause)
    QCOMPARE(tracker.mode_, Timer::Mode::None);
    QVERIFY(!tracker.session_.segment_start_time.isValid());
}

void TimerTest::test_useTimerViaLockEvent_unlock_does_not_restart_after_cross_midnight_discard()
{
    // Arrange: simulate cross-midnight with was_active_before_autopause_ set
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.mode_ = Timer::Mode::Pause;
    tracker.session_.segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));
    tracker.was_active_before_autopause_ = true;
    tracker.timer_.start();

    // Act: Unlock event arrives — guard should discard and not restart
    tracker.useTimerViaLockEvent(LockEvent::Unlock);

    // Assert: stayed in None, did not auto-restart
    QCOMPARE(tracker.mode_, Timer::Mode::None);
    QVERIFY(!tracker.was_active_before_autopause_);
}

void TimerTest::test_saveCheckpointInternal_does_not_write_cross_midnight_row()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.mode_ = Timer::Mode::Activity;
    tracker.session_.segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));
    tracker.timer_.start();

    // Act: checkpoint fires (simulating late checkpoint after sleep)
    QDateTime now = QDateTime::currentDateTime();
    tracker.saveCheckpointInternal(now);

    // Assert: engine stopped, no checkpoint written to DB
    QCOMPARE(tracker.mode_, Timer::Mode::None);
    QCOMPARE(fakeDb.savedCheckpoints.size(), static_cast<size_t>(0));
}

void TimerTest::test_stop_clears_was_active_before_autopause()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.was_active_before_autopause_ = true;

    // Act
    tracker.useTimerViaButton(Button::Stop);

    // Assert
    QVERIFY(!tracker.was_active_before_autopause_);
    QCOMPARE(tracker.mode_, Timer::Mode::None);
}

void TimerTest::test_startTimer_drops_cross_midnight_boot_time_entry()
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
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    // Act
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: cross-midnight boot entry was dropped; no completed durations
    // (the ongoing segment is fresh from startTimer)
    QCOMPARE(tracker.session_.durations.size(), static_cast<size_t>(0));
    QCOMPARE(tracker.mode_, Timer::Mode::Activity);

    tracker.useTimerViaButton(Button::Stop);
}

void TimerTest::test_getOngoingDuration_returns_nullopt_when_cross_midnight()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.mode_ = Timer::Mode::Activity;
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

void TimerTest::test_D_timetracker_public_surface_regression()
{
    // Test D: Construct a Timer and exercise every remaining public
    // method to confirm none were accidentally removed during Phase 1.
    // This is a regression guard — it would fail to compile if a method
    // disappeared from the header.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

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
    tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

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
    tracker.replaceAll(Timeline({}, std::nullopt), Timeline({}, std::nullopt));

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

void TimerTest::test_E_boot_time_gate_entries_yes_skips_boot_time()
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
    FakeSessionStore fakeDb;
    fakeDb.entriesForDateResult = EntriesForDateResult::Yes;
    Timer tracker(settings, fakeDb);

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

void TimerTest::test_E_boot_time_gate_entries_no_adds_boot_time()
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
    FakeSessionStore fakeDb;
    fakeDb.entriesForDateResult = EntriesForDateResult::No;
    Timer tracker(settings, fakeDb);

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

void TimerTest::test_E_boot_time_gate_entries_unknown_skips_boot_time()
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
    FakeSessionStore fakeDb;
    fakeDb.entriesForDateResult = EntriesForDateResult::Unknown;
    Timer tracker(settings, fakeDb);

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

// ============================================================================
// Phase 4 test gate — Test X
// ============================================================================

/**
 * X: Stop persists state via commitSession only.
 *
 * After a start→stop cycle, the FakeSessionStore's callLog must contain
 * "commitSession" for the stop write, and must NOT contain "saveDurations"
 * or "updateDurationsById" as write paths (those are no longer in the
 * interface; any occurrence would indicate a regression to the old paths).
 */
void TimerTest::test_X_stop_persists_via_commitSession_only()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    // Act
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    fakeDb.callLog.clear(); // record only the stop-path writes
    tracker.useTimerViaButton(Button::Stop);

    // Assert: commitSession was called
    QVERIFY2(fakeDb.callLog.contains("commitSession"),
             "Stop must call commitSession to persist the session");

    // Assert: old direct-write methods were NOT called via the interface
    QVERIFY2(!fakeDb.callLog.contains("saveDurations"),
             "saveDurations must not be called; commitSession is the write path");
    QVERIFY2(!fakeDb.callLog.contains("updateDurationsById"),
             "updateDurationsById must not be called; commitSession is the write path");
}

// Issue 3 Layer A: while dialog is open, LongOngoingLock must not call backpauseTimer.
// After endExclusiveEdit(), the deferred event must be replayed.
void TimerTest::test_dialog_open_blocks_backpause()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.setValue("uTimer/autopause_threshold_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    const size_t dursBefore = tracker.session_.durations.size();

    // Open dialog — suspends mutation
    tracker.beginExclusiveEdit();
    QVERIFY(tracker.dialog_open_);

    // Fire LongOngoingLock while dialog is open
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock);

    // backpause must NOT have been applied
    QCOMPARE(tracker.session_.durations.size(), dursBefore);
    QVERIFY(tracker.mode_ == Timer::Mode::Activity);

    // Pending event recorded
    QCOMPARE(tracker.pending_lock_event_, LockEvent::LongOngoingLock);

    // Close dialog — replay deferred LongOngoingLock
    tracker.endExclusiveEdit();
    QVERIFY(!tracker.dialog_open_);
    QCOMPARE(tracker.pending_lock_event_, LockEvent::None);

    // After replay, timer should be in Pause (backpause applied)
    QCOMPARE(tracker.mode_, Timer::Mode::Pause);
}

// Issue 3 Layer A: pending_midnight_stop_ set directly, endExclusiveEdit replays it.
void TimerTest::test_dialog_open_defers_midnight_stop()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Simulate: dialog is open and midnight fired (deferred)
    tracker.beginExclusiveEdit();
    tracker.pending_midnight_stop_ = true;

    // Close dialog — replay deferred midnight stop
    tracker.endExclusiveEdit();

    QVERIFY(!tracker.pending_midnight_stop_);
    QCOMPARE(tracker.mode_, Timer::Mode::None);
}

// Issue 3 Layer A: Lock bookkeeping (is_locked_) must still run even when dialog is open,
// but saveCheckpointInternal must not be called.
void TimerTest::test_dialog_open_allows_lock_bookkeeping()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    tracker.beginExclusiveEdit();
    QVERIFY(!tracker.is_locked_);

    const size_t checksBefore = fakeDb.callLog.count("saveCheckpoint");

    // Fire Lock while dialog is open
    tracker.useTimerViaLockEvent(LockEvent::Lock);

    // is_locked_ must be set (bookkeeping)
    QVERIFY(tracker.is_locked_);

    // saveCheckpoint (i.e. saveCheckpointInternal path via DB) must not have fired
    QCOMPARE(fakeDb.callLog.count("saveCheckpoint"), checksBefore);

    tracker.endExclusiveEdit();
}

// ============================================================================
// Step 4: T1+T9 — Pause-merge removal tests
// ============================================================================

void TimerTest::test_T1_unpause_creates_new_pause_segment_with_fresh_id()
{
    // Verify that resuming from Pause (Pause→Activity) creates a new Pause
    // segment whose segment_id is different from the preceding Activity
    // segment_id. The old bug reused the Activity segment_id for the Pause row,
    // corrupting the DB. Each segment must have its own unique id.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    const SegmentId activitySegId = tracker.session_.id_tracker.current;

    tracker.useTimerViaButton(Button::Pause);
    QTest::qWait(10);
    const SegmentId pauseSegId = tracker.session_.id_tracker.current;
    const size_t dursBefore = tracker.session_.durations.size();

    // Act: resume from Pause
    tracker.useTimerViaButton(Button::Start);

    // Assert: a new Pause segment was appended (not merged into the Activity)
    QVERIFY(tracker.session_.durations.size() > dursBefore);
    // The Pause segment just recorded must have its own id — neither empty,
    // nor the same as the preceding Activity's id.
    const TimeDuration& pauseEntry = tracker.session_.durations.back();
    QCOMPARE(pauseEntry.type, DurationType::Pause);
    QVERIFY2(!pauseEntry.segment_id.isEmpty(),
             "Pause segment must have a non-empty segment_id");
    QVERIFY2(pauseEntry.segment_id != activitySegId,
             "Pause segment must not reuse the Activity segment_id (T1 regression)");
    // The new Activity's segment_id must also be fresh
    QVERIFY2(tracker.session_.id_tracker.current != pauseSegId,
             "New Activity segment must have a different id from the Pause segment");

    tracker.useTimerViaButton(Button::Stop);
}

void TimerTest::test_T9_new_activity_segment_after_unpause_has_activity_type()
{
    // Verify that after resuming from Pause the engine is in Activity mode
    // and the new ongoing segment type is Activity (not Pause or anything else).
    // The old Pause-merge branch could cause type confusion.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Pause);
    QTest::qWait(10);

    // Act
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: engine is in Activity mode
    QCOMPARE(tracker.mode_, Timer::Mode::Activity);

    // The ongoing duration must be Activity type
    auto ongoing = tracker.getOngoingDuration();
    QVERIFY(ongoing.has_value());
    QCOMPARE(ongoing->type, DurationType::Activity);

    // All completed Pause entries in durations must have type Pause,
    // and all Activity entries must have type Activity — no type corruption.
    for (const auto& d : tracker.session_.durations) {
        QVERIFY(d.type == DurationType::Activity || d.type == DurationType::Pause);
        // Segment ids must be non-empty (T1 invariant)
        QVERIFY2(!d.segment_id.isEmpty(), "Every completed segment must have a non-empty segment_id");
    }

    tracker.useTimerViaButton(Button::Stop);
}

// ============================================================================
// Step 4: T10 — autopause flag cleared in startTimer
// ============================================================================

void TimerTest::test_T10_was_active_cleared_on_start_from_none()
{
    // Verify that startTimer from None always clears was_active_before_autopause_,
    // even when that flag was set before the call (e.g. by a previous lock event).

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    // Pre-set the flag to simulate a stale autopause state
    tracker.was_active_before_autopause_ = true;

    // Act: start from None
    tracker.useTimerViaButton(Button::Start);

    // Assert: flag is cleared regardless of prior value
    QVERIFY2(!tracker.was_active_before_autopause_,
             "was_active_before_autopause_ must be false after startTimer from None");

    tracker.useTimerViaButton(Button::Stop);
}

void TimerTest::test_T10_was_active_cleared_on_start_from_pause()
{
    // Verify that startTimer from Pause always clears was_active_before_autopause_.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Pause);

    // Simulate stale autopause flag (as if a LongOngoingLock set it before the pause)
    tracker.was_active_before_autopause_ = true;

    // Act: resume from Pause
    tracker.useTimerViaButton(Button::Start);

    // Assert: flag is cleared
    QVERIFY2(!tracker.was_active_before_autopause_,
             "was_active_before_autopause_ must be false after startTimer from Pause");

    tracker.useTimerViaButton(Button::Stop);
}

// ============================================================================
// Step 4: T8 — destructor ordering smoke test
// ============================================================================

void TimerTest::test_T8_destructor_does_not_crash_while_active()
{
    // Verify that destroying a Timer while it is actively running (and its
    // checkpointTimer_ could in theory fire) does not crash or deadlock.
    //
    // The ordering fix (T8) stops and disconnects checkpointTimer_ before
    // acquiring the mutex in ~Timer(). Without this fix, a queued slot
    // firing during destruction could deadlock on the recursive mutex or
    // access already-freed members.
    //
    // Direct unit-testing of destructor ordering is not feasible (we cannot
    // reliably force a slot to fire exactly during destruction without a
    // race condition in the test itself). This test instead serves as an
    // absence-of-crash guard: if the fix is missing and a slot fires in the
    // wrong window, this test may crash or deadlock during CI. The FakeSessionStore
    // is used so no real DB I/O is needed.

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;

    {
        Timer tracker(settings, fakeDb);
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(10);
        // tracker destroyed here while in Activity mode with checkpointTimer_ running
    }

    // If we reach here without a crash or deadlock, T8 is correct.
    QVERIFY(true);
}

// ============================================================================
// Step 5: C6 + T2 — beginExclusiveEdit / endExclusiveEdit + replaceCurrentDurations guard
// ============================================================================

// T2: replaceCurrentDurations must NOT call saveCheckpoint while dialog_open_ is true.
void TimerTest::test_T2_replaceCurrentDurations_skips_checkpoint_while_dialog_open()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Build a plausible ongoing segment for replaceCurrentDurations to adopt.
    const QDateTime start = QDateTime::currentDateTime().addSecs(-60);
    const QDateTime end   = QDateTime::currentDateTime();
    TimeDuration ongoing = TimeDuration::fromTrusted(DurationType::Activity, start, end, SegmentId::fromString("seg-001"));

    tracker.beginExclusiveEdit();
    QVERIFY(tracker.dialog_open_);

    fakeDb.callLog.clear();

    // replaceCurrentDurations with a valid ongoing segment — checkpoint must be suppressed.
    tracker.replaceCurrentDurations({}, ongoing);

    QCOMPARE(fakeDb.callLog.count("saveCheckpoint"), 0);

    tracker.endExclusiveEdit();
}

// C6: after endExclusiveEdit(), a subsequent replaceCurrentDurations resumes normal writes.
void TimerTest::test_C6_replaceCurrentDurations_writes_checkpoint_after_endExclusiveEdit()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    Timer tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    const QDateTime start = QDateTime::currentDateTime().addSecs(-60);
    const QDateTime end   = QDateTime::currentDateTime();
    TimeDuration ongoing = TimeDuration::fromTrusted(DurationType::Activity, start, end, SegmentId::fromString("seg-002"));

    tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();
    QVERIFY(!tracker.dialog_open_);

    fakeDb.callLog.clear();

    // After endExclusiveEdit, dialog_open_ is false — checkpoint write must proceed.
    tracker.replaceCurrentDurations({}, ongoing);

    QCOMPARE(fakeDb.callLog.count("saveCheckpoint"), 1);
}

void TimerTest::test_S12_marker_error_skips_reconciliation()
{
    // When consumeLastCleanShutdownMarker() returns Status::Error (DB failure),
    // Timer must NOT reconcile orphan checkpoints — the DB state is unknown and
    // finalising orphans could conflict with in-progress data.

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Seed an orphan checkpoint that would normally be reconciled.
    OrphanCheckpoint orphan;
    orphan.id = 1;
    orphan.segment_id = SegmentId::mint();
    orphan.type = DurationType::Activity;
    orphan.startTime = QDateTime::currentDateTime().addSecs(-120);
    orphan.endTime   = QDateTime::currentDateTime().addSecs(-60);
    orphan.duration  = orphan.startTime.msecsTo(orphan.endTime);
    fakeDb.orphanCheckpoints.push_back(orphan);

    // Simulate a marker read failure.
    fakeDb.cleanShutdownMarker = MarkerResult { {}, MarkerResult::Status::Error };

    Timer tracker(settings, fakeDb);

    // No seconds recovered and no reconciliation attempted.
    QCOMPARE(tracker.getStartupRecoveredSeconds(), (qint64)0);
    QVERIFY(!fakeDb.callLog.contains("reconcileUnfinalizedCheckpoints"));
}
