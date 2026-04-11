#include "test_historydialog.h"
#include <QtTest>
#include <QTimer>
#include <QTableWidget>
#include <QCheckBox>
#include <QLabel>

// Expose private members for testing
#define private public
#define protected public
#include "../historydialog.h"
#undef private
#undef protected

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
    TimeTracker tracker(settings);

    QDateTime now = QDateTime::currentDateTime();
    tracker.durations_.push_back(TimeDuration(DurationType::Activity, now.addSecs(-120), now.addSecs(-60)));

    // Save a DB entry for today
    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Pause, now.addSecs(-50), now.addSecs(-40));
    QVERIFY(tracker.replaceDurationsInDB(dbDurations, {}));

    // Start ongoing activity
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(5);

    HistoryDialog dialog(tracker, settings);
    QCOMPARE(dialog.pages_.size(), static_cast<size_t>(1));
    QCOMPARE(dialog.pages_[0].isCurrent, true);
    QCOMPARE(dialog.pendingChanges_[0].size(), static_cast<size_t>(2));
    QCOMPARE(dialog.rowOrigins_[0].size(), static_cast<size_t>(2));
    QVERIFY(dialog.pendingChanges_[0].back().duration > 0);
    QCOMPARE(dialog.rowOrigins_[0].back(), HistoryDialog::RowOrigin::Ongoing);
}

void HistoryDialogTest::test_historydialog_createPages_dedups_db_row_with_small_time_drift()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime memoryStart = now.addSecs(-40).addMSecs(2);
    const QDateTime memoryEnd = now.addSecs(-10);
    tracker.durations_.push_back(TimeDuration(DurationType::Activity, memoryStart, memoryEnd));

    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Activity, memoryStart.addMSecs(-2), memoryEnd);
    QVERIFY(tracker.replaceDurationsInDB(dbDurations, {}));

    HistoryDialog dialog(tracker, settings);
    QCOMPARE(dialog.pages_.size(), static_cast<size_t>(1));
    QCOMPARE(dialog.pendingChanges_[0].size(), static_cast<size_t>(1));
    QCOMPARE(dialog.rowOrigins_[0].size(), static_cast<size_t>(1));
    QCOMPARE(dialog.rowOrigins_[0][0], HistoryDialog::RowOrigin::CurrentMemory);
    QCOMPARE(dialog.pendingChanges_[0][0].startTime, memoryStart);
    QCOMPARE(dialog.pendingChanges_[0][0].endTime, memoryEnd);
}

void HistoryDialogTest::test_historydialog_createPages_groups_unsplit_cross_midnight_row_by_start_date()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

    const QDate startDate = QDate::currentDate().addDays(-1);
    const QDateTime crossMidnightStart(startDate, QTime(23, 59, 50));
    const QDateTime crossMidnightEnd = crossMidnightStart.addSecs(20);

    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Activity, crossMidnightStart, crossMidnightEnd);
    QVERIFY(tracker.replaceDurationsInDB(dbDurations, {}));

    HistoryDialog dialog(tracker, settings);

    QCOMPARE(dialog.pages_.size(), static_cast<size_t>(2));
    QCOMPARE(dialog.pages_[0].isCurrent, true);
    QCOMPARE(dialog.pages_[0].durations.size(), static_cast<size_t>(0));

    QCOMPARE(dialog.pages_[1].isCurrent, false);
    QCOMPARE(dialog.pages_[1].durations.size(), static_cast<size_t>(1));
    QCOMPARE(dialog.pages_[1].durations[0].startTime, crossMidnightStart);
    QCOMPARE(dialog.pages_[1].durations[0].endTime, crossMidnightEnd);
    QVERIFY(dialog.pages_[1].title.startsWith(startDate.toString("yyyy-MM-dd")));
}

void HistoryDialogTest::test_historydialog_checkbox_toggle_updates_pending_and_totals()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

    QDateTime now = QDateTime::currentDateTime();
    tracker.durations_.push_back(TimeDuration(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    HistoryDialog dialog(tracker, settings);
    dialog.show();

    QCheckBox* box = qobject_cast<QCheckBox*>(dialog.table_->cellWidget(0, 3));
    QVERIFY(box != nullptr);
    QCOMPARE(dialog.pendingChanges_[0][0].type, DurationType::Activity);
    box->setChecked(false);

    QCOMPARE(dialog.pendingChanges_[0][0].type, DurationType::Pause);
    QVERIFY(dialog.pageLabel_->text().contains("Pause"));
}

void HistoryDialogTest::test_historydialog_saveChanges_updates_timetracker_and_db()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

    QDateTime now = QDateTime::currentDateTime();
    tracker.durations_.push_back(TimeDuration(DurationType::Activity, now.addSecs(-200), now.addSecs(-150)));

    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Pause, now.addSecs(-140), now.addSecs(-120));
    QVERIFY(tracker.replaceDurationsInDB(dbDurations, {}));

    HistoryDialog dialog(tracker, settings);
    dialog.pendingChanges_[0][0].type = DurationType::Pause;

    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    QCOMPARE(tracker.durations_.size(), static_cast<size_t>(1));
    QCOMPARE(tracker.durations_[0].type, DurationType::Pause);

    DatabaseManager db(settings);
    auto loaded = db.loadDurations();
    QVERIFY(loaded.size() >= 1);
}

void HistoryDialogTest::test_historydialog_split_action_splits_row()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

    QDateTime start = QDateTime::currentDateTime().addSecs(-10);
    QDateTime end = QDateTime::currentDateTime().addSecs(-4);
    tracker.durations_.push_back(TimeDuration(DurationType::Activity, start, end));

    HistoryDialog dialog(tracker, settings);
    dialog.contextMenuRow_ = 0;
    dialog.contextMenuPage_ = 0;

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

    dialog.onSplitRow();
    QCOMPARE(dialog.pendingChanges_[0].size(), static_cast<size_t>(2));
    const auto& first = dialog.pendingChanges_[0][0];
    const auto& second = dialog.pendingChanges_[0][1];
    QCOMPARE(first.startTime, start);
    QCOMPARE(second.endTime, end);
    QCOMPARE(first.endTime, second.startTime);
    QCOMPARE(first.type, DurationType::Activity);
    QCOMPARE(second.type, DurationType::Pause);
}

void HistoryDialogTest::test_historydialog_split_today_mixed_origins_routes_to_correct_bucket()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime memStart = now.addSecs(-20);
    const QDateTime memEnd = now.addSecs(-12);
    const QDateTime dbStart = now.addSecs(-11);
    const QDateTime dbEnd = now.addSecs(-6);

    tracker.durations_.push_back(TimeDuration(DurationType::Activity, memStart, memEnd));

    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Pause, dbStart, dbEnd);
    QVERIFY(tracker.replaceDurationsInDB(dbDurations, {}));

    HistoryDialog dialog(tracker, settings);
    QCOMPARE(dialog.rowOrigins_[0].size(), static_cast<size_t>(2));
    QCOMPARE(dialog.rowOrigins_[0][0], HistoryDialog::RowOrigin::CurrentMemory);
    QCOMPARE(dialog.rowOrigins_[0][1], HistoryDialog::RowOrigin::CurrentDatabase);

    dialog.contextMenuRow_ = 0;
    dialog.contextMenuPage_ = 0;

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

    dialog.onSplitRow();
    QCOMPARE(dialog.pendingChanges_[0].size(), static_cast<size_t>(3));
    QCOMPARE(dialog.rowOrigins_[0].size(), static_cast<size_t>(3));
    QCOMPARE(dialog.rowOrigins_[0][0], HistoryDialog::RowOrigin::CurrentMemory);
    QCOMPARE(dialog.rowOrigins_[0][1], HistoryDialog::RowOrigin::CurrentMemory);
    QCOMPARE(dialog.rowOrigins_[0][2], HistoryDialog::RowOrigin::CurrentDatabase);

    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    QCOMPARE(tracker.durations_.size(), static_cast<size_t>(2));
    QCOMPARE(tracker.durations_[0].startTime, memStart);
    QCOMPARE(tracker.durations_[1].endTime, memEnd);
    QCOMPARE(tracker.durations_[0].endTime, tracker.durations_[1].startTime);

    DatabaseManager db(settings);
    auto loaded = db.loadDurations();
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
    TimeTracker tracker(settings);

    const QDateTime start(QDate::currentDate().addDays(-1), QTime(10, 0, 0));
    const QDateTime end = start.addSecs(8);

    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Activity, start, end);
    QVERIFY(tracker.replaceDurationsInDB(dbDurations, {}));

    HistoryDialog dialog(tracker, settings);
    QVERIFY(dialog.pages_.size() >= static_cast<size_t>(2));

    dialog.contextMenuRow_ = 0;
    dialog.contextMenuPage_ = 1;

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

    dialog.onSplitRow();
    QCOMPARE(dialog.pendingChanges_[1].size(), static_cast<size_t>(2));
    QCOMPARE(dialog.rowOrigins_[1].size(), static_cast<size_t>(2));
    QCOMPARE(dialog.rowOrigins_[1][0], HistoryDialog::RowOrigin::HistoricalDatabase);
    QCOMPARE(dialog.rowOrigins_[1][1], HistoryDialog::RowOrigin::HistoricalDatabase);

    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    DatabaseManager db(settings);
    auto loaded = db.loadDurations();
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
    TimeTracker tracker(settings);

    DatabaseManager manager(settings);
    QVERIFY(manager.lazyOpen());
    QSqlQuery query(manager.db);
    const QDateTime start = QDateTime::currentDateTimeUtc();
    const QDateTime end = start.addMSecs(1000);

    query.prepare("INSERT INTO durations (type, duration, start_date, start_time, end_date, end_time, is_finalized) "
                  "VALUES (99, 1000, :start_date, :start_time, :end_date, :end_time, 1)");
    query.bindValue(":start_date", start.date().toString(Qt::ISODate));
    query.bindValue(":start_time", start.time().toString("HH:mm:ss.zzz"));
    query.bindValue(":end_date", end.date().toString(Qt::ISODate));
    query.bindValue(":end_time", end.time().toString("HH:mm:ss.zzz"));
    QVERIFY(query.exec());

    query.prepare("INSERT INTO durations (type, duration, start_date, start_time, end_date, end_time, is_finalized) "
                  "VALUES (0, 1200, :start_date, :start_time, :end_date, :end_time, 1)");
    query.bindValue(":start_date", start.date().toString(Qt::ISODate));
    query.bindValue(":start_time", start.time().toString("HH:mm:ss.zzz"));
    query.bindValue(":end_date", end.date().toString(Qt::ISODate));
    query.bindValue(":end_time", end.time().toString("HH:mm:ss.zzz"));
    QVERIFY(query.exec());
    manager.lazyClose();

    HistoryDialog dialog(tracker, settings);
    const QString msg = dialog.getLoadReconciliationMessage();
    QCOMPARE(msg, QString("1 rows skipped due to corrupt data, 1 rows auto-repaired."));
    QVERIFY(dialog.loadReconciliationLabel_ != nullptr);
    QCOMPARE(dialog.loadReconciliationLabel_->text(), msg);
    QVERIFY(!dialog.loadReconciliationLabel_->isHidden());
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
    TimeTracker tracker(settings);

    const QDateTime historicalStart(QDate::currentDate().addDays(-1), QTime(10, 0, 0));
    const QDateTime historicalEnd = historicalStart.addSecs(300);
    std::deque<TimeDuration> historicalRows;
    historicalRows.emplace_back(DurationType::Pause, historicalStart, historicalEnd);
    QVERIFY(tracker.replaceDurationsInDB(historicalRows, {}));


    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    tracker.saveCheckpoint();
    const long long oldCheckpointId = tracker.current_checkpoint_id_;
    QVERIFY(oldCheckpointId != -1);

    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(dialog.pages_.size() >= static_cast<size_t>(2));
        dialog.pendingChanges_[1][0].type = DurationType::Activity;
        dialog.done(QDialog::Accepted);
        dialog.saveChanges();
    }

    QCOMPARE(tracker.current_checkpoint_id_ != -1, true);
    QVERIFY(tracker.current_checkpoint_id_ != oldCheckpointId);

    QTest::qWait(20);
    tracker.saveCheckpoint();

    const QString connName = "historydialog_t6_case1";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());

        QSqlQuery historicalQuery(db);
        historicalQuery.prepare(
            "SELECT type, duration FROM durations WHERE start_date = :start_date AND start_time = :start_time AND end_date = :end_date AND end_time = :end_time"
        );
        historicalQuery.bindValue(":start_date", historicalStart.toUTC().date().toString(Qt::ISODate));
        historicalQuery.bindValue(":start_time", historicalStart.toUTC().time().toString("HH:mm:ss.zzz"));
        historicalQuery.bindValue(":end_date", historicalEnd.toUTC().date().toString(Qt::ISODate));
        historicalQuery.bindValue(":end_time", historicalEnd.toUTC().time().toString("HH:mm:ss.zzz"));
        QVERIFY(historicalQuery.exec());
        QVERIFY(historicalQuery.next());
        QCOMPARE(historicalQuery.value(0).toInt(), static_cast<int>(DurationType::Activity));
        QCOMPARE(historicalQuery.value(1).toLongLong(), historicalStart.msecsTo(historicalEnd));

        QSqlQuery checkpointQuery(db);
        checkpointQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0");
        QVERIFY(checkpointQuery.next());
        QCOMPARE(checkpointQuery.value(0).toInt(), 1);

        QSqlQuery checkpointIdQuery(db);
        checkpointIdQuery.prepare("SELECT id FROM durations WHERE is_finalized = 0");
        QVERIFY(checkpointIdQuery.exec());
        QVERIFY(checkpointIdQuery.next());
        QVERIFY(checkpointIdQuery.value(0).toLongLong() != oldCheckpointId);

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
    TimeTracker tracker(settings);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    QVERIFY(tracker.checkpointTimer_.isActive());

    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(tracker.checkpoints_paused_);
        QVERIFY(!tracker.checkpointTimer_.isActive());

        tracker.saveCheckpoint();

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

    QVERIFY(!tracker.checkpoints_paused_);
    QVERIFY(tracker.checkpointTimer_.isActive());

    tracker.saveCheckpoint();

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
    TimeTracker tracker(settings);

    const QDateTime historicalStart(QDate::currentDate().addDays(-1), QTime(9, 0, 0));
    const QDateTime historicalEnd = historicalStart.addSecs(60);
    std::deque<TimeDuration> historicalRows;
    historicalRows.emplace_back(DurationType::Pause, historicalStart, historicalEnd);
    QVERIFY(tracker.replaceDurationsInDB(historicalRows, {}));

    const QDateTime now = QDateTime::currentDateTime();
    tracker.durations_.push_back(TimeDuration(DurationType::Activity, now.addSecs(-30), now.addSecs(-20)));
    tracker.durations_.push_back(TimeDuration(DurationType::Pause, now.addSecs(-20), now.addSecs(-10)));

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
        TimeTracker tracker(settings);

        const QDateTime now = QDateTime::currentDateTime();
        tracker.durations_.push_back(TimeDuration(DurationType::Activity, now.addSecs(-15), now.addSecs(-5)));

        HistoryDialog dialog(tracker, settings);
        dialog.done(QDialog::Accepted);
        dialog.saveChanges();

        // Simulate crash: skip graceful stop/finalize.
        tracker.mode_ = TimeTracker::Mode::None;
        tracker.current_checkpoint_id_ = -1;
    }

    {
        Settings settings(settingsPath);
        DatabaseManager db(settings);

        QVERIFY(db.lazyOpen());
        QSqlQuery unfinalizedQuery(db.db);
        QVERIFY(unfinalizedQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
        QVERIFY(unfinalizedQuery.next());
        QCOMPARE(unfinalizedQuery.value(0).toInt(), 1);
        db.lazyClose();

        TimeTracker reopened(settings);
        auto loaded = db.loadDurations();
        QCOMPARE(loaded.size(), static_cast<size_t>(1));
    }
}

void HistoryDialogTest::test_historydialog_save_uses_ongoing_snapshot_endtime_after_wait()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    TimeTracker tracker(settings);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(30);

    HistoryDialog dialog(tracker, settings);
    QVERIFY(dialog.ongoingSnapshot_.has_value());
    const QDateTime snapshotEnd = dialog.ongoingSnapshot_->endTime;
    const QDateTime snapshotStart = dialog.ongoingSnapshot_->startTime;

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
            "SELECT start_date, start_time, end_date, end_time, duration FROM durations WHERE is_finalized = 0"
        );
        QVERIFY(ongoingQuery.exec());
        QVERIFY(ongoingQuery.next());

        const QDate savedStartDate = QDate::fromString(ongoingQuery.value(0).toString(), Qt::ISODate);
        const QTime savedStartTime = QTime::fromString(ongoingQuery.value(1).toString(), "HH:mm:ss.zzz");
        const QDate savedEndDate = QDate::fromString(ongoingQuery.value(2).toString(), Qt::ISODate);
        const QTime savedEndTime = QTime::fromString(ongoingQuery.value(3).toString(), "HH:mm:ss.zzz");
        const qint64 savedDuration = ongoingQuery.value(4).toLongLong();

        const QDateTime savedStartUtc(savedStartDate, savedStartTime, Qt::UTC);
        const QDateTime savedEndUtc(savedEndDate, savedEndTime, Qt::UTC);

        QCOMPARE(savedStartUtc, snapshotStart.toUTC());
        QCOMPARE(savedEndUtc, snapshotEnd.toUTC());
        QCOMPARE(savedDuration, snapshotStart.msecsTo(snapshotEnd));

        QVERIFY(!ongoingQuery.next());
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void HistoryDialogTest::test_splitdialog_default_types_and_bounds()
{
    QDateTime start = makeTime(0);
    QDateTime end = makeTime(10 * 1000);
    SplitDialog dialog(start, end);

    QCOMPARE(dialog.getFirstSegmentType(), DurationType::Pause);
    QCOMPARE(dialog.getSecondSegmentType(), DurationType::Activity);
    QCOMPARE(dialog.slider_->minimum(), 1);
    QCOMPARE(dialog.slider_->maximum(), 9);
    QCOMPARE(dialog.getSplitTime(), start.addSecs(dialog.slider_->value()));
}

void HistoryDialogTest::test_splitdialog_short_duration_disables_slider()
{
    QDateTime start = makeTime(0);
    QDateTime end = makeTime(2 * 1000);
    SplitDialog dialog(start, end);

    QCOMPARE(dialog.slider_->minimum(), 1);
    QCOMPARE(dialog.slider_->maximum(), 1);
    QCOMPARE(dialog.slider_->value(), 1);
    QVERIFY(!dialog.slider_->isEnabled());
}

void HistoryDialogTest::test_splitdialog_minimum_duration_boundaries()
{
    {
        QDateTime start = makeTime(0);
        QDateTime end = makeTime(2 * 1000);
        SplitDialog dialog(start, end);
        QVERIFY(!dialog.slider_->isEnabled());
        QCOMPARE(dialog.slider_->minimum(), 1);
        QCOMPARE(dialog.slider_->maximum(), 1);
    }

    {
        QDateTime start = makeTime(0);
        QDateTime end = makeTime(3 * 1000);
        SplitDialog dialog(start, end);
        QVERIFY(dialog.slider_->isEnabled());
        QCOMPARE(dialog.slider_->minimum(), 1);
        QCOMPARE(dialog.slider_->maximum(), 2);
    }

    {
        QDateTime start = makeTime(0);
        QDateTime end = makeTime(4 * 1000);
        SplitDialog dialog(start, end);
        QVERIFY(dialog.slider_->isEnabled());
        QCOMPARE(dialog.slider_->minimum(), 1);
        QCOMPARE(dialog.slider_->maximum(), 3);
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
