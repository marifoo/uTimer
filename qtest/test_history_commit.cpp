#include "test_history_commit.h"
#include <QtTest>
#include <QTimer>
#include <QMessageBox>
#include <QAbstractButton>
#include <QApplication>

#include "../historydialog.h"

using TestCommon::createSettingsFile;

void HistoryCommitTest::initTestCase()
{
    db_path_ = QDir(QCoreApplication::applicationDirPath()).filePath("uTimer.sqlite");
    if (QFile::exists(db_path_)) {
        db_backup_path_ = db_path_ + ".bak_commit_test";
        QFile::remove(db_backup_path_);
        QVERIFY(QFile::rename(db_path_, db_backup_path_));
    }
}

void HistoryCommitTest::cleanupTestCase()
{
    if (!db_path_.isEmpty())
        QFile::remove(db_path_);
    if (!db_backup_path_.isEmpty()) {
        QFile::remove(db_path_);
        QVERIFY(QFile::rename(db_backup_path_, db_path_));
    }
}

void HistoryCommitTest::resetDatabaseFile() const
{
    if (QFile::exists(db_path_))
        QFile::remove(db_path_);
}

// ---------------------------------------------------------------------------
// accept() blocks close on DB failure
// ---------------------------------------------------------------------------

void HistoryCommitTest::test_accept_stays_open_on_db_failure()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDateTime now = QDateTime::currentDateTime();
    tracker.sessionState_dbg().durations.push_back(
        TimeDuration(DurationType::Activity, now.addSecs(-30), now.addSecs(-20)));

    std::deque<TimeDuration> dbDurations;
    dbDurations.emplace_back(DurationType::Pause, now.addSecs(-19), now.addSecs(-10));
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)).ok());

    HistoryDialog dialog(tracker, settings);

    // Hold an exclusive DB transaction to force replaceAll to fail.
    const QString lockConnName = "commit_test_db_lock";
    {
        QSqlDatabase lockDb = QSqlDatabase::addDatabase("QSQLITE", lockConnName);
        lockDb.setDatabaseName(db_path_);
        QVERIFY(lockDb.open());
        QSqlQuery lockQuery(lockDb);
        QVERIFY(lockQuery.exec("BEGIN EXCLUSIVE TRANSACTION"));

        // Auto-dismiss the "Database Error" critical dialog.
        QTimer::singleShot(0, []() {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                auto* mb = qobject_cast<QMessageBox*>(w);
                if (mb && mb->windowTitle() == "Database Error") {
                    mb->accept();
                    return;
                }
            }
        });

        dialog.accept();

        QVERIFY(lockQuery.exec("ROLLBACK"));
        lockDb.close();
    }
    QSqlDatabase::removeDatabase(lockConnName);

    // accept() must NOT have closed the dialog — result() should still be the
    // default (not QDialog::Accepted).
    QVERIFY(dialog.result() != QDialog::Accepted);

    // Timer state must be unchanged.
    QCOMPARE(tracker.sessionState_dbg().durations.size(), static_cast<size_t>(1));
    QCOMPARE(tracker.sessionState_dbg().durations[0].type, DurationType::Activity);
}

// ---------------------------------------------------------------------------
// accept() blocks close when user declines merge confirmation
// ---------------------------------------------------------------------------

void HistoryCommitTest::test_accept_stays_open_on_merge_decline()
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
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)).ok());
    tracker.sessionState_dbg().durations.push_back(TimeDuration(DurationType::Activity, memStart, memEnd));

    HistoryDialog dialog(tracker, settings);

    // User clicks "No" on the merge confirmation.
    QTimer::singleShot(0, []() {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* mb = qobject_cast<QMessageBox*>(w);
            if (mb && mb->windowTitle() == "Overlapping Segments") {
                mb->button(QMessageBox::No)->click();
                return;
            }
        }
    });

    dialog.accept();

    // Dialog must remain open (result still not Accepted).
    QVERIFY(dialog.result() != QDialog::Accepted);

    // Timer session unchanged.
    QCOMPARE(tracker.sessionState_dbg().durations.size(), static_cast<size_t>(1));
}

// ---------------------------------------------------------------------------
// accept() closes on success
// ---------------------------------------------------------------------------

void HistoryCommitTest::test_accept_closes_on_success()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    const QDateTime now = QDateTime::currentDateTime();
    tracker.sessionState_dbg().durations.push_back(
        TimeDuration(DurationType::Activity, now.addSecs(-30), now.addSecs(-10)));

    HistoryDialog dialog(tracker, settings);
    dialog.accept();

    // On success, QDialog::accept() sets result to Accepted.
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
}

// ---------------------------------------------------------------------------
// RAII guard: endExclusiveEdit called on accept path
// ---------------------------------------------------------------------------

void HistoryCommitTest::test_raii_guard_resumes_on_accept()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(tracker.isDialogOpen_dbg());
        QVERIFY(!tracker.isCheckpointTimerActive_dbg());

        dialog.accept();
    }

    // After accept (and dialog destruction), exclusive edit must have ended.
    QVERIFY(!tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    tracker.useTimerViaButton(Button::Stop);
}

// ---------------------------------------------------------------------------
// RAII guard: endExclusiveEdit called on reject path
// ---------------------------------------------------------------------------

void HistoryCommitTest::test_raii_guard_resumes_on_reject()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(tracker.isDialogOpen_dbg());
        QVERIFY(!tracker.isCheckpointTimerActive_dbg());

        dialog.reject();
    }

    // After reject, exclusive edit must have ended.
    QVERIFY(!tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    tracker.useTimerViaButton(Button::Stop);
}

// ---------------------------------------------------------------------------
// RAII guard: endExclusiveEdit called on destruction without accept/reject
// ---------------------------------------------------------------------------

void HistoryCommitTest::test_raii_guard_resumes_on_destruction()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    Timer tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(tracker.isDialogOpen_dbg());
        QVERIFY(!tracker.isCheckpointTimerActive_dbg());
        // Dialog is destroyed here without accept or reject.
    }

    // After destruction, exclusive edit must have ended.
    QVERIFY(!tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    tracker.useTimerViaButton(Button::Stop);
}
