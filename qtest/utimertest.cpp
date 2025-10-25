#include <QtTest>
#include <deque>
#include <iterator>
#include <cmath>
#include <QDateTime>
#include <QtDebug>
#include "helpers.h"

class uTimerTest : public QObject
{
    Q_OBJECT

private:
    static TimeDuration mk(DurationType type, qint64 startMs, qint64 endMs)
    {
        QDateTime end = QDateTime::fromMSecsSinceEpoch(endMs);
        return TimeDuration(type, endMs - startMs, end);
    }

private slots:
    void initTestCase_data()
    {
        // Arrange-Act-Assert tests only
    }

    void initTestCase()
    {
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

    void cleanup()
    {
    }

    void cleanupTestCase()
    {
    }
};

QTEST_APPLESS_MAIN(uTimerTest)
#include "utimertest.moc"
