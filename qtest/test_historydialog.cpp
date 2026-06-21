#include "test_historydialog.h"
#include <QtTest>
#include <QTimer>
#include <QTableWidget>
#include <QCheckBox>
#include <QLabel>
#include <QMessageBox>

#include "../historydialog.h"
#include "../historyeditsession.h"

using TestCommon::createSettingsFile;
using TestCommon::mk;
using TestCommon::sumDurations;

namespace {
QDateTime makeTime(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC);
}
}

void HistoryDialogTest::initTestCase()
{
    db_path_ = QDir(QCoreApplication::applicationDirPath()).filePath("uTimer.sqlite");
    if (QFile::exists(db_path_)) {
        db_backup_path_ = db_path_ + ".bak_test";
        QFile::remove(db_backup_path_);
        QVERIFY(QFile::rename(db_path_, db_backup_path_));
    }
}

void HistoryDialogTest::cleanupTestCase()
{
    if (!db_path_.isEmpty()) {
        QFile::remove(db_path_);
    }
    if (!db_backup_path_.isEmpty()) {
        QFile::remove(db_path_);
        QVERIFY(QFile::rename(db_backup_path_, db_path_));
    }
}

void HistoryDialogTest::test_historydialog_createPages_includes_current_db_ongoing()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    QDateTime now = QDateTime::currentDateTime();
    tracker.sessionState_dbg().durations.push_back(TimeDuration(DurationType::Activity, now.addSecs(-120), now.addSecs(-60)));

    // Save a DB entry for today
    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Pause, now.addSecs(-50), now.addSecs(-40));
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)));

    // Start ongoing activity
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(5);

    HistoryDialog dialog(tracker, settings);
    QCOMPARE(dialog.editSession_dbg().pages().size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().pages()[0].isCurrent, true);
    // completed=1 (DB row), ongoing=1; total display rows=2
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
    QVERIFY(dialog.editSession_dbg().pendingTimelines()[0].ongoing().has_value());
    QVERIFY(dialog.editSession_dbg().pendingTimelines()[0].ongoing()->duration > 0);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
}

void HistoryDialogTest::test_historydialog_createPages_dedups_db_row_with_small_time_drift()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime memoryStart = now.addSecs(-40).addMSecs(2);
    const QDateTime memoryEnd = now.addSecs(-10);
    const SegmentId segmentId = SegmentId::mint();
    tracker.sessionState_dbg().durations.push_back(TimeDuration(DurationType::Activity, memoryStart, memoryEnd, segmentId));

    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Activity, memoryStart.addMSecs(-2), memoryEnd, segmentId);
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)));

    HistoryDialog dialog(tracker, settings);
    QCOMPARE(dialog.editSession_dbg().pages().size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].segment_id.toString()), true);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].startTime, memoryStart);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].endTime, memoryEnd);
}

void HistoryDialogTest::test_historydialog_createPages_groups_unsplit_cross_midnight_row_by_start_date()
{
    // H2: cross-midnight rows are now loaded (via fromTrusted) and bucketed on BOTH
    // the start-date page (canonical) and the end-date page (continuation/display-only).
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDate yesterday = QDate::currentDate().addDays(-1);
    const QDateTime crossMidnightStart(yesterday, QTime(23, 59, 50));
    const QDateTime crossMidnightEnd = crossMidnightStart.addSecs(20); // ends today

    // Trigger DB creation by saving a valid same-day row for yesterday.
    {
        const QDateTime validStart(yesterday, QTime(10, 0, 0));
        const QDateTime validEnd(yesterday, QTime(10, 30, 0));
        auto validSeg = TimeDuration::create(DurationType::Activity, validStart, validEnd);
        QVERIFY(validSeg.has_value());
        std::deque<TimeDuration> init;
        init.push_back(std::move(*validSeg));
        QVERIFY(tracker.replaceAll(Timeline(std::move(init), std::nullopt), Timeline({}, std::nullopt)));
    }

    // Insert the cross-midnight row directly via SQL (bypassing same-day invariant).
    {
        const QString connName = QString("test_cross_midnight_%1").arg(
            reinterpret_cast<quintptr>(&db));
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(db_path_);
        QVERIFY(rawDb.open());
        QSqlQuery q(rawDb);
        q.prepare(
            "INSERT INTO durations "
            "(segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, :start_utc, :end_utc, 1)"
        );
        q.bindValue(":sid", TimeDuration::createSegmentId().toString());
        q.bindValue(":start_utc", crossMidnightStart.toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":end_utc", crossMidnightEnd.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
        rawDb.close();
        rawDb = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
    }

    // H2: cross-midnight row is loaded. It appears as crossMidnight on yesterday's
    // page (display-only canonical) and as a continuation on today's page.
    HistoryDialog dialog(tracker, settings);

    // 2 pages: today (isCurrent) and yesterday.
    QCOMPARE(dialog.editSession_dbg().pages().size(), static_cast<size_t>(2));
    QCOMPARE(dialog.editSession_dbg().pages()[0].isCurrent, true);
    QCOMPARE(dialog.editSession_dbg().pages()[1].isCurrent, false);

    // Yesterday: 1 same-day canonical row + 1 cross-midnight (display-only).
    QCOMPARE(dialog.editSession_dbg().pages()[1].durations.size(), static_cast<size_t>(1));     // valid same-day
    QCOMPARE(dialog.editSession_dbg().pages()[1].crossMidnight.size(), static_cast<size_t>(1)); // cross-midnight canonical
    QCOMPARE(dialog.editSession_dbg().pages()[1].continuations.size(), static_cast<size_t>(0));

    // Today's page has the cross-midnight row as a continuation (display-only).
    QCOMPARE(dialog.editSession_dbg().pages()[0].continuations.size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().pages()[0].continuations[0].startTime.date(), yesterday);
    QCOMPARE(dialog.editSession_dbg().pages()[0].crossMidnight.size(), static_cast<size_t>(0));
}

void HistoryDialogTest::test_historydialog_checkbox_toggle_updates_pending_and_totals()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    QDateTime now = QDateTime::currentDateTime();
    tracker.sessionState_dbg().durations.push_back(TimeDuration(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    HistoryDialog dialog(tracker, settings);
    dialog.show();

    QCheckBox* box = qobject_cast<QCheckBox*>(dialog.table_dbg()->cellWidget(0, 3));
    QVERIFY(box != nullptr);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].type, DurationType::Activity);
    box->setChecked(false);

    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].type, DurationType::Pause);
    QVERIFY(dialog.pageLabel_dbg()->text().contains("Pause"));
}

void HistoryDialogTest::test_historydialog_saveChanges_updates_timetracker_and_db()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    QDateTime now = QDateTime::currentDateTime();
    tracker.sessionState_dbg().durations.push_back(TimeDuration(DurationType::Activity, now.addSecs(-200), now.addSecs(-150)));

    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Pause, now.addSecs(-140), now.addSecs(-120));
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)));

    HistoryDialog dialog(tracker, settings);
    dialog.editSession_dbg().pendingTimelines()[0] = dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause);

    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    QCOMPARE(tracker.sessionState_dbg().durations.size(), static_cast<size_t>(1));
    QCOMPARE(tracker.sessionState_dbg().durations[0].type, DurationType::Pause);

    SqliteSessionStore db2(settings);
    auto loaded = db2.loadDurations();
    QVERIFY(loaded.size() >= 1);
}

void HistoryDialogTest::test_historydialog_split_action_splits_row()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    QDateTime start = QDateTime::currentDateTime().addSecs(-10);
    QDateTime end = QDateTime::currentDateTime().addSecs(-4);
    tracker.sessionState_dbg().durations.push_back(TimeDuration(DurationType::Activity, start, end));
    const SegmentId originalSegmentId = tracker.sessionState_dbg().durations.back().segment_id;

    HistoryDialog dialog(tracker, settings);
    dialog.setContextMenuTarget_dbg(0, 0);

    QTimer::singleShot(0, []() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* split = qobject_cast<SplitDialog*>(widget);
            if (!split) continue;
            split->setFirstSegmentType(DurationType::Activity);
            split->setSecondSegmentType(DurationType::Pause);
            split->accept();
            return;
        }
    });

    dialog.onSplitRow_dbg();
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(2));
    const auto& first = dialog.editSession_dbg().pendingTimelines()[0].completed()[0];
    const auto& second = dialog.editSession_dbg().pendingTimelines()[0].completed()[1];
    QCOMPARE(first.startTime, start);
    QCOMPARE(second.endTime, end);
    QCOMPARE(first.endTime, second.startTime);
    QCOMPARE(first.type, DurationType::Activity);
    QCOMPARE(second.type, DurationType::Pause);
    QCOMPARE(first.segment_id, originalSegmentId);
    QVERIFY(!second.segment_id.isEmpty());
    QVERIFY(second.segment_id != originalSegmentId);
}

void HistoryDialogTest::test_historydialog_split_today_mixed_origins_routes_to_correct_bucket()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime memStart = now.addSecs(-20);
    const QDateTime memEnd = now.addSecs(-12);
    const QDateTime dbStart = now.addSecs(-11);
    const QDateTime dbEnd = now.addSecs(-6);

    tracker.sessionState_dbg().durations.push_back(TimeDuration(DurationType::Activity, memStart, memEnd));

    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Pause, dbStart, dbEnd);
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)));

    HistoryDialog dialog(tracker, settings);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(2));
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].segment_id.toString()), true);
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[1].segment_id.toString()), false);

    dialog.setContextMenuTarget_dbg(0, 0);

    QTimer::singleShot(0, []() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* split = qobject_cast<SplitDialog*>(widget);
            if (!split) continue;
            split->setFirstSegmentType(DurationType::Activity);
            split->setSecondSegmentType(DurationType::Pause);
            split->accept();
            return;
        }
    });

    dialog.onSplitRow_dbg();
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(3));
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].segment_id.toString()), true);
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[1].segment_id.toString()), true);
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[2].segment_id.toString()), false);

    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    QCOMPARE(tracker.sessionState_dbg().durations.size(), static_cast<size_t>(2));
    QCOMPARE(tracker.sessionState_dbg().durations[0].startTime, memStart);
    QCOMPARE(tracker.sessionState_dbg().durations[1].endTime, memEnd);
    QCOMPARE(tracker.sessionState_dbg().durations[0].endTime, tracker.sessionState_dbg().durations[1].startTime);

    SqliteSessionStore db2(settings);
    auto loaded = db2.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(1));
    QCOMPARE(loaded[0].startTime, dbStart);
    QCOMPARE(loaded[0].endTime, dbEnd);
    QCOMPARE(loaded[0].type, DurationType::Pause);
}

void HistoryDialogTest::test_historydialog_split_non_today_db_row_survives_save_roundtrip()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDateTime start(QDate::currentDate().addDays(-1), QTime(10, 0, 0));
    const QDateTime end = start.addSecs(8);

    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Activity, start, end);
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)));

    HistoryDialog dialog(tracker, settings);
    QVERIFY(dialog.editSession_dbg().pages().size() >= static_cast<size_t>(2));

    dialog.setContextMenuTarget_dbg(0, 1);

    QTimer::singleShot(0, []() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* split = qobject_cast<SplitDialog*>(widget);
            if (!split) continue;
            split->setFirstSegmentType(DurationType::Activity);
            split->setSecondSegmentType(DurationType::Pause);
            split->accept();
            return;
        }
    });

    dialog.onSplitRow_dbg();
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed().size(), static_cast<size_t>(2));
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed().size(), static_cast<size_t>(2));
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[1].completed()[0].segment_id.toString()), false);
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[1].completed()[1].segment_id.toString()), false);

    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    SqliteSessionStore db2(settings);
    auto loaded = db2.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(2));
    QCOMPARE(loaded[0].startTime, start);
    QCOMPARE(loaded[1].endTime, end);
    QCOMPARE(loaded[0].endTime, loaded[1].startTime);
}

void HistoryDialogTest::test_historydialog_shows_load_reconciliation_banner()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    SqliteSessionStore manager(settings);
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery query(manager.rawDb_dbg());
    const QDateTime start = QDateTime::currentDateTimeUtc();
    const QDateTime end = start.addMSecs(1000);

    // Row with invalid type → will be skipped
    query.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:segment_id, 99, :start_utc, :end_utc, 1)");
    query.bindValue(":segment_id", TimeDuration::createSegmentId().toString());
    query.bindValue(":start_utc", start.toString(Qt::ISODateWithMs));
    query.bindValue(":end_utc", end.toString(Qt::ISODateWithMs));
    QVERIFY(query.exec());

    // Row with valid type → loads normally; no stored duration to mismatch
    query.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:segment_id, 0, :start_utc, :end_utc, 1)");
    query.bindValue(":segment_id", TimeDuration::createSegmentId().toString());
    query.bindValue(":start_utc", start.addSecs(10).toString(Qt::ISODateWithMs));
    query.bindValue(":end_utc", end.addSecs(10).toString(Qt::ISODateWithMs));
    QVERIFY(query.exec());
    manager.lazyClose_dbg();

    HistoryDialog dialog(tracker, settings);
    const QString msg = dialog.getLoadReconciliationMessage();
    QCOMPARE(msg, QString("1 rows skipped due to corrupt data, 0 rows auto-repaired."));
    QVERIFY(dialog.loadReconciliationLabel_dbg() != nullptr);
    QCOMPARE(dialog.loadReconciliationLabel_dbg()->text(), msg);
    QVERIFY(!dialog.loadReconciliationLabel_dbg()->isHidden());
}

void HistoryDialogTest::test_historydialog_save_unrelated_edit_preserves_row_and_creates_new_checkpoint()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    SqliteSessionStore db2(settings);
    Timer tracker(settings, db2);

    const QDateTime historicalStart(QDate::currentDate().addDays(-1), QTime(10, 0, 0));
    const QDateTime historicalEnd = historicalStart.addSecs(300);
    std::deque<TimeDuration> historicalRows;
    historicalRows.emplace_back(DurationType::Pause, historicalStart, historicalEnd);
    QVERIFY(tracker.replaceAll(Timeline(historicalRows, std::nullopt), Timeline({}, std::nullopt)));


    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    tracker.triggerCheckpoint_dbg();
    const SegmentId oldCheckpointSegmentId = tracker.sessionState_dbg().segment_id;
    QVERIFY(!oldCheckpointSegmentId.isEmpty());

    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(dialog.editSession_dbg().pages().size() >= static_cast<size_t>(2));
        dialog.editSession_dbg().pendingTimelines()[1] = dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(0, DurationType::Activity);
        dialog.done(QDialog::Accepted);
        dialog.saveChanges();
    }

    QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());
    QCOMPARE(tracker.sessionState_dbg().segment_id.toString(), oldCheckpointSegmentId.toString());

    QTest::qWait(20);
    tracker.triggerCheckpoint_dbg();

    const QString connName = "historydialog_t6_case1";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());

        QSqlQuery historicalQuery(db);
        historicalQuery.prepare(
            "SELECT type FROM durations WHERE start_utc = :start_utc AND end_utc = :end_utc AND is_finalized = 1"
        );
        historicalQuery.bindValue(":start_utc", historicalStart.toUTC().toString(Qt::ISODateWithMs));
        historicalQuery.bindValue(":end_utc", historicalEnd.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(historicalQuery.exec());
        QVERIFY(historicalQuery.next());
        QCOMPARE(historicalQuery.value(0).toInt(), static_cast<int>(DurationType::Activity));

        QSqlQuery checkpointQuery(db);
        checkpointQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0");
        QVERIFY(checkpointQuery.next());
        QCOMPARE(checkpointQuery.value(0).toInt(), 1);

        QSqlQuery checkpointIdQuery(db);
        checkpointIdQuery.prepare("SELECT segment_id FROM durations WHERE is_finalized = 0");
        QVERIFY(checkpointIdQuery.exec());
        QVERIFY(checkpointIdQuery.next());
        QCOMPARE(checkpointIdQuery.value(0).toString(), oldCheckpointSegmentId.toString());

        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void HistoryDialogTest::test_historydialog_pauses_checkpoint_timer_for_dialog_lifetime()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    SqliteSessionStore db2(settings);
    Timer tracker(settings, db2);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(tracker.isDialogOpen_dbg());
        QVERIFY(!tracker.isCheckpointTimerActive_dbg());

        tracker.triggerCheckpoint_dbg();

        const QString connName = "historydialog_t6_case2_open";
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
            db.setDatabaseName(db_path_);
            QVERIFY(db.open());

            QSqlQuery countQuery(db);
            QVERIFY(countQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
            QVERIFY(countQuery.next());
            QCOMPARE(countQuery.value(0).toInt(), 0);

            db.close();
        }
        QSqlDatabase::removeDatabase(connName);
    }

    QVERIFY(!tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    tracker.triggerCheckpoint_dbg();

    const QString connName = "historydialog_t6_case2_closed";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());

        QSqlQuery countQuery(db);
        QVERIFY(countQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
        QVERIFY(countQuery.next());
        QCOMPARE(countQuery.value(0).toInt(), 1);

        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void HistoryDialogTest::test_historydialog_save_keeps_db_rows_for_history_plus_current_session()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDateTime historicalStart(QDate::currentDate().addDays(-1), QTime(9, 0, 0));
    const QDateTime historicalEnd = historicalStart.addSecs(60);
    std::deque<TimeDuration> historicalRows;
    historicalRows.emplace_back(DurationType::Pause, historicalStart, historicalEnd);
    QVERIFY(tracker.replaceAll(Timeline(historicalRows, std::nullopt), Timeline({}, std::nullopt)));

    const QDateTime now = QDateTime::currentDateTime();
    tracker.sessionState_dbg().durations.push_back(TimeDuration(DurationType::Activity, now.addSecs(-30), now.addSecs(-20)));
    tracker.sessionState_dbg().durations.push_back(TimeDuration(DurationType::Pause, now.addSecs(-20), now.addSecs(-10)));

    const int expectedHistoryRows = 1;
    const int expectedCurrentSessionRows = 2;

    HistoryDialog dialog(tracker, settings);
    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    const QString connName = "historydialog_t7_row_count";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());

        QSqlQuery totalQuery(db);
        QVERIFY(totalQuery.exec("SELECT COUNT(*) FROM durations"));
        QVERIFY(totalQuery.next());
        QCOMPARE(totalQuery.value(0).toInt(), expectedHistoryRows + expectedCurrentSessionRows);

        QSqlQuery finalizedQuery(db);
        QVERIFY(finalizedQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 1"));
        QVERIFY(finalizedQuery.next());
        QCOMPARE(finalizedQuery.value(0).toInt(), expectedHistoryRows);

        QSqlQuery unfinalizedQuery(db);
        QVERIFY(unfinalizedQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
        QVERIFY(unfinalizedQuery.next());
        QCOMPARE(unfinalizedQuery.value(0).toInt(), expectedCurrentSessionRows);

        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void HistoryDialogTest::test_historydialog_save_then_crash_reopen_retains_current_segment_row()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString settingsPath = createSettingsFile(tempDir.path(), 7);

    {
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

        const QDateTime now = QDateTime::currentDateTime();
        tracker.sessionState_dbg().durations.push_back(TimeDuration(DurationType::Activity, now.addSecs(-15), now.addSecs(-5)));

        HistoryDialog dialog(tracker, settings);
        dialog.done(QDialog::Accepted);
        dialog.saveChanges();

        // Simulate crash: skip graceful stop/finalize.
        tracker.forceMode_dbg(Timer::Mode::None);
        tracker.sessionState_dbg().segment_id = SegmentId{};
    }

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);

        QVERIFY(db.ensureOpen_dbg());
        QSqlQuery unfinalizedQuery(db.rawDb_dbg());
        QVERIFY(unfinalizedQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
        QVERIFY(unfinalizedQuery.next());
        QCOMPARE(unfinalizedQuery.value(0).toInt(), 1);
        db.lazyClose_dbg();

        SqliteSessionStore db3(settings);
        Timer reopened(settings, db3);
        reopened.initializeFromStore();
        auto loaded = db.loadDurations();
        QCOMPARE(loaded.size(), static_cast<size_t>(1));
    }
}

// Issue 8: saveChanges() must refresh the ongoing end-time from the engine,
// not preserve the stale snapshot captured at dialog-open time.
void HistoryDialogTest::test_historydialog_save_uses_ongoing_snapshot_endtime_after_wait()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(30);

    HistoryDialog dialog(tracker, settings);
    QVERIFY(dialog.editSession_dbg().pendingTimelines()[0].ongoing().has_value());
    const QDateTime snapshotEnd = dialog.editSession_dbg().pendingTimelines()[0].ongoing()->endTime;
    const QDateTime snapshotStart = dialog.editSession_dbg().pendingTimelines()[0].ongoing()->startTime;

    // Wait so the engine's ongoing end-time advances past snapshotEnd
    QTest::qWait(200);

    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    const QString connName = "historydialog_t9_snapshot_endtime";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());

        QSqlQuery ongoingQuery(db);
        ongoingQuery.prepare(
            "SELECT start_utc, end_utc FROM durations WHERE is_finalized = 0"
        );
        QVERIFY(ongoingQuery.exec());
        QVERIFY(ongoingQuery.next());

        const QDateTime savedStartUtc = QDateTime::fromString(ongoingQuery.value(0).toString(), Qt::ISODateWithMs);
        const QDateTime savedEndUtc   = QDateTime::fromString(ongoingQuery.value(1).toString(), Qt::ISODateWithMs);

        // Start time is preserved exactly
        QCOMPARE(savedStartUtc, snapshotStart.toUTC());

        // End-time must be strictly > snapshotEnd (refreshed, not stale)
        QVERIFY2(savedEndUtc > snapshotEnd.toUTC(),
                 "savedEndUtc must be > snapshotEnd: end-time was not refreshed");

        QVERIFY(!ongoingQuery.next());
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void HistoryDialogTest::test_historydialog_save_failed_db_replace_keeps_runtime_state_unchanged()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime memStart = now.addSecs(-30);
    const QDateTime memEnd = now.addSecs(-20);
    const QDateTime dbStart = now.addSecs(-19);
    const QDateTime dbEnd = now.addSecs(-10);

    tracker.sessionState_dbg().durations.push_back(TimeDuration(DurationType::Activity, memStart, memEnd));
    const SegmentId checkpointSegmentBeforeSave = SegmentId::fromString("checkpoint-before-save");
    const QDateTime checkpointStartBeforeSave = now.addSecs(-5);
    tracker.sessionState_dbg().segment_id = checkpointSegmentBeforeSave;
    tracker.sessionState_dbg().segment_start_time = checkpointStartBeforeSave;

    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Pause, dbStart, dbEnd);
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)));

    HistoryDialog dialog(tracker, settings);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(2));
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].segment_id.toString()), true);
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[1].segment_id.toString()), false);

    dialog.editSession_dbg().pendingTimelines()[0] = dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause);

    const QString lockConnName = "historydialog_save_failure_lock";
    {
        QSqlDatabase lockDb = QSqlDatabase::addDatabase("QSQLITE", lockConnName);
        lockDb.setDatabaseName(db_path_);
        QVERIFY(lockDb.open());

        QSqlQuery lockQuery(lockDb);
        QVERIFY(lockQuery.exec("BEGIN EXCLUSIVE TRANSACTION"));

        QTimer::singleShot(0, []() {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* messageBox = qobject_cast<QMessageBox*>(widget);
                if (!messageBox) {
                    continue;
                }
                if (messageBox->windowTitle() == "Database Error") {
                    messageBox->accept();
                    return;
                }
            }
        });

        dialog.done(QDialog::Accepted);
        dialog.saveChanges();

        QVERIFY(lockQuery.exec("ROLLBACK"));
        lockDb.close();
    }
    QSqlDatabase::removeDatabase(lockConnName);

    QCOMPARE(tracker.sessionState_dbg().durations.size(), static_cast<size_t>(1));
    QCOMPARE(tracker.sessionState_dbg().durations[0].type, DurationType::Activity);
    QCOMPARE(tracker.sessionState_dbg().durations[0].startTime, memStart);
    QCOMPARE(tracker.sessionState_dbg().durations[0].endTime, memEnd);
    QCOMPARE(tracker.sessionState_dbg().segment_id.toString(), checkpointSegmentBeforeSave.toString());
    QCOMPARE(tracker.sessionState_dbg().segment_start_time, checkpointStartBeforeSave);

    const QString verifyConnName = "historydialog_save_failure_verify";
    {
        QSqlDatabase verifyDb = QSqlDatabase::addDatabase("QSQLITE", verifyConnName);
        verifyDb.setDatabaseName(db_path_);
        QVERIFY(verifyDb.open());

        QSqlQuery verifyQuery(verifyDb);
        QVERIFY(verifyQuery.exec("SELECT type, start_utc, end_utc FROM durations ORDER BY id"));
        QVERIFY(verifyQuery.next());
        QCOMPARE(verifyQuery.value(0).toInt(), static_cast<int>(DurationType::Pause));

        const QDateTime savedStartUtc = QDateTime::fromString(verifyQuery.value(1).toString(), Qt::ISODateWithMs);
        const QDateTime savedEndUtc   = QDateTime::fromString(verifyQuery.value(2).toString(), Qt::ISODateWithMs);

        QCOMPARE(savedStartUtc, dbStart.toUTC());
        QCOMPARE(savedEndUtc, dbEnd.toUTC());
        QVERIFY(!verifyQuery.next());

        verifyDb.close();
    }
    QSqlDatabase::removeDatabase(verifyConnName);
}

void HistoryDialogTest::test_splitdialog_default_types_and_bounds()
{
    QDateTime start = makeTime(0);
    QDateTime end = makeTime(10 * 1000);
    SplitDialog dialog(start, end);

    QCOMPARE(dialog.getFirstSegmentType(), DurationType::Pause);
    QCOMPARE(dialog.getSecondSegmentType(), DurationType::Activity);
    QCOMPARE(dialog.slider_dbg()->minimum(), 1);
    QCOMPARE(dialog.slider_dbg()->maximum(), 9);
    QCOMPARE(dialog.getSplitTime(), start.addSecs(dialog.slider_dbg()->value()));
}

void HistoryDialogTest::test_splitdialog_short_duration_disables_slider()
{
    QDateTime start = makeTime(0);
    QDateTime end = makeTime(2 * 1000);
    SplitDialog dialog(start, end);

    QCOMPARE(dialog.slider_dbg()->minimum(), 1);
    QCOMPARE(dialog.slider_dbg()->maximum(), 1);
    QCOMPARE(dialog.slider_dbg()->value(), 1);
    QVERIFY(!dialog.slider_dbg()->isEnabled());
}

void HistoryDialogTest::test_splitdialog_minimum_duration_boundaries()
{
    {
        QDateTime start = makeTime(0);
        QDateTime end = makeTime(2 * 1000);
        SplitDialog dialog(start, end);
        QVERIFY(!dialog.slider_dbg()->isEnabled());
        QCOMPARE(dialog.slider_dbg()->minimum(), 1);
        QCOMPARE(dialog.slider_dbg()->maximum(), 1);
    }

    {
        QDateTime start = makeTime(0);
        QDateTime end = makeTime(3 * 1000);
        SplitDialog dialog(start, end);
        QVERIFY(dialog.slider_dbg()->isEnabled());
        QCOMPARE(dialog.slider_dbg()->minimum(), 1);
        QCOMPARE(dialog.slider_dbg()->maximum(), 2);
    }

    {
        QDateTime start = makeTime(0);
        QDateTime end = makeTime(4 * 1000);
        SplitDialog dialog(start, end);
        QVERIFY(dialog.slider_dbg()->isEnabled());
        QCOMPARE(dialog.slider_dbg()->minimum(), 1);
        QCOMPARE(dialog.slider_dbg()->maximum(), 3);
    }
}

void HistoryDialogTest::test_splitdialog_setters_affect_types()
{
    QDateTime start = makeTime(0);
    QDateTime end = makeTime(8 * 1000);
    SplitDialog dialog(start, end);

    dialog.setFirstSegmentType(DurationType::Activity);
    QCOMPARE(dialog.getFirstSegmentType(), DurationType::Activity);
    QCOMPARE(dialog.getSecondSegmentType(), DurationType::Pause);

    dialog.setSecondSegmentType(DurationType::Activity);
    QCOMPARE(dialog.getFirstSegmentType(), DurationType::Pause);
    QCOMPARE(dialog.getSecondSegmentType(), DurationType::Activity);
}

// ============================================================================
// Test R — round-trip type toggle via accept updates Timer
// ============================================================================

void HistoryDialogTest::test_R_round_trip_type_toggle_via_accept()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDateTime now = QDateTime::currentDateTime();
    tracker.sessionState_dbg().durations.push_back(
        TimeDuration(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    {
        HistoryDialog dialog(tracker, settings);
        QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].type, DurationType::Activity);

        // Toggle to Pause via Timeline API
        dialog.editSession_dbg().pendingTimelines()[0] = dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause);
        QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].type, DurationType::Pause);

        dialog.done(QDialog::Accepted);
        dialog.saveChanges();
    }

    // Timer reflects the change
    QCOMPARE(tracker.getCurrentDurations().completed().size(), static_cast<size_t>(1));
    QCOMPARE(tracker.getCurrentDurations().completed()[0].type, DurationType::Pause);
}

// ============================================================================
// Test S — cancel preserves state
// ============================================================================

void HistoryDialogTest::test_S_cancel_preserves_state()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDateTime now = QDateTime::currentDateTime();
    tracker.sessionState_dbg().durations.push_back(
        TimeDuration(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    {
        HistoryDialog dialog(tracker, settings);
        // Toggle to Pause but cancel
        dialog.editSession_dbg().pendingTimelines()[0] = dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause);

        dialog.done(QDialog::Rejected);
        dialog.saveChanges();  // saveChanges checks result() and returns early when Rejected
    }

    // Timer is unchanged — still Activity
    QCOMPARE(tracker.getCurrentDurations().completed().size(), static_cast<size_t>(1));
    QCOMPARE(tracker.getCurrentDurations().completed()[0].type, DurationType::Activity);
}

void HistoryDialogTest::test_saveChanges_deduplicates_cross_bucket_overlaps()
{
    // DB history row (today): Activity 10:00-10:30
    // In-memory session row (today): Activity 10:15-10:45
    // After save+normalize, expect exactly one Activity row covering 10:00-10:45.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDate today = QDate::currentDate();
    const QDateTime dbStart(today, QTime(10, 0, 0), Qt::UTC);
    const QDateTime dbEnd(today, QTime(10, 30, 0), Qt::UTC);
    const QDateTime memStart(today, QTime(10, 15, 0), Qt::UTC);
    const QDateTime memEnd(today, QTime(10, 45, 0), Qt::UTC);

    // Seed DB with today's finalized history row
    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Activity, dbStart, dbEnd);
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)));

    // Seed in-memory session row (overlaps the DB row)
    tracker.sessionState_dbg().durations.push_back(TimeDuration(DurationType::Activity, memStart, memEnd));

    HistoryDialog dialog(tracker, settings);
    dialog.done(QDialog::Accepted);

    // H4: confirm the overlap-merge dialog
    QTimer::singleShot(0, []() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* mb = qobject_cast<QMessageBox*>(widget);
            if (mb && mb->windowTitle() == "Overlapping Segments") {
                mb->button(QMessageBox::Yes)->click();
                return;
            }
        }
    });
    dialog.saveChanges();

    // Read back DB and verify exactly one Activity row covering 10:00-10:45
    const QString connName = "historydialog_cross_bucket_dedup";
    {
        QSqlDatabase sqlDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        sqlDb.setDatabaseName(db_path_);
        QVERIFY(sqlDb.open());

        QSqlQuery countQuery(sqlDb);
        QVERIFY(countQuery.exec("SELECT COUNT(*) FROM durations WHERE type = " +
            QString::number(static_cast<int>(DurationType::Activity))));
        QVERIFY(countQuery.next());
        QCOMPARE(countQuery.value(0).toInt(), 1);

        QSqlQuery rowQuery(sqlDb);
        QVERIFY(rowQuery.exec("SELECT start_utc, end_utc FROM durations WHERE type = " +
            QString::number(static_cast<int>(DurationType::Activity))));
        QVERIFY(rowQuery.next());
        const QDateTime savedStart = QDateTime::fromString(rowQuery.value(0).toString(), Qt::ISODateWithMs);
        const QDateTime savedEnd   = QDateTime::fromString(rowQuery.value(1).toString(), Qt::ISODateWithMs);
        QCOMPARE(savedStart.toUTC().time(), QTime(10, 0, 0));
        QCOMPARE(savedEnd.toUTC().time(), QTime(10, 45, 0));

        // The merged row absorbed current-session time, so it must be written as unfinalized
        // (current-session) in the DB — accepted side effect documented in plan01.md #6.
        QSqlQuery unfinalizedQuery(sqlDb);
        QVERIFY(unfinalizedQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0 AND type = " +
            QString::number(static_cast<int>(DurationType::Activity))));
        QVERIFY(unfinalizedQuery.next());
        QCOMPARE(unfinalizedQuery.value(0).toInt(), 1);

        sqlDb.close();
    }
    QSqlDatabase::removeDatabase(connName);

    // The merged 10:00-10:45 row must be retained in the live session after save.
    // Before fix #6: session loses the row entirely (bug — current-session time drops to 0).
    // After fix #6: session contains exactly the merged row (inflated from 30 min to 45 min).
    QCOMPARE(tracker.sessionState_dbg().durations.size(), static_cast<size_t>(1));
    QCOMPARE(tracker.sessionState_dbg().durations[0].type, DurationType::Activity);
    QCOMPARE(tracker.sessionState_dbg().durations[0].startTime.toUTC().time(), QTime(10, 0, 0));
    QCOMPARE(tracker.sessionState_dbg().durations[0].endTime.toUTC().time(), QTime(10, 45, 0));
}

void HistoryDialogTest::test_saveChanges_noop_save_unchanged()
{
    // Regression: non-overlapping data should be preserved exactly after a noop save.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDate today = QDate::currentDate();
    const QDateTime dbStart(today, QTime(8, 0, 0), Qt::UTC);
    const QDateTime dbEnd(today, QTime(8, 30, 0), Qt::UTC);
    const QDateTime memStart(today, QTime(9, 0, 0), Qt::UTC);
    const QDateTime memEnd(today, QTime(9, 30, 0), Qt::UTC);

    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Pause, dbStart, dbEnd);
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)));

    tracker.sessionState_dbg().durations.push_back(TimeDuration(DurationType::Activity, memStart, memEnd));

    HistoryDialog dialog(tracker, settings);
    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    // Both rows should still be present
    const QString connName = "historydialog_noop_save";
    {
        QSqlDatabase sqlDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        sqlDb.setDatabaseName(db_path_);
        QVERIFY(sqlDb.open());

        QSqlQuery countQuery(sqlDb);
        QVERIFY(countQuery.exec("SELECT COUNT(*) FROM durations"));
        QVERIFY(countQuery.next());
        QCOMPARE(countQuery.value(0).toInt(), 2);

        QSqlQuery pauseQuery(sqlDb);
        QVERIFY(pauseQuery.exec("SELECT start_utc, end_utc FROM durations WHERE type = " +
            QString::number(static_cast<int>(DurationType::Pause))));
        QVERIFY(pauseQuery.next());
        QCOMPARE(QDateTime::fromString(pauseQuery.value(0).toString(), Qt::ISODateWithMs).toUTC().time(), QTime(8, 0, 0));
        QCOMPARE(QDateTime::fromString(pauseQuery.value(1).toString(), Qt::ISODateWithMs).toUTC().time(), QTime(8, 30, 0));

        QSqlQuery actQuery(sqlDb);
        QVERIFY(actQuery.exec("SELECT start_utc, end_utc FROM durations WHERE type = " +
            QString::number(static_cast<int>(DurationType::Activity))));
        QVERIFY(actQuery.next());
        QCOMPARE(QDateTime::fromString(actQuery.value(0).toString(), Qt::ISODateWithMs).toUTC().time(), QTime(9, 0, 0));
        QCOMPARE(QDateTime::fromString(actQuery.value(1).toString(), Qt::ISODateWithMs).toUTC().time(), QTime(9, 30, 0));

        sqlDb.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void HistoryDialogTest::test_H3_ongoing_type_edit_preserved_when_engine_stops()
{
    // H3 bug scenario: engine is still running when saveChanges() is called.
    // Without the ongoingRowModified_ guard, saveChanges() would refresh the
    // ongoing end-time from the engine and overwrite the user's Pause edit back
    // to Activity (the engine's type). With the guard, the user's edit wins.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    HistoryDialog dialog(tracker, settings);
    QVERIFY(dialog.editSession_dbg().pendingTimelines()[0].ongoing().has_value());
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].ongoing()->type, DurationType::Activity);

    // User edits ongoing type to Pause
    auto ongoing = dialog.editSession_dbg().pendingTimelines()[0].ongoing().value();
    ongoing.type = DurationType::Pause;
    dialog.editSession_dbg().pendingTimelines()[0] = Timeline(dialog.editSession_dbg().pendingTimelines()[0].completed(), ongoing);
    dialog.editSession_dbg().markOngoingModified();

    // Engine is still running (Activity mode) — saveChanges() would overwrite type to Activity
    // without the H3 guard (ongoingRowModified_ == true).
    QVERIFY(tracker.isActive());

    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    // H3: user's Pause type is preserved in the DB (unfinalized row), not overwritten
    // by the engine's Activity type refresh.
    const QString connName = "historydialog_h3_check";
    {
        QSqlDatabase sqlDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        sqlDb.setDatabaseName(db_path_);
        QVERIFY(sqlDb.open());
        QSqlQuery q(sqlDb);
        QVERIFY(q.exec("SELECT type FROM durations WHERE is_finalized = 0"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), static_cast<int>(DurationType::Pause));
        sqlDb.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void HistoryDialogTest::test_H4_overlap_cancel_aborts_save()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDate today = QDate::currentDate();
    const QDateTime dbStart(today, QTime(11, 0, 0), Qt::UTC);
    const QDateTime dbEnd(today, QTime(11, 30, 0), Qt::UTC);
    const QDateTime memStart(today, QTime(11, 15, 0), Qt::UTC);
    const QDateTime memEnd(today, QTime(11, 45, 0), Qt::UTC);

    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Activity, dbStart, dbEnd);
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)));
    tracker.sessionState_dbg().durations.push_back(TimeDuration(DurationType::Activity, memStart, memEnd));

    HistoryDialog dialog(tracker, settings);

    QTimer::singleShot(0, []() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* mb = qobject_cast<QMessageBox*>(widget);
            if (mb && mb->windowTitle() == "Overlapping Segments") {
                mb->button(QMessageBox::No)->click();
                return;
            }
        }
    });

    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    // H4: save aborted — DB should still have exactly 1 finalized row (original)
    const QString connName = "historydialog_h4_cancel";
    {
        QSqlDatabase sqlDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        sqlDb.setDatabaseName(db_path_);
        QVERIFY(sqlDb.open());
        QSqlQuery q(sqlDb);
        QVERIFY(q.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 1"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);
        sqlDb.close();
    }
    QSqlDatabase::removeDatabase(connName);

    // Timer session unchanged: still has 1 memory row
    QCOMPARE(tracker.sessionState_dbg().durations.size(), static_cast<size_t>(1));
}

void HistoryDialogTest::test_H2_midnight_crossing_shown_on_both_pages()
{
    // A cross-midnight segment (starts 23:30 yesterday, ends 00:30 today) must appear
    // on BOTH day pages. The start-day (yesterday) page holds the canonical (editable)
    // copy; the end-day (today) page holds a display-only continuation copy.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDate yesterday = QDate::currentDate().addDays(-1);
    const QDateTime segStart(yesterday, QTime(23, 30, 0));
    const QDateTime segEnd = segStart.addSecs(3600); // ends at 00:30 today

    // Insert cross-midnight segment directly via SQL.
    {
        const QString connName = "h2_test_setup";
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(db_path_);
        // DB may not exist yet — seed it via replaceAll first.
        rawDb.close();
        rawDb = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
    }
    // Seed DB to create the schema.
    QVERIFY(tracker.replaceAll(Timeline({}, std::nullopt), Timeline({}, std::nullopt)));
    {
        const QString connName = "h2_insert";
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(db_path_);
        QVERIFY(rawDb.open());
        QSqlQuery q(rawDb);
        q.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, :s, :e, 1)"
        );
        q.bindValue(":sid", TimeDuration::createSegmentId().toString());
        q.bindValue(":s", segStart.toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":e", segEnd.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
        rawDb.close();
        rawDb = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
    }

    HistoryDialog dialog(tracker, settings);

    // Two pages: today (isCurrent) and yesterday.
    QCOMPARE(dialog.editSession_dbg().pages().size(), static_cast<size_t>(2));
    QCOMPARE(dialog.editSession_dbg().pages()[0].isCurrent, true);
    QCOMPARE(dialog.editSession_dbg().pages()[1].isCurrent, false);

    // Yesterday page: cross-midnight row is in crossMidnight (display-only canonical),
    // not in durations (which holds only same-day editable rows).
    QCOMPARE(dialog.editSession_dbg().pages()[1].durations.size(), static_cast<size_t>(0));
    QCOMPARE(dialog.editSession_dbg().pages()[1].crossMidnight.size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().pages()[1].crossMidnight[0].startTime.date(), yesterday);
    QCOMPARE(dialog.editSession_dbg().pages()[1].continuations.size(), static_cast<size_t>(0));

    // Today page: continuation copy (display-only).
    QCOMPARE(dialog.editSession_dbg().pages()[0].continuations.size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().pages()[0].continuations[0].startTime.date(), yesterday);
    QCOMPARE(dialog.editSession_dbg().pages()[0].continuations[0].endTime.date(), QDate::currentDate());
    QCOMPARE(dialog.editSession_dbg().pages()[0].crossMidnight.size(), static_cast<size_t>(0));

    // Both show the same segment_id.
    QCOMPARE(dialog.editSession_dbg().pages()[0].continuations[0].segment_id,
             dialog.editSession_dbg().pages()[1].crossMidnight[0].segment_id);

    // Cross-midnight row is NOT in pendingTimelines_ (Timeline rejects cross-midnight).
    // It is preserved in crossMidnightRows_ for saving.
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(0));
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed().size(), static_cast<size_t>(0));
    QCOMPARE(dialog.editSession_dbg().crossMidnightRows().size(), static_cast<size_t>(1));
}

void HistoryDialogTest::test_H14_cross_midnight_totals_and_counts()
{
    // Seed a finalized cross-midnight Activity row: 23:50 yesterday → 00:10 today (20 min total).
    // Start-day (yesterday) page should show 10 min Activity in totals and count the row.
    // End-day (today) page (continuation) should show 10 min Activity in totals.
    // Both portions together sum to 20 min — no double-counting.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDate yesterday = QDate::currentDate().addDays(-1);
    const QDateTime segStart(yesterday, QTime(23, 50, 0), Qt::LocalTime);
    const QDateTime segEnd(QDate::currentDate(), QTime(0, 10, 0), Qt::LocalTime); // 20 min total

    // Seed DB schema, then insert cross-midnight row directly via SQL.
    QVERIFY(tracker.replaceAll(Timeline({}, std::nullopt), Timeline({}, std::nullopt)));
    {
        const QString connName = "h14_insert";
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(db_path_);
        QVERIFY(rawDb.open());
        QSqlQuery q(rawDb);
        q.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, :s, :e, 1)"
        );
        q.bindValue(":sid", TimeDuration::createSegmentId().toString());
        q.bindValue(":s", segStart.toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":e", segEnd.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
        rawDb.close();
        rawDb = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
    }

    HistoryDialog dialog(tracker, settings);

    // Two pages: today (isCurrent, index 0) and yesterday (index 1).
    QCOMPARE(dialog.editSession_dbg().pages().size(), static_cast<size_t>(2));
    QCOMPARE(dialog.editSession_dbg().pages()[0].isCurrent, true);
    QCOMPARE(dialog.editSession_dbg().pages()[1].isCurrent, false);

    // Yesterday page: has the crossMidnight row (display-only) — count must be 1, not 0.
    QCOMPARE(dialog.editSession_dbg().pages()[1].crossMidnight.size(), static_cast<size_t>(1));
    QVERIFY(dialog.editSession_dbg().pages()[1].title.contains("entries: 1"));

    // Today page: has the continuation row — count must be 1, not 0.
    QCOMPARE(dialog.editSession_dbg().pages()[0].continuations.size(), static_cast<size_t>(1));
    QVERIFY(dialog.editSession_dbg().pages()[0].title.contains("entries: 1"));

    // Today page is page 0 — pageLabel_ already shows its totals.
    // The continuation is midnight→00:10 (10 min = 600 s = 00:10:00 Activity).
    dialog.show();
    const QString todayLabel = dialog.pageLabel_dbg()->text();
    QVERIFY2(todayLabel.contains("00:10:00"),
             qPrintable("Today page label should contain 00:10:00 but got: " + todayLabel));

    // Navigate to yesterday's page and verify its totals.
    dialog.updateTotalsLabel_dbg(1);
    const QString yesterdayLabel = dialog.pageLabel_dbg()->text();
    QVERIFY2(yesterdayLabel.contains("00:10:00"),
             qPrintable("Yesterday page label should contain 00:10:00 but got: " + yesterdayLabel));

    // The two portions together must sum to the full 20-minute span.
    // Extract activity from each page's pending + display-only totals.
    // Today: continuation portion = 00:10:00, Yesterday: crossMidnight portion = 00:10:00.
    // Neither page should show 00:00:00 Activity (which was the pre-fix bug).
    QVERIFY(!todayLabel.contains("Activity: 00:00:00"));
    QVERIFY(!yesterdayLabel.contains("Activity: 00:00:00"));
}

void HistoryDialogTest::test_H6_split_dialog_preset_from_source_row_type()
{
    // H6: onSplitRow() presets SplitDialog's first-segment type to match the
    // source row's type. An Activity source row presets Activity→Pause;
    // a Pause source row presets Pause→Activity.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDateTime now = QDateTime::currentDateTime();
    // Source row is Pause
    tracker.sessionState_dbg().durations.push_back(
        TimeDuration(DurationType::Pause, now.addSecs(-10), now.addSecs(-4)));

    HistoryDialog dialog(tracker, settings);
    dialog.setContextMenuTarget_dbg(0, 0);

    DurationType capturedFirst = DurationType::Activity; // will be overwritten
    QTimer::singleShot(0, [&capturedFirst]() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* split = qobject_cast<SplitDialog*>(widget);
            if (!split) continue;
            capturedFirst = split->getFirstSegmentType();
            split->accept();
            return;
        }
    });

    dialog.onSplitRow_dbg();

    // The dialog should have been preset to Pause (matching the source row).
    QCOMPARE(capturedFirst, DurationType::Pause);
    // Split should have produced 2 rows
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(2));
}
