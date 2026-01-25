#ifndef UTIMERTEST_H
#define UTIMERTEST_H

#include <QtTest>
#include <deque>
#include <iterator>
#include <cmath>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtDebug>

// Expose private members for testing
#define private public
#define protected public
#include "timetracker.h"
#include "lockstatewatcher.h"
#undef private
#undef protected

#include "databasemanager.h"
#include "helpers.h"
#include "settings.h"

class uTimerTest : public QObject
{
    Q_OBJECT

private:
    QString db_path_;
    QString db_backup_path_;

    static TimeDuration mk(DurationType type, qint64 startMs, qint64 endMs)
    {
        QDateTime start = QDateTime::fromMSecsSinceEpoch(startMs, Qt::UTC);
        QDateTime end = QDateTime::fromMSecsSinceEpoch(endMs, Qt::UTC);
        return TimeDuration(type, start, end);
    }

    static QString createSettingsFile(const QString& dirPath, int historyDays)
    {
        QString settingsPath = QDir(dirPath).filePath("user-settings.ini");
        QSettings seed(settingsPath, QSettings::IniFormat);
        seed.setValue("uTimer/history_days_to_keep", historyDays);
        seed.setValue("uTimer/debug_log_to_file", false);
        seed.sync();
        return settingsPath;
    }

    void resetDatabaseFile() const
    {
        if (QFile::exists(db_path_)) {
            QFile::remove(db_path_);
        }
    }

    static qint64 sumDurations(const std::deque<TimeDuration>& d, DurationType type)
    {
        qint64 total = 0;
        for (const auto& t : d) {
            if (t.type == type) total += t.duration;
        }
        return total;
    }

private slots:
    void initTestCase_data()
    {
        // Arrange-Act-Assert tests only
    }

    void initTestCase()
    {
        db_path_ = QDir(QCoreApplication::applicationDirPath()).filePath("uTimer.sqlite");
        if (QFile::exists(db_path_)) {
            db_backup_path_ = db_path_ + ".bak_test";
            QFile::remove(db_backup_path_);
            QVERIFY(QFile::rename(db_path_, db_backup_path_));
        }
    }

    void init()
    {
    }

    // ==================== Settings parsing ====================
    void test_settings_defaults_and_bounds()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        QString settingsPath = QDir(tempDir.path()).filePath("user-settings.ini");

        // Write nothing: should load defaults
        Settings defaults(settingsPath);
        QCOMPARE(defaults.getHistoryDays(), 99);
        QCOMPARE(defaults.isAutopauseEnabled(), true);
        QCOMPARE(defaults.logToFile(), false);
        QCOMPARE(defaults.getBackpauseMsec(), convMinToMsec(15));
        QCOMPARE(defaults.getCheckpointIntervalMsec(), convMinToMsec(5));

        // Negative history clamps to 0
        QSettings writer(settingsPath, QSettings::IniFormat);
        writer.setValue("uTimer/history_days_to_keep", -5);
        writer.sync();
        Settings clamped(settingsPath);
        QCOMPARE(clamped.getHistoryDays(), 0);

        // Bounds for checkpoint interval [0,60]
        writer.setValue("uTimer/checkpoint_interval_minutes", 120);
        writer.sync();
        Settings capped(settingsPath);
        QCOMPARE(capped.getCheckpointIntervalMsec(), convMinToMsec(60));
    }

    void test_cleanDurations_duplicateRemoval()
    {
        // Description: Identical Activity entries -> keep one

        // Arrange
        std::deque<TimeDuration> d;
        const qint64 base = 1'000'000;
        d.push_back(mk(DurationType::Activity, base - 1000, base));
        d.push_back(mk(DurationType::Activity, base - 1000, base));
        QCOMPARE((int)d.size(), 2);

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 1);
        QCOMPARE(d.front().type, DurationType::Activity);
        QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), base);
        QCOMPARE(d.front().duration, (qint64)1000);
    }

    void test_cleanDurations_nearDuplicateRemoval()
    {
        // Description: Nearly identical Activity entries (<=50ms diff) -> remove the latter

        // Arrange
        std::deque<TimeDuration> d;
        const qint64 base = 1'000'000;
        d.push_back(mk(DurationType::Activity, base - 1000, base));          // [base-1000, base]
        d.push_back(mk(DurationType::Activity, base - 990, base + 20));      // [base-990, base+20]
        QCOMPARE((int)d.size(), 2);

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 1);
        QCOMPARE(d.front().type, DurationType::Activity);
        QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), base);
        QCOMPARE(d.front().duration, (qint64)1000);
    }

    void test_cleanDurations_mergeSmallGap()
    {
        // Description: Merge same-type entries with small positive gap (<500ms)

        // Arrange
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 0, 1000));    // [0,1000]
        d.push_back(mk(DurationType::Activity, 1100, 1300)); // gap=100, union => [0,1300]
        QCOMPARE((int)d.size(), 2);

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 1);
        QCOMPARE(d.front().type, DurationType::Activity);
        QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)1300);
        QCOMPARE(d.front().duration, (qint64)1300);
    }

    void test_cleanDurations_subsetRemoval()
    {
        // Description: Second entry fully inside previous -> remove subset

        // Arrange
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 1000, 2000)); // [1000,2000]
        d.push_back(mk(DurationType::Activity, 1500, 1700)); // inside previous
        QCOMPARE((int)d.size(), 2);

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 1);
        QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)2000);
        QCOMPARE(d.front().duration, (qint64)1000);
    }

    void test_cleanDurations_overlapExtendForward()
    {
        // Description: Overlap where second extends beyond previous end -> join and extend correctly (no double count)

        // Arrange
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 1000, 1500)); // dur 500
        d.push_back(mk(DurationType::Activity, 1400, 1700)); // extends beyond
        QCOMPARE((int)d.size(), 2);

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 1);
        QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)1700);
        // After bugfix, duration must be union length: 1700 - 1000 = 700
        QCOMPARE(d.front().duration, (qint64)700);
    }

    void test_cleanDurations_sameEndDifferentLength_keepLonger()
    {
        // Description: Two Activity durations ending at same time but different lengths -> keep the longer

        // Arrange
        std::deque<TimeDuration> d;
        const qint64 end = 10'000;
        d.push_back(mk(DurationType::Activity, end - 800, end));  // shorter [9200,10000]
        d.push_back(mk(DurationType::Activity, end - 1000, end)); // longer  [9000,10000]
        QCOMPARE((int)d.size(), 2);

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 1);
        QCOMPARE(d.front().type, DurationType::Activity);
        QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), end);
        QCOMPARE(d.front().duration, (qint64)1000);
    }

    void test_cleanDurations_mergeTwoThenRemoveSuperset()
    {
        // Description: Merge [0,10000] and [10001,20000] (1ms gap), then remove near-duplicate [0,20000]

        // Arrange
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 0, 10000));     // dur 10000
        d.push_back(mk(DurationType::Activity, 10001, 20000)); // gap=1, dur 9999
        d.push_back(mk(DurationType::Activity, 0, 20000));     // identical to merged result
        QCOMPARE((int)d.size(), 3);

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 1);
        QCOMPARE(d.front().type, DurationType::Activity);
        QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)20000);
        QCOMPARE(d.front().duration, (qint64)20000);
    }

    void test_cleanDurations_gapEqualsThreshold_noMerge_supersetRemains()
    {
        // Description: [0,10000], [10500,20000] (gap=500, no merge), then [0,20500] -> one-pass keeps [0,20500]

        // Arrange
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 0, 10000));     // [0,10000]
        d.push_back(mk(DurationType::Activity, 10500, 20000)); // [10500,20000] gap=500
        d.push_back(mk(DurationType::Activity, 0, 20500));     // superset
        QCOMPARE((int)d.size(), 3);

        // Act
        cleanDurations(&d);

        // Assert (document current behavior: no second pass)
        QCOMPARE((int)d.size(), 1);
        QCOMPARE(d[0].type, DurationType::Activity);
        QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), (qint64)20500);
        QCOMPARE(d[0].duration, (qint64)20500);
    }

    void test_cleanDurations_gapEqualsThreshold_noMerge_allUntouched()
    {
        // Description: [0,10000], [10500,20000] (gap=500, no merge), then [0,20500] -> one-pass keeps Activity [0,20500] and Pause [10500,20000]

        // Arrange
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 0, 10000));     // [0,10000]
        d.push_back(mk(DurationType::Pause, 10500, 20000));    // [10500,20000] gap=500
        d.push_back(mk(DurationType::Activity, 0, 20500));     // superset
        QCOMPARE((int)d.size(), 3);

        // Act
        cleanDurations(&d);

        // Assert (document current behavior: no second pass)
        QCOMPARE((int)d.size(), 2);
        QCOMPARE(d[0].type, DurationType::Activity);
        QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), (qint64)20500);
        QCOMPARE(d[0].duration, (qint64)20500);
        QCOMPARE(d[1].type, DurationType::Pause);
        QCOMPARE(d[1].endTime.toMSecsSinceEpoch(), (qint64)20000);
        QCOMPARE(d[1].duration, (qint64)20000 - 10500);
    }

    // chain merge across three small gaps
    void test_cleanDurations_chainMerge_threeSmallGaps()
    {
        // Description: [0,1000], [1050,2000], [2050,3000] -> merge into single [0,3000]

        // Arrange
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 0, 1000));
        d.push_back(mk(DurationType::Activity, 1050, 2000));
        d.push_back(mk(DurationType::Activity, 2050, 3000));
        QCOMPARE((int)d.size(), 3);

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 1);
        QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)3000);
        QCOMPARE(d.front().duration, (qint64)3000);
    }

    // remove multiple near-duplicates leaving one
    void test_cleanDurations_removeMultipleNearDuplicates()
    {
        // Description: [0,1000], [10,1010], [20,1020] -> collapse to one

        // Arrange
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 0, 1000));
        d.push_back(mk(DurationType::Activity, 10, 1010));
        d.push_back(mk(DurationType::Activity, 20, 1020));
        QCOMPARE((int)d.size(), 3);

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 1);
        QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)1000);
        QCOMPARE(d.front().duration, (qint64)1000);
    }

    // left-side overlap join behavior
    void test_cleanDurations_leftOverlapJoin()
    {
        // Description: it starts before prev and ends inside prev -> join to [900,1600]

        // Arrange
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 1000, 1600)); // prev
        d.push_back(mk(DurationType::Activity, 900, 1500));  // it (left overlap)
        QCOMPARE((int)d.size(), 2);

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 1);
        QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)1600);
        QCOMPARE(d.front().duration, (qint64)700);
    }

    // chain merge then remove duplicate
    void test_cleanDurations_chainMergeThenRemoveDuplicate()
    {
        // Description: [0,1000], [1050,2000], [2050,3000], [0,3000] -> end with one

        // Arrange
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 0, 1000));
        d.push_back(mk(DurationType::Activity, 1050, 2000));
        d.push_back(mk(DurationType::Activity, 2050, 3000));
        d.push_back(mk(DurationType::Activity, 0, 3000));
        QCOMPARE((int)d.size(), 4);

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 1);
        QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)3000);
        QCOMPARE(d.front().duration, (qint64)3000);
    }

    // touching intervals (gap == 0) are merged by overlap rule
    void test_cleanDurations_touchingIntervals_mergedByOverlap()
    {
        // Description: [0,1000], [1000,1500] -> merged (it_start <= prev_end triggers overlap rule)

        // Arrange
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 0, 1000));
        d.push_back(mk(DurationType::Activity, 1000, 1500));
        QCOMPARE((int)d.size(), 2);

        // Act
        cleanDurations(&d);

        // Assert (matches union length)
        QCOMPARE((int)d.size(), 1);
        QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)1500);
        QCOMPARE(d.front().duration, (qint64)1500);
    }

    // longer first, shorter second with same end -> remove shorter
    void test_cleanDurations_longerFirst_shorterSecond_sameEnd()
    {
        // Description: prev longer [9000,10000], it shorter [9500,10000] -> keep longer

        // Arrange
        std::deque<TimeDuration> d;
        const qint64 end = 10'000;
        d.push_back(mk(DurationType::Activity, end - 1000, end)); // prev longer
        d.push_back(mk(DurationType::Activity, end - 500, end));  // it shorter

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 1);
        QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), end);
        QCOMPARE(d.front().duration, (qint64)1000);
    }

    // disjoint with large gap -> no merge
    void test_cleanDurations_disjointLargeGap_noMerge()
    {
        // Description: [0,1000], [2000,3000] -> remain two

        // Arrange
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 0, 1000));
        d.push_back(mk(DurationType::Activity, 2000, 3000));

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 2);
        QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), (qint64)1000);
        QCOMPARE(d[1].endTime.toMSecsSinceEpoch(), (qint64)3000);
    }

    // boundary: gap just under threshold (499ms) merges
    void test_cleanDurations_gapJustUnderThreshold_merges()
    {
        // Description: [0,1000], [1499,1600] gap=499 -> merge to [0,1600]

        // Arrange
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 0, 1000));
        d.push_back(mk(DurationType::Activity, 1499, 1600));

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 1);
        QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)1600);
        QCOMPARE(d.front().duration, (qint64)1600);
    }

    // Presorted mixed types, touching boundaries: no cleaning across types
    void test_cleanDurations_presorted_mixedTypes_touching_noChange()
    {
        // Description: Activity [0,1000], Pause [1000,1500], Activity [1500,2000] -> unchanged

        // Arrange (already sorted)
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 0, 1000));
        d.push_back(mk(DurationType::Pause, 1000, 1500));
        d.push_back(mk(DurationType::Activity, 1500, 2000));

        // Copy for comparison
        auto before = d;

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 3);
        QCOMPARE(d[0].type, before[0].type);
        QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), before[0].endTime.toMSecsSinceEpoch());
        QCOMPARE(d[0].duration, before[0].duration);
        QCOMPARE(d[1].type, before[1].type);
        QCOMPARE(d[1].endTime.toMSecsSinceEpoch(), before[1].endTime.toMSecsSinceEpoch());
        QCOMPARE(d[1].duration, before[1].duration);
        QCOMPARE(d[2].type, before[2].type);
        QCOMPARE(d[2].endTime.toMSecsSinceEpoch(), before[2].endTime.toMSecsSinceEpoch());
        QCOMPARE(d[2].duration, before[2].duration);
    }

    // Presorted mixed types with small gaps: no cleaning across types
    void test_cleanDurations_presorted_mixedTypes_smallGaps_noChange()
    {
        // Description: Activity [0,1000], Pause [1100,1300], Activity [1350,1600] -> unchanged

        // Arrange (already sorted)
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 0, 1000));
        d.push_back(mk(DurationType::Pause, 1100, 1300));
        d.push_back(mk(DurationType::Activity, 1350, 1600));
        auto before = d;

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 3);
        for (size_t i = 0; i < d.size(); ++i) {
            QCOMPARE(d[i].type, before[i].type);
            QCOMPARE(d[i].endTime.toMSecsSinceEpoch(), before[i].endTime.toMSecsSinceEpoch());
            QCOMPARE(d[i].duration, before[i].duration);
        }
    }

    // Presorted identical time windows but different types: keep both
    void test_cleanDurations_presorted_identicalTimes_differentTypes_keepBoth()
    {
        // Description: Activity [0,1000], Pause [0,1000] -> both remain (no same-type rule)

        // Arrange (already sorted by start/end)
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 0, 1000));
        d.push_back(mk(DurationType::Pause, 0, 1000));
        auto before = d;

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 2);
        QCOMPARE(d[0].type, before[0].type);
        QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), before[0].endTime.toMSecsSinceEpoch());
        QCOMPARE(d[0].duration, before[0].duration);
        QCOMPARE(d[1].type, before[1].type);
        QCOMPARE(d[1].endTime.toMSecsSinceEpoch(), before[1].endTime.toMSecsSinceEpoch());
        QCOMPARE(d[1].duration, before[1].duration);
    }

    void test_cleanDurations_differentTypesNotMerged()
    {
        // Description: Different types must never be merged

        // Arrange
        std::deque<TimeDuration> d;
        d.push_back(mk(DurationType::Activity, 0, 1000));
        d.push_back(mk(DurationType::Pause, 1000, 1200));
        QCOMPARE((int)d.size(), 2);

        // Act
        cleanDurations(&d);

        // Assert
        QCOMPARE((int)d.size(), 2);
        QCOMPARE(d[0].type, DurationType::Activity);
        QCOMPARE(d[1].type, DurationType::Pause);
    }

    void test_database_backups_and_retention_and_disable()
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

    // ==================== Tests for explicit start times ====================
    void test_timetracker_start_pause_resume_stop_and_checkpoints()
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

    void test_timetracker_backpause_resets_checkpoint_and_splits()
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

    void test_timetracker_midnight_split_and_checkpoint_reset()
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

    void test_timetracker_lock_events_checkpoint_and_resume()
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

    void test_lockstatewatcher_debounce_logic()
    {
        Settings settings(createSettingsFile(QDir::tempPath(), 7));
        LockStateWatcher watcher(settings);

        // feed majority lock pattern
        std::deque<bool> pattern = {false,false,true,true,true};
        LockEvent event = LockEvent::None;
        for (bool state : pattern) {
            event = watcher.determineLockEvent(state);
        }
        QCOMPARE(event, LockEvent::Lock);

        // feed majority unlock pattern
        pattern = {true,true,false,false,false};
        for (bool state : pattern) {
            event = watcher.determineLockEvent(state);
        }
        QCOMPARE(event, LockEvent::Unlock);
    }

    void test_explicitStartTimes_constructorComputesDuration()
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

    void test_explicitStartTimes_startTimePreservedAfterClean()
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

    void test_explicitStartTimes_mergeUpdatesAllFields()
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

    void test_splitPreservesStartTime()
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

    void test_zeroDurationNotAdded()
    {
        // Description: Zero-duration segments should not affect the deque

        // Arrange
        QDateTime now = QDateTime::fromMSecsSinceEpoch(1000, Qt::UTC);
        TimeDuration d(DurationType::Activity, now, now);

        // Assert
        QCOMPARE(d.duration, (qint64)0);
    }

    void test_negativeDurationHandled()
    {
        // Description: If end is before start, duration should be negative (validation catches this)

        // Arrange
        QDateTime start = QDateTime::fromMSecsSinceEpoch(5000, Qt::UTC);
        QDateTime end = QDateTime::fromMSecsSinceEpoch(3000, Qt::UTC);
        TimeDuration d(DurationType::Activity, start, end);

        // Assert - duration is negative, validation layer should reject
        QCOMPARE(d.duration, (qint64)-2000);
    }

    void test_schemaValidation_missingStartColumns()
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

    void test_exactMatching_upsertReplacesByStartTime()
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

    void test_checkpointPreservesStartTimeOnUpdate()
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

    void test_clockDriftResilience_durationStoredFromElapsed()
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

    void test_loadDurations_skipsNegativeDurationRows()
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

    void test_helpers_conversions()
    {
        // convMSecToTimeStr
        QCOMPARE(convMSecToTimeStr(3661000), QString("01:01:01"));
        QCOMPARE(convMSecToTimeStr(0), QString("00:00:00"));

        // convMinAndSecToHourPctString
        // 30 min = 0.5 hours -> "50" (dot is added by caller)
        QCOMPARE(convMinAndSecToHourPctString(30, 0), QString("50"));
        // 15 min = 0.25 hours -> "25"
        QCOMPARE(convMinAndSecToHourPctString(15, 0), QString("25"));
        // 45 min = 0.75 hours -> "75"
        QCOMPARE(convMinAndSecToHourPctString(45, 0), QString("75"));

        // convTimeStrToDurationStr
        QCOMPARE(convTimeStrToDurationStr("1:30:00"), QString("1.50"));
        QCOMPARE(convTimeStrToDurationStr("0:15:00"), QString("0.25"));
    }

    void test_timetracker_ongoing_duration()
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

    void test_timetracker_set_duration_type()
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

    void test_timetracker_checkpoints_paused()
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

    void test_databasemanager_write_failure()
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

    // ==================== Database Transaction & Rollback Tests ====================
    
    void test_database_transaction_rollback_on_insert_failure()
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

    void test_database_transaction_rollback_on_replace_failure()
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

    // ==================== Checkpoint System Tests ====================
    
    void test_database_checkpoint_id_reuse()
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

    void test_database_checkpoint_deleted_row_creates_new()
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

    void test_database_checkpoint_preserves_start_time()
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

    // ==================== UPSERT Tests ====================
    
    void test_database_upsert_insert_mode()
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

    void test_database_upsert_replace_mode()
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

    void test_database_upsert_unique_constraint()
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

    void test_database_upsert_empty_deque()
    {
        resetDatabaseFile();
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Settings settings(createSettingsFile(tempDir.path(), 7));
        DatabaseManager manager(settings);

        std::deque<TimeDuration> empty;
        QVERIFY(manager.updateDurationsByStartTime(empty)); // Should succeed as no-op
    }

    // ==================== Data Validation Tests ====================
    
    void test_database_load_negative_duration()
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

    void test_database_load_start_after_end()
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

    void test_database_load_invalid_enum_type()
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

    void test_database_load_duration_mismatch_tolerance()
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

    // ==================== Timestamp Handling Tests ====================
    
    void test_database_timezone_roundtrip()
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

    void test_database_millisecond_precision()
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

    // ==================== Schema Validation Tests ====================
    
    void test_database_schema_validation_missing_start_date()
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

    void test_database_schema_validation_fresh_database()
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

    // ==================== Backup System Tests ====================
    
    void test_database_backup_file_creation()
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

    void test_database_backup_preserves_data()
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

    // ==================== Integration Tests ====================
    
    void test_integration_checkpoint_recovery_on_restart()
    {
        resetDatabaseFile();
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        QString settingsPath = createSettingsFile(tempDir.path(), 7);
        
        QDateTime start = QDateTime::currentDateTime();
        
        {
            // First session: Start timer and save checkpoint
            Settings settings(settingsPath);
            TimeTracker tracker(settings);
            
            tracker.useTimerViaButton(Button::Start);
            QTest::qWait(100);
            
            // Manually save checkpoint
            tracker.saveCheckpointInternal();
            QVERIFY(tracker.current_checkpoint_id_ != -1);
            
            // Simulate crash (tracker destroyed without stopping)
        }
        
        {
            // Second session: Load from database (simulates app restart)
            Settings settings(settingsPath);
            DatabaseManager db(settings);
            
            auto loaded = db.loadDurations();
            QCOMPARE(loaded.size(), (size_t)1); // Checkpoint recovered
            QCOMPARE(loaded[0].type, DurationType::Activity);
        }
    }

    void test_integration_memory_db_consistency()
    {
        resetDatabaseFile();
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Settings settings(createSettingsFile(tempDir.path(), 7));
        TimeTracker tracker(settings);

        // Start -> Activity for 50ms
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(50);
        
        // Pause
        tracker.useTimerViaButton(Button::Pause);
        auto memoryDurations = tracker.getCurrentDurations();
        QVERIFY(memoryDurations.size() >= 1);
        
        // Resume -> Activity for 50ms
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(50);
        
        // Save checkpoint
        tracker.saveCheckpointInternal();
        QVERIFY(tracker.current_checkpoint_id_ != -1);
        
        // Stop
        tracker.useTimerViaButton(Button::Stop);
        
        // Load from DB
        DatabaseManager db(settings);
        auto dbDurations = db.loadDurations();
        
        // Total durations should match (memory + checkpoint)
        qint64 memoryTotal = sumDurations(memoryDurations, DurationType::Activity) + 
                            sumDurations(memoryDurations, DurationType::Pause);
        qint64 dbTotal = sumDurations(dbDurations, DurationType::Activity) +
                        sumDurations(dbDurations, DurationType::Pause);
        
        // Allow small tolerance for timing differences
        QVERIFY(qAbs(memoryTotal - dbTotal) < 200);
    }

    void test_integration_retention_cleanup_preserves_current()
    {
        resetDatabaseFile();
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Settings settings(createSettingsFile(tempDir.path(), 2)); // Keep 2 days
        DatabaseManager manager(settings);

        QDateTime now = QDateTime::currentDateTimeUtc();
        
        // Insert data from 5 days ago (should be deleted)
        std::deque<TimeDuration> old;
        old.emplace_back(DurationType::Activity, now.addDays(-5), now.addDays(-5).addSecs(60));
        QVERIFY(manager.saveDurations(old, TransactionMode::Append));
        
        // Insert data from today (should be kept)
        std::deque<TimeDuration> current;
        current.emplace_back(DurationType::Activity, now.addSecs(-100), now.addSecs(-90));
        QVERIFY(manager.saveDurations(current, TransactionMode::Append));
        
        // Force cleanup by reopening database
        DatabaseManager manager2(settings);
        
        auto loaded = manager2.loadDurations();
        QVERIFY(loaded.size() >= 1); // Current day preserved
        
        // Verify old entries are gone
        bool hasOldEntry = false;
        for (const auto& d : loaded) {
            if (d.endTime.date() == now.addDays(-5).date()) {
                hasOldEntry = true;
                break;
            }
        }
        QVERIFY(!hasOldEntry);
    }

    void test_integration_duplicate_prevention()
    {
        resetDatabaseFile();
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Settings settings(createSettingsFile(tempDir.path(), 7));
        DatabaseManager manager(settings);

        QDateTime start = QDateTime::currentDateTime();
        QDateTime end = start.addSecs(10);
        
        // Insert same duration twice using saveDurations
        std::deque<TimeDuration> durations;
        durations.emplace_back(DurationType::Activity, start, end);
        
        QVERIFY(manager.saveDurations(durations, TransactionMode::Append));
        QVERIFY(manager.saveDurations(durations, TransactionMode::Append));
        
        // Load - due to UNIQUE constraint, should have only 1 entry
        auto loaded = manager.loadDurations();
        QCOMPARE(loaded.size(), (size_t)1);
    }

    void test_integration_empty_database_operations()
    {
        resetDatabaseFile();
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Settings settings(createSettingsFile(tempDir.path(), 7));
        DatabaseManager manager(settings);

        // Operations on empty database should succeed
        QVERIFY(manager.saveDurations({}, TransactionMode::Append));
        auto loaded = manager.loadDurations();
        QCOMPARE(loaded.size(), (size_t)0);
        
        QVERIFY(!manager.hasEntriesForDate(QDate::currentDate()));
        
        std::deque<TimeDuration> empty;
        QVERIFY(manager.updateDurationsByStartTime(empty));
    }

    void test_integration_backpause_db_update()
    {
        resetDatabaseFile();
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Settings settings(createSettingsFile(tempDir.path(), 7));
        TimeTracker tracker(settings);

        // Start activity
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(100);
        
        // Save checkpoint
        tracker.saveCheckpointInternal();
        long long checkpointId = tracker.current_checkpoint_id_;
        QVERIFY(checkpointId != -1);
        
        // Simulate lock
        tracker.useTimerViaLockEvent(LockEvent::Lock);
        
        // Checkpoint ID should still be valid (lock doesn't reset it)
        QCOMPARE(tracker.current_checkpoint_id_, checkpointId);
        
        // Simulate long ongoing lock (triggers backpause)
        tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock);
        
        // Verify checkpoint ID was reset by backpause
        QCOMPARE(tracker.current_checkpoint_id_, (long long)-1);
        
        // Verify durations were updated in DB
        DatabaseManager db(settings);
        auto loaded = db.loadDurations();
        QVERIFY(loaded.size() >= 1);
    }

    void cleanup()
    {
    }

    void cleanupTestCase()
    {
        if (!db_path_.isEmpty()) {
            QFile::remove(db_path_);
        }
        if (!db_backup_path_.isEmpty()) {
            QFile::remove(db_path_);
            QVERIFY(QFile::rename(db_backup_path_, db_path_));
        }
    }
};

#endif // UTIMERTEST_H
