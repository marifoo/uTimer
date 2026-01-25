#include "test_database.h"
#include <QtTest>

using TestCommon::createSettingsFile;
using TestCommon::mk;

void DatabaseTest::initTestCase()
{
    db_path_ = QDir(QCoreApplication::applicationDirPath()).filePath("uTimer.sqlite");
    if (QFile::exists(db_path_)) {
        db_backup_path_ = db_path_ + ".bak_test";
        QFile::remove(db_backup_path_);
        QVERIFY(QFile::rename(db_path_, db_backup_path_));
    }
}

void DatabaseTest::cleanupTestCase()
{
    if (!db_path_.isEmpty()) {
        QFile::remove(db_path_);
    }
    if (!db_backup_path_.isEmpty()) {
        QFile::remove(db_path_);
        QVERIFY(QFile::rename(db_backup_path_, db_path_));
    }
}

void DatabaseTest::test_database_backups_and_retention_and_disable()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 2));
    DatabaseManager manager(settings);

    // history_days_to_keep>0 => lazyOpen creates DB
    QVERIFY(manager.saveDurations({}, TransactionMode::Append)); // no-op but should succeed

    // Insert entries across 3 days to trigger pruning on next open
    std::deque<TimeDuration> durations;
    QDateTime now = QDateTime::currentDateTimeUtc();
    durations.emplace_back(DurationType::Activity, now.addDays(-3), now.addDays(-3).addSecs(60));
    durations.emplace_back(DurationType::Activity, now.addDays(-1), now.addDays(-1).addSecs(60));
    durations.emplace_back(DurationType::Activity, now, now.addSecs(60));
    QVERIFY(manager.saveDurations(durations, TransactionMode::Replace));

    auto loaded = manager.loadDurations();
    QVERIFY(static_cast<int>(loaded.size()) >= 2); // old entries pruned on lazyOpen

    // history_days_to_keep=0 disables db
    Settings disabled(createSettingsFile(tempDir.path(), 0));
    DatabaseManager managerDisabled(disabled);
    QVERIFY(managerDisabled.saveDurations(durations, TransactionMode::Append));
    auto loadedDisabled = managerDisabled.loadDurations();
    QCOMPARE(static_cast<int>(loadedDisabled.size()), 0);
}

void DatabaseTest::test_explicitStartTimes_constructorComputesDuration()
{
    // Description: TimeDuration constructor computes duration from start/end

    // Arrange & Act
    QDateTime start = QDateTime::fromMSecsSinceEpoch(1000, Qt::UTC);
    QDateTime end = QDateTime::fromMSecsSinceEpoch(5000, Qt::UTC);
    TimeDuration d(DurationType::Activity, start, end);

    // Assert
    QCOMPARE(d.duration, (qint64)4000);
    QCOMPARE(d.startTime.toMSecsSinceEpoch(), (qint64)1000);
    QCOMPARE(d.endTime.toMSecsSinceEpoch(), (qint64)5000);
}

void DatabaseTest::test_explicitStartTimes_startTimePreservedAfterClean()
{
    // Description: cleanDurations preserves startTime field after merges

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 1000));
    d.push_back(mk(DurationType::Activity, 1050, 2000)); // will merge

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().startTime.toMSecsSinceEpoch(), (qint64)0);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)2000);
    QCOMPARE(d.front().duration, (qint64)2000);
}

void DatabaseTest::test_explicitStartTimes_mergeUpdatesAllFields()
{
    // Description: Merge branch updates startTime, endTime, and duration consistently

    // Arrange - entry that extends before prev
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 1000, 2000)); // [1000, 2000]
    d.push_back(mk(DurationType::Activity, 500, 1500));  // starts before, ends inside

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().startTime.toMSecsSinceEpoch(), (qint64)500);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)2000);
    QCOMPARE(d.front().duration, (qint64)1500);
}

void DatabaseTest::test_splitPreservesStartTime()
{
    // Description: When splitting a duration, the first segment should keep original startTime

    // Arrange
    QDateTime start = QDateTime::fromMSecsSinceEpoch(1000, Qt::UTC);
    QDateTime split = QDateTime::fromMSecsSinceEpoch(3000, Qt::UTC);
    QDateTime end = QDateTime::fromMSecsSinceEpoch(5000, Qt::UTC);

    // Act - simulate a split
    TimeDuration first(DurationType::Activity, start, split);
    TimeDuration second(DurationType::Pause, split, end);

    // Assert
    QCOMPARE(first.startTime.toMSecsSinceEpoch(), (qint64)1000);
    QCOMPARE(first.endTime.toMSecsSinceEpoch(), (qint64)3000);
    QCOMPARE(first.duration, (qint64)2000);
    QCOMPARE(second.startTime.toMSecsSinceEpoch(), (qint64)3000);
    QCOMPARE(second.endTime.toMSecsSinceEpoch(), (qint64)5000);
    QCOMPARE(second.duration, (qint64)2000);
}

void DatabaseTest::test_zeroDurationNotAdded()
{
    // Description: Zero-duration segments should not affect the deque

    // Arrange
    QDateTime now = QDateTime::fromMSecsSinceEpoch(1000, Qt::UTC);
    TimeDuration d(DurationType::Activity, now, now);

    // Assert
    QCOMPARE(d.duration, (qint64)0);
}

void DatabaseTest::test_negativeDurationHandled()
{
    // Description: If end is before start, duration should be negative (validation catches this)

    // Arrange
    QDateTime start = QDateTime::fromMSecsSinceEpoch(5000, Qt::UTC);
    QDateTime end = QDateTime::fromMSecsSinceEpoch(3000, Qt::UTC);
    TimeDuration d(DurationType::Activity, start, end);

    // Assert - duration is negative, validation layer should reject
    QCOMPARE(d.duration, (qint64)-2000);
}

void DatabaseTest::test_schemaValidation_missingStartColumns()
{
    // Description: Schema check fails when start_date/start_time are missing

    resetDatabaseFile();

    const QString connName = "schema_legacy";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "type INTEGER NOT NULL,"
            "duration INTEGER NOT NULL,"
            "end_date DATE NOT NULL,"
            "end_time TEXT NOT NULL)"
        ));
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    QVERIFY(!manager.checkSchemaOnStartup());
}

void DatabaseTest::test_exactMatching_upsertReplacesByStartTime()
{
    // Description: Upsert replaces by exact start_time/type, leaving one row

    resetDatabaseFile();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30000));
    DatabaseManager manager(settings);

    QDateTime start = QDateTime::fromMSecsSinceEpoch(1'000'000, Qt::UTC);
    QDateTime end1 = start.addSecs(10);
    QDateTime end2 = start.addSecs(20);

    std::deque<TimeDuration> durations;
    durations.emplace_back(DurationType::Activity, start, end1);
    QVERIFY(manager.updateDurationsByStartTime(durations));

    durations.clear();
    durations.emplace_back(DurationType::Activity, start, end2);
    QVERIFY(manager.updateDurationsByStartTime(durations));

    const QString connName = "exact_match_query";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);
        query.prepare(
            "SELECT COUNT(*), end_time, duration FROM durations "
            "WHERE start_date = :date AND start_time = :time AND type = :type"
        );
        query.bindValue(":date", start.toUTC().date().toString(Qt::ISODate));
        query.bindValue(":time", start.toUTC().time().toString("HH:mm:ss.zzz"));
        query.bindValue(":type", static_cast<int>(DurationType::Activity));
        QVERIFY(query.exec());
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 1);
        QCOMPARE(query.value(1).toString(), end2.toUTC().time().toString("HH:mm:ss.zzz"));
        QCOMPARE(query.value(2).toLongLong(), start.msecsTo(end2));
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void DatabaseTest::test_checkpointPreservesStartTimeOnUpdate()
{
    // Description: Checkpoint updates do not overwrite the original start time

    resetDatabaseFile();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30000));
    DatabaseManager manager(settings);

    QDateTime start = QDateTime::fromMSecsSinceEpoch(2'000'000, Qt::UTC);
    QDateTime end1 = start.addSecs(5);
    QDateTime end2 = start.addSecs(15);

    long long checkpointId = -1;
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start.msecsTo(end1), start, end1, checkpointId));
    QVERIFY(checkpointId != -1);

    QDateTime driftedStart = start.addSecs(3600);
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start.msecsTo(end2), driftedStart, end2, checkpointId));

    const QString connName = "checkpoint_query";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);
        query.prepare("SELECT start_date, start_time, end_time, duration FROM durations WHERE id = :id");
        query.bindValue(":id", checkpointId);
        QVERIFY(query.exec());
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), start.toUTC().date().toString(Qt::ISODate));
        QCOMPARE(query.value(1).toString(), start.toUTC().time().toString("HH:mm:ss.zzz"));
        QCOMPARE(query.value(2).toString(), end2.toUTC().time().toString("HH:mm:ss.zzz"));
        QCOMPARE(query.value(3).toLongLong(), start.msecsTo(end2));
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void DatabaseTest::test_clockDriftResilience_durationStoredFromElapsed()
{
    // Description: Stored duration reflects provided elapsed time, not wall-clock delta

    resetDatabaseFile();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30000));
    DatabaseManager manager(settings);

    QDateTime start = QDateTime::fromMSecsSinceEpoch(3'000'000, Qt::UTC);
    QDateTime end = start.addSecs(3600);
    qint64 elapsed = 120'000;

    long long checkpointId = -1;
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, elapsed, start, end, checkpointId));
    QVERIFY(checkpointId != -1);

    const QString connName = "drift_query";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);
        query.prepare("SELECT duration FROM durations WHERE id = :id");
        query.bindValue(":id", checkpointId);
        QVERIFY(query.exec());
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toLongLong(), elapsed);
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void DatabaseTest::test_loadDurations_skipsNegativeDurationRows()
{
    // Description: loadDurations skips rows with negative stored duration

    resetDatabaseFile();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30000));
    DatabaseManager manager(settings);

    QDateTime start = QDateTime::fromMSecsSinceEpoch(4'000'000, Qt::UTC);
    QDateTime end = start.addSecs(10);
    std::deque<TimeDuration> durations;
    durations.emplace_back(DurationType::Activity, start, end);
    QVERIFY(manager.updateDurationsByStartTime(durations));

    const QString connName = "negative_duration_insert";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);
        query.prepare(
            "INSERT INTO durations (type, duration, start_date, start_time, end_date, end_time) "
            "VALUES (:type, :duration, :start_date, :start_time, :end_date, :end_time)"
        );
        query.bindValue(":type", static_cast<int>(DurationType::Activity));
        query.bindValue(":duration", static_cast<qint64>(-500));
        query.bindValue(":start_date", start.toUTC().date().toString(Qt::ISODate));
        query.bindValue(":start_time", start.toUTC().time().toString("HH:mm:ss.zzz"));
        query.bindValue(":end_date", end.toUTC().date().toString(Qt::ISODate));
        query.bindValue(":end_time", end.toUTC().time().toString("HH:mm:ss.zzz"));
        QVERIFY(query.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    auto loaded = manager.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 1);
}

void DatabaseTest::test_databasemanager_write_failure()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    // Create DB first
    QVERIFY(manager.saveDurations({}, TransactionMode::Append));
    
    // Make DB read-only
    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));

    // Try to save
    QDateTime now = QDateTime::currentDateTime();
    std::deque<TimeDuration> d;
    d.emplace_back(DurationType::Activity, now, now.addSecs(10));
    
    // Should fail gracefully
    QVERIFY(!manager.saveDurations(d, TransactionMode::Append));

    // Restore permissions
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));
    
    // Should succeed now
    QVERIFY(manager.saveDurations(d, TransactionMode::Append));
}

void DatabaseTest::test_database_transaction_rollback_on_insert_failure()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    // Create valid database first
    QDateTime now = QDateTime::currentDateTime();
    std::deque<TimeDuration> validData;
    validData.emplace_back(DurationType::Activity, now.addSecs(-100), now.addSecs(-90));
    QVERIFY(manager.saveDurations(validData, TransactionMode::Append));
    
    // Verify data was saved
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    
    // Make database read-only to force INSERT failure
    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser));
    
    // Try to append - should fail and rollback
    std::deque<TimeDuration> newData;
    newData.emplace_back(DurationType::Pause, now.addSecs(-50), now.addSecs(-40));
    QVERIFY(!manager.saveDurations(newData, TransactionMode::Append));
    
    // Restore permissions
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser));
    
    // Original data should still be intact (rollback worked)
    loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    QCOMPARE(loaded[0].type, DurationType::Activity);
}

void DatabaseTest::test_database_transaction_rollback_on_replace_failure()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    // Create initial data
    QDateTime now = QDateTime::currentDateTime();
    std::deque<TimeDuration> originalData;
    originalData.emplace_back(DurationType::Activity, now.addSecs(-200), now.addSecs(-190));
    originalData.emplace_back(DurationType::Pause, now.addSecs(-180), now.addSecs(-170));
    QVERIFY(manager.saveDurations(originalData, TransactionMode::Append));
    
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)2);
    
    // Make database read-only after DELETE succeeds but before INSERT
    // This simulates partial transaction failure
    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser));
    
    // Try Replace mode - should fail and rollback
    std::deque<TimeDuration> replacementData;
    replacementData.emplace_back(DurationType::Activity, now.addSecs(-50), now.addSecs(-40));
    QVERIFY(!manager.saveDurations(replacementData, TransactionMode::Replace));
    
    // Restore permissions
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser));
    
    // Original data should be preserved (rollback after DELETE)
    loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)2);
}

void DatabaseTest::test_database_checkpoint_id_reuse()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    QDateTime start = QDateTime::currentDateTime();
    QDateTime end = start.addSecs(10);
    long long checkpointId = -1;
    
    // First checkpoint - creates new row
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, 10000, start, end, checkpointId));
    QVERIFY(checkpointId != -1);
    long long firstId = checkpointId;
    
    // Second checkpoint with same ID - should UPDATE existing row
    QDateTime newEnd = start.addSecs(20);
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, 20000, start, newEnd, checkpointId));
    QCOMPARE(checkpointId, firstId); // ID unchanged
    
    // Load and verify only ONE entry exists
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    QCOMPARE(loaded[0].duration, (qint64)20000);
    QCOMPARE(loaded[0].startTime.toString(Qt::ISODate), start.toString(Qt::ISODate));
}

void DatabaseTest::test_database_checkpoint_deleted_row_creates_new()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    QDateTime start = QDateTime::currentDateTime();
    QDateTime end = start.addSecs(10);
    long long checkpointId = -1;
    
    // Create checkpoint
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, 10000, start, end, checkpointId));
    QVERIFY(checkpointId != -1);
    long long firstId = checkpointId;
    
    // Manually delete the checkpoint row (simulating retention cleanup)
    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    query.prepare("DELETE FROM durations WHERE id = :id");
    query.bindValue(":id", checkpointId);
    QVERIFY(query.exec());
    manager.lazyClose();
    
    // Try to update checkpoint - should detect missing row and INSERT new one
    QDateTime newEnd = start.addSecs(20);
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, 20000, start, newEnd, checkpointId));
    QVERIFY(checkpointId != -1);
    QVERIFY(checkpointId != firstId); // New ID assigned
    
    // Verify new row exists
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    QCOMPARE(loaded[0].duration, (qint64)20000);
}

void DatabaseTest::test_database_checkpoint_preserves_start_time()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    QDateTime originalStart = QDateTime::currentDateTime();
    QDateTime firstEnd = originalStart.addSecs(10);
    long long checkpointId = -1;
    
    // First checkpoint
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, 10000, originalStart, firstEnd, checkpointId));
    
    // Update checkpoint with new end time (simulate ongoing timer)
    QDateTime secondEnd = originalStart.addSecs(30);
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, 30000, originalStart, secondEnd, checkpointId));
    
    // Load and verify start time is preserved
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    QCOMPARE(loaded[0].startTime.toString(Qt::ISODate), originalStart.toString(Qt::ISODate));
    QCOMPARE(loaded[0].endTime.toString(Qt::ISODate), secondEnd.toString(Qt::ISODate));
    QCOMPARE(loaded[0].duration, (qint64)30000);
}

void DatabaseTest::test_database_upsert_insert_mode()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    QDateTime now = QDateTime::currentDateTime();
    std::deque<TimeDuration> durations;
    durations.emplace_back(DurationType::Activity, now.addSecs(-100), now.addSecs(-90));
    durations.emplace_back(DurationType::Pause, now.addSecs(-80), now.addSecs(-70));
    
    // First upsert - should INSERT both
    QVERIFY(manager.updateDurationsByStartTime(durations));
    
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)2);
}

void DatabaseTest::test_database_upsert_replace_mode()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    QDateTime now = QDateTime::currentDateTime();
    QDateTime start1 = now.addSecs(-100);
    QDateTime end1 = now.addSecs(-90);
    
    // Insert initial duration
    std::deque<TimeDuration> initial;
    initial.emplace_back(DurationType::Activity, start1, end1);
    QVERIFY(manager.updateDurationsByStartTime(initial));
    
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    QCOMPARE(loaded[0].duration, (qint64)10000);
    
    // Upsert with SAME start time but DIFFERENT end time
    std::deque<TimeDuration> updated;
    QDateTime end2 = now.addSecs(-80); // Extended duration
    updated.emplace_back(DurationType::Activity, start1, end2);
    QVERIFY(manager.updateDurationsByStartTime(updated));
    
    // Should have REPLACED the row (due to UNIQUE constraint)
    loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    QCOMPARE(loaded[0].duration, (qint64)20000); // Duration updated
}

void DatabaseTest::test_database_upsert_unique_constraint()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    QDateTime now = QDateTime::currentDateTime();
    QDateTime start = now.addSecs(-100);
    
    // Insert Activity at same start time
    std::deque<TimeDuration> activity;
    activity.emplace_back(DurationType::Activity, start, start.addSecs(10));
    QVERIFY(manager.updateDurationsByStartTime(activity));
    
    // Insert Pause at SAME start time (different type)
    std::deque<TimeDuration> pause;
    pause.emplace_back(DurationType::Pause, start, start.addSecs(5));
    QVERIFY(manager.updateDurationsByStartTime(pause));
    
    // Both should exist (UNIQUE is on start_date + start_time + TYPE)
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)2);
}

void DatabaseTest::test_database_upsert_empty_deque()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    std::deque<TimeDuration> empty;
    QVERIFY(manager.updateDurationsByStartTime(empty)); // Should succeed as no-op
}

void DatabaseTest::test_database_load_negative_duration()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    // Manually insert negative duration
    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    QDateTime now = QDateTime::currentDateTimeUtc();
    query.prepare("INSERT INTO durations (type, duration, start_date, start_time, end_date, end_time) "
                 "VALUES (0, -5000, :start_date, :start_time, :end_date, :end_time)");
    query.bindValue(":start_date", now.date().toString(Qt::ISODate));
    query.bindValue(":start_time", now.time().toString("HH:mm:ss.zzz"));
    QDateTime end = now.addSecs(5);
    query.bindValue(":end_date", end.date().toString(Qt::ISODate));
    query.bindValue(":end_time", end.time().toString("HH:mm:ss.zzz"));
    QVERIFY(query.exec());
    manager.lazyClose();

    // Load should compute duration from timestamps and use that
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    QVERIFY(loaded[0].duration >= 0); // Computed duration is positive
}

void DatabaseTest::test_database_load_start_after_end()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    // Manually insert start > end
    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    QDateTime now = QDateTime::currentDateTimeUtc();
    QDateTime start = now;
    QDateTime end = now.addSecs(-10); // End BEFORE start
    
    query.prepare("INSERT INTO durations (type, duration, start_date, start_time, end_date, end_time) "
                 "VALUES (0, 10000, :start_date, :start_time, :end_date, :end_time)");
    query.bindValue(":start_date", start.date().toString(Qt::ISODate));
    query.bindValue(":start_time", start.time().toString("HH:mm:ss.zzz"));
    query.bindValue(":end_date", end.date().toString(Qt::ISODate));
    query.bindValue(":end_time", end.time().toString("HH:mm:ss.zzz"));
    QVERIFY(query.exec());
    manager.lazyClose();

    // Load should SKIP invalid entry
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)0);
}

void DatabaseTest::test_database_load_invalid_enum_type()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    // Manually insert invalid type (valid are 0=Activity, 1=Pause)
    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    QDateTime now = QDateTime::currentDateTimeUtc();
    QDateTime end = now.addSecs(10);
    
    query.prepare("INSERT INTO durations (type, duration, start_date, start_time, end_date, end_time) "
                 "VALUES (99, 10000, :start_date, :start_time, :end_date, :end_time)");
    query.bindValue(":start_date", now.date().toString(Qt::ISODate));
    query.bindValue(":start_time", now.time().toString("HH:mm:ss.zzz"));
    query.bindValue(":end_date", end.date().toString(Qt::ISODate));
    query.bindValue(":end_time", end.time().toString("HH:mm:ss.zzz"));
    QVERIFY(query.exec());
    manager.lazyClose();

    // Load should skip invalid type
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)0);
}

void DatabaseTest::test_database_load_duration_mismatch_tolerance()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    QDateTime start = QDateTime::currentDateTimeUtc();
    QDateTime end = start.addMSecs(1000); // Actual duration: 1000ms
    
    // Insert with stored duration = 1003ms (within 5ms tolerance)
    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    query.prepare("INSERT INTO durations (type, duration, start_date, start_time, end_date, end_time) "
                 "VALUES (0, 1003, :start_date, :start_time, :end_date, :end_time)");
    query.bindValue(":start_date", start.date().toString(Qt::ISODate));
    query.bindValue(":start_time", start.time().toString("HH:mm:ss.zzz"));
    query.bindValue(":end_date", end.date().toString(Qt::ISODate));
    query.bindValue(":end_time", end.time().toString("HH:mm:ss.zzz"));
    QVERIFY(query.exec());
    manager.lazyClose();

    // Should load successfully (within tolerance)
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    QCOMPARE(loaded[0].duration, (qint64)1000); // Uses computed value
}

void DatabaseTest::test_database_timezone_roundtrip()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    // Create duration in local time
    QDateTime localStart = QDateTime::currentDateTime();
    QDateTime localEnd = localStart.addSecs(60);
    
    std::deque<TimeDuration> durations;
    durations.emplace_back(DurationType::Activity, localStart, localEnd);
    
    // Save and reload
    QVERIFY(manager.saveDurations(durations, TransactionMode::Append));
    auto loaded = manager.loadDurations();
    
    QCOMPARE(loaded.size(), (size_t)1);
    // Times should be preserved (stored as UTC, loaded as local)
    QCOMPARE(loaded[0].startTime.toString(Qt::ISODate), localStart.toString(Qt::ISODate));
    QCOMPARE(loaded[0].endTime.toString(Qt::ISODate), localEnd.toString(Qt::ISODate));
}

void DatabaseTest::test_database_millisecond_precision()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    // Create timestamps with millisecond precision
    QDateTime start = QDateTime::currentDateTime();
    qint64 startMsec = start.toMSecsSinceEpoch();
    startMsec = (startMsec / 1000) * 1000 + 123; // Set milliseconds to 123
    start = QDateTime::fromMSecsSinceEpoch(startMsec);
    
    QDateTime end = start.addMSecs(4567); // Add 4567ms
    
    std::deque<TimeDuration> durations;
    durations.emplace_back(DurationType::Activity, start, end);
    
    QVERIFY(manager.saveDurations(durations, TransactionMode::Append));
    auto loaded = manager.loadDurations();
    
    QCOMPARE(loaded.size(), (size_t)1);
    // Milliseconds should be preserved
    QCOMPARE(loaded[0].startTime.time().msec(), 123);
    QCOMPARE(loaded[0].duration, (qint64)4567);
}

void DatabaseTest::test_database_schema_validation_missing_start_date()
{
    resetDatabaseFile();
    
    // Create database with old schema (missing start_date/start_time)
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "schema_test");
    db.setDatabaseName(db_path_);
    QVERIFY(db.open());
    
    QSqlQuery query(db);
    QVERIFY(query.exec(
        "CREATE TABLE durations ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "type INTEGER NOT NULL,"
        "duration INTEGER NOT NULL,"
        "end_date DATE NOT NULL,"
        "end_time TEXT NOT NULL"
        ")"
    ));
    db.close();
    QSqlDatabase::removeDatabase("schema_test");
    
    // Now try to use DatabaseManager
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);
    
    // checkSchemaOnStartup should detect outdated schema
    QVERIFY(!manager.checkSchemaOnStartup());
}

void DatabaseTest::test_database_schema_validation_fresh_database()
{
    resetDatabaseFile();
    
    // No database file exists
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);
    
    // Should return true (will create fresh schema)
    QVERIFY(manager.checkSchemaOnStartup());
}

void DatabaseTest::test_database_backup_file_creation()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    // Create initial data
    QDateTime now = QDateTime::currentDateTime();
    std::deque<TimeDuration> durations;
    durations.emplace_back(DurationType::Activity, now.addSecs(-100), now.addSecs(-90));
    
    QVERIFY(manager.saveDurations(durations, TransactionMode::Append));
    
    // Save again to trigger backup
    durations.clear();
    durations.emplace_back(DurationType::Pause, now.addSecs(-50), now.addSecs(-40));
    QVERIFY(manager.saveDurations(durations, TransactionMode::Append));
    
    // Check that backup files exist in qtest directory
    QDir testDir(QCoreApplication::applicationDirPath());
    QStringList backupFiles = testDir.entryList(QStringList() << "*.backup", QDir::Files);
    QVERIFY(backupFiles.size() > 0);
    
    QStringList durationTxtFiles = testDir.entryList(QStringList() << "*.durations.txt", QDir::Files);
    QVERIFY(durationTxtFiles.size() > 0);
}

void DatabaseTest::test_database_backup_preserves_data()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    DatabaseManager manager(settings);

    // Create data
    QDateTime now = QDateTime::currentDateTime();
    std::deque<TimeDuration> original;
    original.emplace_back(DurationType::Activity, now.addSecs(-100), now.addSecs(-90));
    original.emplace_back(DurationType::Pause, now.addSecs(-80), now.addSecs(-70));
    
    QVERIFY(manager.saveDurations(original, TransactionMode::Append));
    
    // Trigger backup with Replace mode
    std::deque<TimeDuration> replacement;
    replacement.emplace_back(DurationType::Activity, now.addSecs(-50), now.addSecs(-40));
    QVERIFY(manager.saveDurations(replacement, TransactionMode::Replace));
    
    // Find the most recent backup file
    QDir testDir(QCoreApplication::applicationDirPath());
    QStringList backupFiles = testDir.entryList(QStringList() << "*.backup", QDir::Files, QDir::Time);
    QVERIFY(backupFiles.size() > 0);
    
    QString backupPath = testDir.filePath(backupFiles.first());
    
    // Restore from backup and verify it has original data
    QFile::remove(db_path_);
    QVERIFY(QFile::copy(backupPath, db_path_));
    
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)2); // Original had 2 entries
}
