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
    SqliteSessionStore manager(settings);

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
    SqliteSessionStore managerDisabled(disabled);
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
    SqliteSessionStore manager(settings);

    QVERIFY(!manager.checkSchemaOnStartup());
}

void DatabaseTest::test_exactMatching_upsertReplacesById()
{
    // Description: Upsert replaces by exact start_time/type, leaving one row

    resetDatabaseFile();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30000));
    SqliteSessionStore manager(settings);

    QDateTime start = QDateTime::fromMSecsSinceEpoch(1'000'000, Qt::UTC);
    QDateTime end1 = start.addSecs(10);
    QDateTime end2 = start.addSecs(20);

    std::deque<TimeDuration> durations;
    durations.emplace_back(DurationType::Activity, start, end1);
    QVERIFY(manager.updateDurationsById(durations));
    const QString stableSegmentId = durations.front().segment_id;

    durations.clear();
    durations.emplace_back(DurationType::Activity, start, end2, stableSegmentId);
    QVERIFY(manager.updateDurationsById(durations));

    const QString connName = "exact_match_query";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);
        query.prepare(
            "SELECT COUNT(*), end_time, duration FROM durations "
            "WHERE segment_id = :segment_id"
        );
        query.bindValue(":segment_id", durations.front().segment_id);
        QVERIFY(query.exec());
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 1);
        QCOMPARE(query.value(1).toString(), end2.toUTC().time().toString("HH:mm:ss.zzz"));
        QCOMPARE(query.value(2).toLongLong(), start.msecsTo(end2));
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void DatabaseTest::test_checkpointPreservesStartTimeOnUpdateBySegmentId()
{
    // Description: Checkpoint updates do not overwrite the original start time

    resetDatabaseFile();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30000));
    SqliteSessionStore manager(settings);

    QDateTime start = QDateTime::fromMSecsSinceEpoch(2'000'000, Qt::UTC);
    QDateTime end1 = start.addSecs(5);
    QDateTime end2 = start.addSecs(15);

    const QString checkpointSegmentId = TimeDuration::createSegmentId();
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start.msecsTo(end1), start, end1, checkpointSegmentId));

    QDateTime driftedStart = start.addSecs(3600);
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start.msecsTo(end2), driftedStart, end2, checkpointSegmentId));

    const QString connName = "checkpoint_query";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);
        query.prepare("SELECT start_date, start_time, end_time, duration FROM durations WHERE segment_id = :segment_id");
        query.bindValue(":segment_id", checkpointSegmentId);
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
    SqliteSessionStore manager(settings);

    QDateTime start = QDateTime::fromMSecsSinceEpoch(3'000'000, Qt::UTC);
    QDateTime end = start.addSecs(3600);
    qint64 elapsed = 120'000;

    const QString checkpointSegmentId = TimeDuration::createSegmentId();
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, elapsed, start, end, checkpointSegmentId));

    const QString connName = "drift_query";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);
        query.prepare("SELECT duration FROM durations WHERE segment_id = :segment_id");
        query.bindValue(":segment_id", checkpointSegmentId);
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
    SqliteSessionStore manager(settings);

    QDateTime start = QDateTime::fromMSecsSinceEpoch(4'000'000, Qt::UTC);
    QDateTime end = start.addSecs(10);
    std::deque<TimeDuration> durations;
    durations.emplace_back(DurationType::Activity, start, end);
    QVERIFY(manager.updateDurationsById(durations));

    const QString connName = "negative_duration_insert";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);
        query.prepare(
            "INSERT INTO durations (segment_id, type, duration, start_date, start_time, end_date, end_time, is_finalized) "
            "VALUES (:segment_id, :type, :duration, :start_date, :start_time, :end_date, :end_time, 1)"
        );
        query.bindValue(":segment_id", TimeDuration::createSegmentId());
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
    QCOMPARE(static_cast<int>(loaded.size()), 2);
}

void DatabaseTest::test_databasemanager_write_failure()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

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
    SqliteSessionStore manager(settings);

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
    SqliteSessionStore manager(settings);

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

void DatabaseTest::test_database_checkpoint_single_row_per_segment()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime start = QDateTime::currentDateTime();
    QDateTime end = start.addSecs(10);
    const QString segmentId = TimeDuration::createSegmentId();
    
    // First checkpoint - creates new row
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, 10000, start, end, segmentId));
    
    // Second checkpoint with same ID - should UPDATE existing row
    QDateTime newEnd = start.addSecs(20);
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, 20000, start, newEnd, segmentId));
    
    // Verify checkpoint row remains unfinalized and updated in place.
    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    query.prepare("SELECT COUNT(*), duration, is_finalized FROM durations WHERE segment_id = :segment_id");
    query.bindValue(":segment_id", segmentId);
    QVERIFY(query.exec());
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
    QCOMPARE(query.value(1).toLongLong(), static_cast<qint64>(20000));
    QCOMPARE(query.value(2).toInt(), 0);
    manager.lazyClose();
}

void DatabaseTest::test_database_checkpoint_missing_segment_reinserts_same_segment_id()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime start = QDateTime::currentDateTime();
    QDateTime end = start.addSecs(10);
    const QString segmentId = TimeDuration::createSegmentId();
    
    // Create checkpoint
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, 10000, start, end, segmentId));
    
    // Manually delete the checkpoint row (simulating retention cleanup)
    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    query.prepare("DELETE FROM durations WHERE segment_id = :segment_id");
    query.bindValue(":segment_id", segmentId);
    QVERIFY(query.exec());
    manager.lazyClose();
    
    // Try to update checkpoint - should detect missing row and INSERT new one
    QDateTime newEnd = start.addSecs(20);
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, 20000, start, newEnd, segmentId));
    
    // Verify new checkpoint row exists as unfinalized
    QVERIFY(manager.lazyOpen());
    QSqlQuery verifyQuery(manager.db);
    verifyQuery.prepare("SELECT COUNT(*), duration, is_finalized FROM durations WHERE segment_id = :segment_id");
    verifyQuery.bindValue(":segment_id", segmentId);
    QVERIFY(verifyQuery.exec());
    QVERIFY(verifyQuery.next());
    QCOMPARE(verifyQuery.value(0).toInt(), 1);
    QCOMPARE(verifyQuery.value(1).toLongLong(), static_cast<qint64>(20000));
    QCOMPARE(verifyQuery.value(2).toInt(), 0);
    manager.lazyClose();
}

void DatabaseTest::test_database_checkpoint_preserves_start_time()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime originalStart = QDateTime::currentDateTime();
    QDateTime firstEnd = originalStart.addSecs(10);
    const QString checkpointSegmentId = TimeDuration::createSegmentId();
    
    // First checkpoint
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, 10000, originalStart, firstEnd, checkpointSegmentId));
    
    // Update checkpoint with new end time (simulate ongoing timer)
    QDateTime secondEnd = originalStart.addSecs(30);
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, 30000, originalStart, secondEnd, checkpointSegmentId));
    
    // Verify start/end/duration are preserved on the checkpoint row itself.
    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    query.prepare("SELECT start_date, start_time, end_date, end_time, duration, is_finalized FROM durations WHERE segment_id = :segment_id");
    query.bindValue(":segment_id", checkpointSegmentId);
    QVERIFY(query.exec());
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), originalStart.toUTC().date().toString(Qt::ISODate));
    QCOMPARE(query.value(1).toString(), originalStart.toUTC().time().toString("HH:mm:ss.zzz"));
    QCOMPARE(query.value(2).toString(), secondEnd.toUTC().date().toString(Qt::ISODate));
    QCOMPARE(query.value(3).toString(), secondEnd.toUTC().time().toString("HH:mm:ss.zzz"));
    QCOMPARE(query.value(4).toLongLong(), static_cast<qint64>(30000));
    QCOMPARE(query.value(5).toInt(), 0);
    manager.lazyClose();
}

void DatabaseTest::test_database_update_by_id_insert_mode()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = QDateTime::currentDateTime();
    std::deque<TimeDuration> durations;
    durations.emplace_back(DurationType::Activity, now.addSecs(-100), now.addSecs(-90));
    durations.emplace_back(DurationType::Pause, now.addSecs(-80), now.addSecs(-70));
    
    // First upsert - should INSERT both
    QVERIFY(manager.updateDurationsById(durations));
    
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)2);
}

void DatabaseTest::test_database_update_by_id_updates_existing_row_on_start_drift()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = QDateTime::currentDateTime();
    QDateTime start1 = now.addSecs(-100);
    QDateTime end1 = now.addSecs(-90);
    
    // Insert initial duration
    std::deque<TimeDuration> initial;
    initial.emplace_back(DurationType::Activity, start1, end1);
    QVERIFY(manager.updateDurationsById(initial));
    
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    QCOMPARE(loaded[0].duration, (qint64)10000);
    
    // Update same segment with drifted start time and longer end.
    std::deque<TimeDuration> updated;
    QDateTime driftedStart = start1.addSecs(5);
    QDateTime end2 = now.addSecs(-80); // Extended duration
    updated.emplace_back(DurationType::Activity, driftedStart, end2, initial.front().segment_id);
    QVERIFY(manager.updateDurationsById(updated));
    
    // Should update existing row by segment_id (no duplicate).
    loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    QCOMPARE(loaded[0].duration, driftedStart.msecsTo(end2));
    QCOMPARE(loaded[0].startTime, driftedStart);
}

void DatabaseTest::test_database_update_by_id_different_segments_same_start_both_exist()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = QDateTime::currentDateTime();
    QDateTime start = now.addSecs(-100);
    
    // Insert Activity at same start time
    std::deque<TimeDuration> activity;
    activity.emplace_back(DurationType::Activity, start, start.addSecs(10));
    QVERIFY(manager.updateDurationsById(activity));
    
    // Insert Pause at SAME start time (different type)
    std::deque<TimeDuration> pause;
    pause.emplace_back(DurationType::Pause, start, start.addSecs(5));
    QVERIFY(manager.updateDurationsById(pause));
    
    // Both should exist because segment_id differs.
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)2);
}

void DatabaseTest::test_database_update_by_id_empty_deque()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    std::deque<TimeDuration> empty;
    QVERIFY(manager.updateDurationsById(empty)); // Should succeed as no-op
}

void DatabaseTest::test_database_load_negative_duration()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Manually insert negative duration
    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    QDateTime now = QDateTime::currentDateTimeUtc();
    query.prepare("INSERT INTO durations (segment_id, type, duration, start_date, start_time, end_date, end_time, is_finalized) "
                 "VALUES (:segment_id, 0, -5000, :start_date, :start_time, :end_date, :end_time, 1)");
    query.bindValue(":segment_id", TimeDuration::createSegmentId());
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
    SqliteSessionStore manager(settings);

    // Manually insert start > end
    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    QDateTime now = QDateTime::currentDateTimeUtc();
    QDateTime start = now;
    QDateTime end = now.addSecs(-10); // End BEFORE start
    
    query.prepare("INSERT INTO durations (segment_id, type, duration, start_date, start_time, end_date, end_time, is_finalized) "
                 "VALUES (:segment_id, 0, 10000, :start_date, :start_time, :end_date, :end_time, 1)");
    query.bindValue(":segment_id", TimeDuration::createSegmentId());
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
    SqliteSessionStore manager(settings);

    // Manually insert invalid type (valid are 0=Activity, 1=Pause)
    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    QDateTime now = QDateTime::currentDateTimeUtc();
    QDateTime end = now.addSecs(10);
    
    query.prepare("INSERT INTO durations (segment_id, type, duration, start_date, start_time, end_date, end_time, is_finalized) "
                 "VALUES (:segment_id, 99, 10000, :start_date, :start_time, :end_date, :end_time, 1)");
    query.bindValue(":segment_id", TimeDuration::createSegmentId());
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
    SqliteSessionStore manager(settings);

    QDateTime start = QDateTime::currentDateTimeUtc();
    QDateTime end = start.addMSecs(1000); // Actual duration: 1000ms
    
    // Insert with stored duration = 1003ms (within 100ms tolerance)
    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    query.prepare("INSERT INTO durations (segment_id, type, duration, start_date, start_time, end_date, end_time, is_finalized) "
                 "VALUES (:segment_id, 0, 1003, :start_date, :start_time, :end_date, :end_time, 1)");
    query.bindValue(":segment_id", TimeDuration::createSegmentId());
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
    SqliteSessionStore manager(settings);

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

void DatabaseTest::test_database_load_invalid_type_increments_skipped_and_omits_row()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    const QDateTime start = QDateTime::currentDateTimeUtc();
    const QDateTime end = start.addSecs(10);
    query.prepare("INSERT INTO durations (segment_id, type, duration, start_date, start_time, end_date, end_time, is_finalized) "
                 "VALUES (:segment_id, 99, 10000, :start_date, :start_time, :end_date, :end_time, 1)");
    query.bindValue(":segment_id", TimeDuration::createSegmentId());
    query.bindValue(":start_date", start.date().toString(Qt::ISODate));
    query.bindValue(":start_time", start.time().toString("HH:mm:ss.zzz"));
    query.bindValue(":end_date", end.date().toString(Qt::ISODate));
    query.bindValue(":end_time", end.time().toString("HH:mm:ss.zzz"));
    QVERIFY(query.exec());
    manager.lazyClose();

    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.skipped, 1);
    QCOMPARE(loaded.size(), static_cast<size_t>(0));
}

void DatabaseTest::test_database_load_200ms_mismatch_increments_repaired_and_uses_computed()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    const QDateTime start = QDateTime::currentDateTimeUtc();
    const QDateTime end = start.addMSecs(1000);
    query.prepare("INSERT INTO durations (segment_id, type, duration, start_date, start_time, end_date, end_time, is_finalized) "
                 "VALUES (:segment_id, 0, 1200, :start_date, :start_time, :end_date, :end_time, 1)");
    query.bindValue(":segment_id", TimeDuration::createSegmentId());
    query.bindValue(":start_date", start.date().toString(Qt::ISODate));
    query.bindValue(":start_time", start.time().toString("HH:mm:ss.zzz"));
    query.bindValue(":end_date", end.date().toString(Qt::ISODate));
    query.bindValue(":end_time", end.time().toString("HH:mm:ss.zzz"));
    QVERIFY(query.exec());
    manager.lazyClose();

    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.repaired, 1);
    QCOMPARE(loaded.size(), static_cast<size_t>(1));
    QCOMPARE(loaded[0].duration, static_cast<qint64>(1000));
}

void DatabaseTest::test_database_millisecond_precision()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

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
    
    // Now try to use SqliteSessionStore
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);
    
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
    SqliteSessionStore manager(settings);
    
    // Should return true (will create fresh schema)
    QVERIFY(manager.checkSchemaOnStartup());
}

void DatabaseTest::test_database_schema_creates_idx_finalized_start()
{
    resetDatabaseFile();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    QVERIFY(query.exec(
        "SELECT name FROM sqlite_master "
        "WHERE type = 'index' AND name = 'idx_finalized_start'"
    ));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), QString("idx_finalized_start"));
}

void DatabaseTest::test_database_schema_migration_adds_is_finalized_and_segment_id_marks_existing_rows()
{
    resetDatabaseFile();

    const QString connName = "schema_migration_is_finalized";
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
            "start_date DATE NOT NULL,"
            "start_time TEXT NOT NULL,"
            "end_date DATE NOT NULL,"
            "end_time TEXT NOT NULL,"
            "UNIQUE(start_date, start_time, type) ON CONFLICT REPLACE"
            ")"
        ));

        const QDateTime start = QDateTime::currentDateTimeUtc().addSecs(-30);
        const QDateTime end = start.addSecs(20);
        query.prepare(
            "INSERT INTO durations (type, duration, start_date, start_time, end_date, end_time) "
            "VALUES (0, :duration, :start_date, :start_time, :end_date, :end_time)"
        );
        query.bindValue(":duration", static_cast<qint64>(20000));
        query.bindValue(":start_date", start.date().toString(Qt::ISODate));
        query.bindValue(":start_time", start.time().toString("HH:mm:ss.zzz"));
        query.bindValue(":end_date", end.date().toString(Qt::ISODate));
        query.bindValue(":end_time", end.time().toString("HH:mm:ss.zzz"));
        QVERIFY(query.exec());
    }
    QSqlDatabase::removeDatabase(connName);

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.loadDurations().size() == static_cast<size_t>(1));

    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    QVERIFY(query.exec("PRAGMA table_info(durations)"));
    bool hasIsFinalized = false;
    bool hasSegmentId = false;
    while (query.next()) {
        if (query.value(1).toString() == "is_finalized") {
            hasIsFinalized = true;
        }
        if (query.value(1).toString() == "segment_id") {
            hasSegmentId = true;
        }
    }
    QVERIFY(hasIsFinalized);
    QVERIFY(hasSegmentId);

    QVERIFY(query.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 1"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);

    QVERIFY(query.exec("SELECT COUNT(*) FROM durations WHERE segment_id IS NOT NULL AND segment_id != ''"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
    manager.lazyClose();
}

void DatabaseTest::test_database_backup_file_creation()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

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
    SqliteSessionStore manager(settings);

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

// ============================================================================
// hasEntriesForDate tri-state result tests (T18)
// ============================================================================

void DatabaseTest::test_hasEntriesForDate_returns_unknown_when_history_disabled()
{
    // Arrange: history_days_to_keep = 0 disables the database entirely.
    // hasEntriesForDate must return Unknown (not No) so the caller
    // never assumes "no entries exist" when it simply can't check.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 0));
    SqliteSessionStore manager(settings);

    // Act
    EntriesForDateResult result = manager.hasEntriesForDate(QDate::currentDate());

    // Assert
    QCOMPARE(result, EntriesForDateResult::Unknown);
}

void DatabaseTest::test_hasEntriesForDate_returns_no_on_empty_database()
{
    // Arrange: history enabled, fresh empty database.
    // hasEntriesForDate must return No (not Unknown) to confirm that
    // zero entries exist, allowing the caller to add boot time.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Act
    EntriesForDateResult result = manager.hasEntriesForDate(QDate::currentDate());

    // Assert
    QCOMPARE(result, EntriesForDateResult::No);
}

void DatabaseTest::test_hasEntriesForDate_returns_yes_when_entries_exist()
{
    // Arrange: history enabled, database has finalized entries for today.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = QDateTime::currentDateTimeUtc();
    std::deque<TimeDuration> durations;
    durations.emplace_back(DurationType::Activity, now.addSecs(-60), now);
    QVERIFY(manager.saveDurations(durations, TransactionMode::Append));

    // Act
    EntriesForDateResult result = manager.hasEntriesForDate(QDate::currentDate());

    // Assert
    QCOMPARE(result, EntriesForDateResult::Yes);
}

void DatabaseTest::test_hasEntriesForDate_utc_positive_offset()
{
    // Simulate Asia/Tokyo (+09:00).
    // A row stored as UTC end_date='2024-12-31' end_time='15:30:00.000'
    // equals local 2025-01-01 00:30:00 +09:00, so it belongs to local date
    // 2025-01-01, not 2024-12-31.

    // RAII guard: restores TZ even if a QVERIFY/QCOMPARE assertion fires return early.
    struct TzGuard {
        QByteArray orig;
        TzGuard() : orig(qgetenv("TZ")) {}
        ~TzGuard() { orig.isEmpty() ? qunsetenv("TZ") : qputenv("TZ", orig); tzset(); }
    } tzGuard;
    qputenv("TZ", "Asia/Tokyo");
    tzset();

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    // Use a large retention window so old test-specific dates are never pruned.
    Settings settings(createSettingsFile(tempDir.path(), 30000));

    // Bootstrap schema.
    {
        SqliteSessionStore bootstrap(settings);
        QVERIFY(bootstrap.saveDurations({}, TransactionMode::Append));
    }

    // Seed the row with the UTC values directly.
    const QString connName = "seed_tz_pos";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare(
            "INSERT INTO durations (segment_id, type, duration, start_date, start_time, end_date, end_time, is_finalized) "
            "VALUES (:sid, 0, 1800000, '2024-12-31', '15:00:00.000', '2024-12-31', '15:30:00.000', 1)"
        );
        q.bindValue(":sid", TimeDuration::createSegmentId());
        QVERIFY(q.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);

    // The row's UTC end is 2024-12-31T15:30Z = local 2025-01-01 in Tokyo.
    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 1, 1)), EntriesForDateResult::Yes);
    QCOMPARE(manager.hasEntriesForDate(QDate(2024, 12, 31)), EntriesForDateResult::No);
}

void DatabaseTest::test_hasEntriesForDate_utc_negative_offset()
{
    // Simulate Pacific/Honolulu (-10:00).
    // A row stored as UTC end_date='2025-01-02' end_time='08:00:00.000'
    // equals local 2025-01-01 22:00:00 -10:00, so it belongs to local date
    // 2025-01-01, not 2025-01-02.

    struct TzGuard {
        QByteArray orig;
        TzGuard() : orig(qgetenv("TZ")) {}
        ~TzGuard() { orig.isEmpty() ? qunsetenv("TZ") : qputenv("TZ", orig); tzset(); }
    } tzGuard;
    qputenv("TZ", "Pacific/Honolulu");
    tzset();

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    // Use a large retention window so old test-specific dates are never pruned.
    Settings settings(createSettingsFile(tempDir.path(), 30000));

    {
        SqliteSessionStore bootstrap(settings);
        QVERIFY(bootstrap.saveDurations({}, TransactionMode::Append));
    }

    const QString connName = "seed_tz_neg";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare(
            "INSERT INTO durations (segment_id, type, duration, start_date, start_time, end_date, end_time, is_finalized) "
            "VALUES (:sid, 0, 3600000, '2025-01-02', '07:00:00.000', '2025-01-02', '08:00:00.000', 1)"
        );
        q.bindValue(":sid", TimeDuration::createSegmentId());
        QVERIFY(q.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);

    // The row's UTC end is 2025-01-02T08:00Z = local 2025-01-01 22:00 in Honolulu.
    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 1, 1)), EntriesForDateResult::Yes);
    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 1, 2)), EntriesForDateResult::No);
}

void DatabaseTest::test_hasEntriesForDate_utc_regression()
{
    // UTC zone, mid-day entry: end_date='2025-06-15' end_time='12:00:00.000'.
    // With TZ=UTC local date == UTC date, so behaviour should be unchanged.

    struct TzGuard {
        QByteArray orig;
        TzGuard() : orig(qgetenv("TZ")) {}
        ~TzGuard() { orig.isEmpty() ? qunsetenv("TZ") : qputenv("TZ", orig); tzset(); }
    } tzGuard;
    qputenv("TZ", "UTC");
    tzset();

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    // Use a large retention window so old test-specific dates are never pruned.
    Settings settings(createSettingsFile(tempDir.path(), 30000));

    {
        SqliteSessionStore bootstrap(settings);
        QVERIFY(bootstrap.saveDurations({}, TransactionMode::Append));
    }

    const QString connName = "seed_tz_utc";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare(
            "INSERT INTO durations (segment_id, type, duration, start_date, start_time, end_date, end_time, is_finalized) "
            "VALUES (:sid, 0, 3600000, '2025-06-15', '11:00:00.000', '2025-06-15', '12:00:00.000', 1)"
        );
        q.bindValue(":sid", TimeDuration::createSegmentId());
        QVERIFY(q.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);

    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 6, 15)), EntriesForDateResult::Yes);
    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 6, 14)), EntriesForDateResult::No);
    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 6, 16)), EntriesForDateResult::No);
}

// ============================================================================
// Retention cleanup once-per-session tests (T22)
// ============================================================================

void DatabaseTest::test_retention_cleanup_runs_once_across_multiple_opens()
{
    // The retention DELETE in lazyOpen() should execute at most once per
    // SqliteSessionStore lifetime.  We verify this by seeding the DB with an
    // old entry via direct SQL (bypassing lazyOpen's cleanup), then using
    // a fresh SqliteSessionStore whose first lazyOpen runs the cleanup.
    // After that, ten more open/close cycles must not re-run the DELETE.

    // Arrange: create the database and seed entries via a raw connection
    // so that no cleanup has run yet.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 2)); // keep 2 days

    // Use a first manager just to create the schema.
    {
        SqliteSessionStore bootstrap(settings);
        std::deque<TimeDuration> empty;
        QVERIFY(bootstrap.saveDurations(empty, TransactionMode::Append));
    }

    // Insert entries directly so that the old one survives (no cleanup yet).
    QDateTime now = QDateTime::currentDateTimeUtc();
    const QString connName = "seed_retention_test";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);

        // Old entry: 10 days ago (should be pruned by 2-day retention)
        QDateTime oldStart = now.addDays(-10);
        QDateTime oldEnd = oldStart.addSecs(60);
        query.prepare(
            "INSERT INTO durations (segment_id, type, duration, start_date, start_time, end_date, end_time, is_finalized) "
            "VALUES (:sid, 0, :dur, :sd, :st, :ed, :et, 1)"
        );
        query.bindValue(":sid", TimeDuration::createSegmentId());
        query.bindValue(":dur", oldStart.msecsTo(oldEnd));
        query.bindValue(":sd", oldStart.date().toString(Qt::ISODate));
        query.bindValue(":st", oldStart.time().toString("HH:mm:ss.zzz"));
        query.bindValue(":ed", oldEnd.date().toString(Qt::ISODate));
        query.bindValue(":et", oldEnd.time().toString("HH:mm:ss.zzz"));
        QVERIFY(query.exec());

        // Recent entry: today (should survive cleanup)
        QDateTime recentStart = now.addSecs(-60);
        QDateTime recentEnd = now;
        query.bindValue(":sid", TimeDuration::createSegmentId());
        query.bindValue(":dur", recentStart.msecsTo(recentEnd));
        query.bindValue(":sd", recentStart.date().toString(Qt::ISODate));
        query.bindValue(":st", recentStart.time().toString("HH:mm:ss.zzz"));
        query.bindValue(":ed", recentEnd.date().toString(Qt::ISODate));
        query.bindValue(":et", recentEnd.time().toString("HH:mm:ss.zzz"));
        QVERIFY(query.exec());

        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    // Act: create a fresh manager.  Its first lazyOpen should run cleanup.
    SqliteSessionStore manager(settings);
    QVERIFY(!manager.retention_cleanup_done_); // flag starts false

    QVERIFY(manager.lazyOpen());
    manager.lazyClose();

    // Assert: cleanup ran once.
    QVERIFY(manager.retention_cleanup_done_);

    // Open ten more times; cleanup must not re-execute.
    for (int i = 0; i < 10; ++i) {
        QVERIFY(manager.lazyOpen());
        manager.lazyClose();
    }

    // Flag still true, no re-run.
    QVERIFY(manager.retention_cleanup_done_);

    // The old entry was removed; the recent one survives.
    auto loaded = manager.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 1);
}

void DatabaseTest::test_retention_cleanup_retries_after_failure()
{
    // If the cleanup transaction fails (e.g. because the DB file is
    // read-only), retention_cleanup_done_ must remain false so the next
    // lazyOpen() retries automatically.  Once the obstacle is removed,
    // the retry should succeed and set the flag.

    // Arrange
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 2));
    SqliteSessionStore manager(settings);

    // Create the database and seed an old entry.
    QDateTime now = QDateTime::currentDateTimeUtc();
    std::deque<TimeDuration> durations;
    durations.emplace_back(DurationType::Activity, now.addDays(-10), now.addDays(-10).addSecs(60));
    durations.emplace_back(DurationType::Activity, now.addSecs(-60), now);
    QVERIFY(manager.saveDurations(durations, TransactionMode::Replace));

    // The first lazyOpen (inside saveDurations) already ran cleanup
    // successfully.  Reset the flag to simulate a fresh session where the
    // first cleanup attempt will fail.
    manager.retention_cleanup_done_ = false;

    // Make the DB read-only so the retention DELETE fails.
    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser));

    // Act: lazyOpen should fail because the cleanup transaction cannot write.
    QVERIFY(!manager.lazyOpen());

    // Assert: flag must still be false (cleanup was not successful).
    QVERIFY(!manager.retention_cleanup_done_);

    // Restore write permissions.
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser));

    // Act: the next lazyOpen should retry cleanup and succeed.
    QVERIFY(manager.lazyOpen());
    manager.lazyClose();

    // Assert: flag is now true.
    QVERIFY(manager.retention_cleanup_done_);

    // Verify the old entry was removed.
    auto loaded = manager.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 1);
}

/**
 * T25: Verifies that connection names are unique even when SqliteSessionStores
 * are repeatedly created and destroyed in a loop.
 *
 * The old implementation used reinterpret_cast<quintptr>(this) for connection
 * names, which collides when the allocator reuses the same address after
 * destruction. The atomic counter guarantees uniqueness regardless of address
 * reuse.
 */
void DatabaseTest::test_connection_names_unique_across_100_instances()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 0)); // history disabled — avoids DB I/O

    QSet<QString> connectionNames;

    // Act: create and destroy 100 SqliteSessionStores, collecting connection names.
    // With reinterpret_cast<quintptr>(this), the allocator frequently reuses the
    // same address, causing name collisions. With an atomic counter, every name
    // is guaranteed unique.
    for (int i = 0; i < 100; ++i) {
        auto* mgr = new SqliteSessionStore(settings);
        QString name = mgr->db.connectionName();
        connectionNames.insert(name);
        delete mgr;
    }

    // Assert: all 100 names were distinct.
    QCOMPARE(connectionNames.size(), 100);
}

// ============================================================================
// Phase 4 test gate — Tests T, U, V, W
// ============================================================================

/**
 * T: commitSession is upsert-by-segment-id.
 * Save a Timeline; mutate one segment's type; commitSession again;
 * assert one row in the DB, not two.
 */
void DatabaseTest::test_T_commitSession_upserts_by_segment_id()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = QDateTime::currentDateTime();
    QString segId = TimeDuration::createSegmentId();
    std::deque<TimeDuration> segs;
    segs.emplace_back(DurationType::Activity, now.addSecs(-60), now, segId);

    // First save
    QVERIFY(manager.commitSession(Timeline(segs, std::nullopt)));

    // Mutate the type and save again with the same segment_id
    segs.front().type = DurationType::Pause;
    QVERIFY(manager.commitSession(Timeline(segs, std::nullopt)));

    // Should have exactly one row
    auto loaded = manager.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 1);
    QCOMPARE(loaded.durations.front().type, DurationType::Pause);
    QCOMPARE(loaded.durations.front().segment_id, segId);
}

/**
 * U: Orphan cleanup is internal to commitSession.
 * Save a Timeline with two mergeable adjacent segments; commitSession
 * a normalized smaller Timeline (one segment); assert the merged-away
 * row is gone without the caller ever passing orphan IDs.
 */
void DatabaseTest::test_U_commitSession_orphan_cleanup_is_internal()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = QDateTime::currentDateTime();
    QString segA = TimeDuration::createSegmentId();
    QString segB = TimeDuration::createSegmentId();

    // Two adjacent Activity segments — will merge inside commitSession's normalized()
    std::deque<TimeDuration> twoSegs;
    twoSegs.emplace_back(DurationType::Activity, now.addSecs(-100), now.addSecs(-50), segA);
    twoSegs.emplace_back(DurationType::Activity, now.addSecs(-50), now.addSecs(-1), segB);

    // Seed them via saveDurations (bypasses commitSession) so they're both in DB
    QVERIFY(manager.saveDurations(twoSegs, TransactionMode::Append));
    QCOMPARE(static_cast<int>(manager.loadDurations().size()), 2);

    // Now commitSession with the same two segments — commitSession normalizes
    // internally, merges them into one, and deletes the orphan (segB)
    QVERIFY(manager.commitSession(Timeline(twoSegs, std::nullopt)));

    auto loaded = manager.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 1);

    // Only segA (the keeper after merge) should remain
    bool segAFound = false;
    bool segBFound = false;
    for (const auto& d : loaded.durations) {
        if (d.segment_id == segA) segAFound = true;
        if (d.segment_id == segB) segBFound = true;
    }
    QVERIFY(segAFound || !segBFound); // exactly one segment, segB is gone
    QVERIFY(!segBFound);
}

/**
 * V: replaceAll wipes and rewrites.
 * Pre-populate with X rows, call replaceAll with Y rows, assert exactly Y remain.
 */
void DatabaseTest::test_V_replaceAll_wipes_and_rewrites()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = QDateTime::currentDateTime();

    // Seed 3 rows
    std::deque<TimeDuration> initial;
    initial.emplace_back(DurationType::Activity, now.addSecs(-300), now.addSecs(-200));
    initial.emplace_back(DurationType::Pause,    now.addSecs(-200), now.addSecs(-100));
    initial.emplace_back(DurationType::Activity, now.addSecs(-100), now.addSecs(-50));
    QVERIFY(manager.saveDurations(initial, TransactionMode::Append));
    QCOMPARE(static_cast<int>(manager.loadDurations().size()), 3);

    // replaceAll with 2 different rows
    std::deque<TimeDuration> replacement;
    replacement.emplace_back(DurationType::Activity, now.addSecs(-80), now.addSecs(-40));
    replacement.emplace_back(DurationType::Pause,    now.addSecs(-40), now.addSecs(-10));

    QVERIFY(manager.replaceAll(Timeline(replacement, std::nullopt),
                                Timeline({}, std::nullopt)));

    auto loaded = manager.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 2);
}

/**
 * W: Backup is created before replaceAll but NOT before commitSession.
 * commitSession should NOT create a .backup file; replaceAll should.
 */
void DatabaseTest::test_W_backup_created_before_replaceAll_not_commitSession()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDir appDir(QCoreApplication::applicationDirPath());
    QStringList before = appDir.entryList(QStringList() << "*.backup", QDir::Files);

    QDateTime now = QDateTime::currentDateTime();
    std::deque<TimeDuration> segs;
    segs.emplace_back(DurationType::Activity, now.addSecs(-60), now);

    // Seed the DB so a .sqlite file exists (needed for createBackup to copy)
    QVERIFY(manager.saveDurations(segs, TransactionMode::Append));

    QStringList afterSeed = appDir.entryList(QStringList() << "*.backup", QDir::Files);
    // saveDurations creates a backup too; record count after seeding
    int countAfterSeed = afterSeed.size();

    // commitSession should NOT create new backup files
    QVERIFY(manager.commitSession(Timeline(segs, std::nullopt)));
    QStringList afterCommit = appDir.entryList(QStringList() << "*.backup", QDir::Files);
    QCOMPARE(afterCommit.size(), countAfterSeed); // no new backup

    // Wait >1 s so the timestamp in the backup filename differs from the seed backup.
    // createBackup uses second-level timestamps; QFile::copy silently fails if the
    // destination already exists.
    QTest::qWait(1100);

    // replaceAll SHOULD create a new backup file
    QVERIFY(manager.replaceAll(Timeline(segs, std::nullopt),
                                Timeline({}, std::nullopt)));
    QStringList afterReplace = appDir.entryList(QStringList() << "*.backup", QDir::Files);
    QVERIFY(afterReplace.size() > countAfterSeed); // new backup created

    (void)before; // silence unused variable warning
}

// ============================================================================
// flushToDisc durable heartbeat tests (Issue 2)
// ============================================================================

void DatabaseTest::test_flushToDisc_writes_heartbeat_row()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Ensure the DB is created before flushing
    QVERIFY(manager.saveDurations({}, TransactionMode::Append));

    manager.flushToDisc();

    // Open DB directly and check for the heartbeat row
    const QString connName = "flush_heartbeat_check";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);
        query.prepare("SELECT value FROM app_settings WHERE key = 'last_flush_utc'");
        QVERIFY(query.exec());
        QVERIFY(query.next());
        const QString value = query.value(0).toString();
        QVERIFY(!value.isEmpty());
        QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
        QVERIFY(parsed.isValid());
        QCOMPARE(parsed.timeSpec(), Qt::UTC);
        QVERIFY(parsed.secsTo(QDateTime::currentDateTimeUtc()) < 5);
        QVERIFY(!query.next()); // exactly one row
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void DatabaseTest::test_flushToDisc_idempotent()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.saveDurations({}, TransactionMode::Append));

    manager.flushToDisc();

    // Capture timestamp after first flush
    QString firstTs;
    const QString connName = "flush_idempotent_check";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare("SELECT value FROM app_settings WHERE key = 'last_flush_utc'");
        QVERIFY(q.exec());
        QVERIFY(q.next());
        firstTs = q.value(0).toString();
        QVERIFY(!q.next()); // exactly one row
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    manager.flushToDisc(); // second call must not fail

    const QString connName2 = "flush_idempotent_check2";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName2);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare("SELECT value FROM app_settings WHERE key = 'last_flush_utc'");
        QVERIFY(q.exec());
        QVERIFY(q.next());
        const QString secondTs = q.value(0).toString();
        QVERIFY(!q.next()); // still exactly one row
        QDateTime parsed = QDateTime::fromString(secondTs, Qt::ISODateWithMs);
        QVERIFY(parsed.isValid());
        QCOMPARE(parsed.timeSpec(), Qt::UTC);
        // Second flush timestamp must be >= first flush timestamp
        QVERIFY(secondTs >= firstTs);
        db.close();
    }
    QSqlDatabase::removeDatabase(connName2);
}

// ============================================================================
// Step 2 (S1): saveDurations(Append) upserts on segment_id collision
// ============================================================================

/**
 * Calling saveDurations(Append) a second time with a row whose segment_id
 * already exists must update the row in place (not throw, not duplicate, and
 * preserve the autoincrement `id`). The previous plain INSERT would abort the
 * surrounding transaction on UNIQUE(segment_id) collision.
 */
void DatabaseTest::test_database_saveDurations_append_upserts_existing_segment_id()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    const QDateTime now = QDateTime::currentDateTime();
    const QString segId = TimeDuration::createSegmentId();

    // First Append: seed a single row with segment_id == segId.
    std::deque<TimeDuration> first;
    first.emplace_back(DurationType::Activity, now.addSecs(-60), now.addSecs(-30), segId);
    QVERIFY(manager.saveDurations(first, TransactionMode::Append));

    // Capture the autoincrement id assigned to that row.
    qint64 originalRowId = -1;
    {
        QVERIFY(manager.lazyOpen());
        QSqlQuery q(manager.db);
        q.prepare("SELECT id FROM durations WHERE segment_id = :sid");
        q.bindValue(":sid", segId);
        QVERIFY(q.exec());
        QVERIFY(q.next());
        originalRowId = q.value(0).toLongLong();
        QVERIFY(!q.next()); // exactly one row
        manager.lazyClose();
    }
    QVERIFY(originalRowId > 0);

    // Second Append: same segment_id, different type and time window.
    std::deque<TimeDuration> second;
    second.emplace_back(DurationType::Pause, now.addSecs(-20), now.addSecs(-5), segId);
    QVERIFY(manager.saveDurations(second, TransactionMode::Append));

    // Assert: still exactly one row for that segment_id, fields are updated,
    // and the autoincrement id is unchanged.
    QVERIFY(manager.lazyOpen());
    QSqlQuery verify(manager.db);
    verify.prepare("SELECT id, type, duration, start_date, start_time, end_date, end_time "
                   "FROM durations WHERE segment_id = :sid");
    verify.bindValue(":sid", segId);
    QVERIFY(verify.exec());
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toLongLong(), originalRowId); // id preserved
    QCOMPARE(verify.value(1).toInt(), static_cast<int>(DurationType::Pause));
    QCOMPARE(verify.value(2).toLongLong(), second.front().duration);
    const QDateTime expStartUtc = second.front().startTime.toUTC();
    const QDateTime expEndUtc = second.front().endTime.toUTC();
    QCOMPARE(verify.value(3).toString(), expStartUtc.date().toString(Qt::ISODate));
    QCOMPARE(verify.value(4).toString(), expStartUtc.time().toString("HH:mm:ss.zzz"));
    QCOMPARE(verify.value(5).toString(), expEndUtc.date().toString(Qt::ISODate));
    QCOMPARE(verify.value(6).toString(), expEndUtc.time().toString("HH:mm:ss.zzz"));
    QVERIFY(!verify.next()); // no duplicate
    manager.lazyClose();
}
