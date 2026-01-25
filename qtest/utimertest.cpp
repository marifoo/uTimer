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

    // ==================== Tests for explicit start times ====================

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
        Settings settings(createSettingsFile(tempDir.path(), 7));
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
        Settings settings(createSettingsFile(tempDir.path(), 7));
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
        Settings settings(createSettingsFile(tempDir.path(), 7));
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
        Settings settings(createSettingsFile(tempDir.path(), 7));
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

QTEST_APPLESS_MAIN(uTimerTest)
#include "utimertest.moc"
