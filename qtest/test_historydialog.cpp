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
    QVERIFY(tracker.replaceDurationsInDB(dbDurations));

    // Start ongoing activity
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(5);
    tracker.useTimerViaButton(Button::Stop);
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(5);

    HistoryDialog dialog(tracker, settings);
    QCOMPARE(dialog.pages_.size(), static_cast<size_t>(1));
    QCOMPARE(dialog.pages_[0].isCurrent, true);
    QCOMPARE(dialog.pendingChanges_[0].size(), static_cast<size_t>(3));
    QCOMPARE(dialog.rowOrigins_[0].size(), static_cast<size_t>(3));
    QVERIFY(dialog.pendingChanges_[0].back().duration > 0);
    QCOMPARE(dialog.rowOrigins_[0].back(), HistoryDialog::RowOrigin::Ongoing);
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
    QVERIFY(tracker.replaceDurationsInDB(dbDurations));

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
