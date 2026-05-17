#include "test_cleanduration.h"
#include <QtTest>
#include <algorithm>

using TestCommon::mk;

// Helper: create a TimeDuration with a specific segment_id for ID-tracking tests
static TimeDuration mkId(DurationType type, qint64 startMs, qint64 endMs, const QString& id)
{
    QDateTime start = QDateTime::fromMSecsSinceEpoch(startMs, Qt::UTC);
    QDateTime end = QDateTime::fromMSecsSinceEpoch(endMs, Qt::UTC);
    return TimeDuration::fromTrusted(type, start, end, SegmentId::fromString(id));
}

void CleanDurationsTest::test_cleanDurations_duplicateRemoval()
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

void CleanDurationsTest::test_cleanDurations_nearDuplicateRemoval()
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

void CleanDurationsTest::test_cleanDurations_mergeSmallGap()
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

void CleanDurationsTest::test_cleanDurations_subsetRemoval()
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

void CleanDurationsTest::test_cleanDurations_overlapExtendForward()
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

void CleanDurationsTest::test_cleanDurations_sameEndDifferentLength_keepLonger()
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

void CleanDurationsTest::test_cleanDurations_mergeTwoThenRemoveSuperset()
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

void CleanDurationsTest::test_cleanDurations_gapEqualsThreshold_noMerge_supersetRemains()
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

void CleanDurationsTest::test_cleanDurations_gapEqualsThreshold_noMerge_allUntouched()
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

void CleanDurationsTest::test_cleanDurations_chainMerge_threeSmallGaps()
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

void CleanDurationsTest::test_cleanDurations_removeMultipleNearDuplicates()
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

void CleanDurationsTest::test_cleanDurations_leftOverlapJoin()
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

void CleanDurationsTest::test_cleanDurations_chainMergeThenRemoveDuplicate()
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

void CleanDurationsTest::test_cleanDurations_touchingIntervals_mergedByOverlap()
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

void CleanDurationsTest::test_cleanDurations_longerFirst_shorterSecond_sameEnd()
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

void CleanDurationsTest::test_cleanDurations_disjointLargeGap_noMerge()
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

void CleanDurationsTest::test_cleanDurations_gapJustUnderThreshold_merges()
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

void CleanDurationsTest::test_cleanDurations_presorted_mixedTypes_touching_noChange()
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

void CleanDurationsTest::test_cleanDurations_presorted_mixedTypes_smallGaps_noChange()
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

void CleanDurationsTest::test_cleanDurations_presorted_identicalTimes_differentTypes_keepBoth()
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

void CleanDurationsTest::test_cleanDurations_differentTypesNotMerged()
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

// ──────────────────────────────────────────────────────────────────
// Tests for removed segment_id tracking (T14)
// ──────────────────────────────────────────────────────────────────

void CleanDurationsTest::test_cleanDurations_removedIds_empty_when_no_merge()
{
    // Description: Two disjoint same-type entries with large gap -> no merge, empty removedIds

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 0, 1000, "id-A"));
    d.push_back(mkId(DurationType::Activity, 5000, 6000, "id-B"));

    // Act
    auto removed = cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 2);
    QVERIFY(removed.empty());
}

void CleanDurationsTest::test_cleanDurations_removedIds_branch1_nearDuplicate()
{
    // Description: Branch 1 — near-duplicate (<=50ms diff). The erased entry (it) is the orphan.

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 0, 1000, "id-keep"));
    d.push_back(mkId(DurationType::Activity, 10, 1020, "id-dup"));  // diff_end=20, diff_dur~=20
    QCOMPARE((int)d.size(), 2);

    // Act
    auto removed = cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-keep"));
    QCOMPARE((int)removed.size(), 1);
    QCOMPARE(removed[0], QString("id-dup"));
}

void CleanDurationsTest::test_cleanDurations_removedIds_branch2_currentStartsBeforePrevShorter()
{
    // Description: Branches 2 and 3 require it_start < prev_start, which is unreachable
    // after the ascending sort — no merge branch increases prevIt->startTime.
    // These branches are defensive safety nets. This test exercises branch 5 (subset
    // removal) instead, verifying that it->segment_id is correctly reported as removed
    // when a shorter entry is fully contained in a longer one.

    // Arrange: [100,500] fully contains [200,400] -> branch 5 fires
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 100, 500, "id-big"));
    d.push_back(mkId(DurationType::Activity, 200, 400, "id-subset"));
    QCOMPARE((int)d.size(), 2);

    // Act
    auto removed = cleanDurations(&d);

    // Assert: subset removed, big kept
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-big"));
    QCOMPARE((int)removed.size(), 1);
    QCOMPARE(removed[0], QString("id-subset"));
}

void CleanDurationsTest::test_cleanDurations_removedIds_branch3_leftOverlapJoin()
{
    // Description: Branches 2 and 3 require it_start < prev_start, which is unreachable
    // after the ascending sort — no merge branch increases prevIt->startTime.
    // This test exercises branch 4 (overlap-extend-forward) instead, verifying that
    // it->segment_id is correctly reported as removed when overlapping entries are joined.

    // Arrange: [1000,1500] overlaps with [1400,1700] -> branch 4 fires (extend forward)
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 1000, 1500, "id-prev"));
    d.push_back(mkId(DurationType::Activity, 1400, 1700, "id-overlap"));
    QCOMPARE((int)d.size(), 2);

    // Act
    auto removed = cleanDurations(&d);

    // Assert: prevIt keeps its ID, it's ID is orphaned
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-prev"));
    QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), (qint64)1700);
    QCOMPARE(d[0].duration, (qint64)700);
    QCOMPARE((int)removed.size(), 1);
    QCOMPARE(removed[0], QString("id-overlap"));
}

void CleanDurationsTest::test_cleanDurations_removedIds_branch4_overlapExtendForward()
{
    // Description: Branch 4 — current overlaps into prev and extends beyond (includes touching).
    // The erased entry (it) is the orphan; prevIt keeps its segment_id.

    // Arrange: [0,1000] touching [1000,1500] -> branch 4 fires
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 0, 1000, "id-first"));
    d.push_back(mkId(DurationType::Activity, 1000, 1500, "id-second"));
    QCOMPARE((int)d.size(), 2);

    // Act
    auto removed = cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-first"));
    QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), (qint64)1500);
    QCOMPARE(d[0].duration, (qint64)1500);
    QCOMPARE((int)removed.size(), 1);
    QCOMPARE(removed[0], QString("id-second"));
}

void CleanDurationsTest::test_cleanDurations_removedIds_branch5_subsetRemoval()
{
    // Description: Branch 5 — current is a subset of previous -> erase current.
    // The erased entry (it) is the orphan; prevIt keeps its segment_id.

    // Arrange: [1000,3000] contains [1500,2500]
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 1000, 3000, "id-outer"));
    d.push_back(mkId(DurationType::Activity, 1500, 2500, "id-inner"));
    QCOMPARE((int)d.size(), 2);

    // Act
    auto removed = cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-outer"));
    QCOMPARE(d[0].duration, (qint64)2000);
    QCOMPARE((int)removed.size(), 1);
    QCOMPARE(removed[0], QString("id-inner"));
}

void CleanDurationsTest::test_cleanDurations_removedIds_branch6_smallGapMerge()
{
    // Description: Branch 6 — adjacent disjoint entries with gap < 500ms -> merge.
    // The erased entry (it) is the orphan; prevIt keeps its segment_id.

    // Arrange: [0,1000] and [1200,2000] -> gap=200 < 500 -> merge
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 0, 1000, "id-left"));
    d.push_back(mkId(DurationType::Activity, 1200, 2000, "id-right"));
    QCOMPARE((int)d.size(), 2);

    // Act
    auto removed = cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-left"));
    QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), (qint64)2000);
    QCOMPARE(d[0].duration, (qint64)2000);
    QCOMPARE((int)removed.size(), 1);
    QCOMPARE(removed[0], QString("id-right"));
}

void CleanDurationsTest::test_cleanDurations_removedIds_branch7_slightOverlapMerge()
{
    // Description: Branch 7 handles slight overlaps (gap < 0, abs < 100ms), but branches 4
    // and 5 catch all cases where it_start <= prev_end first, making branch 7 unreachable
    // after the ascending sort. This test exercises branch 4 with a slight overlap scenario,
    // verifying correct ID tracking when entries overlap by a small amount.

    // Arrange: [0,1000] and [950,1800] -> branch 4 fires (it_start 950 <= prev_end 1000,
    // prev_end 1000 <= it_end 1800)
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 0, 1000, "id-first"));
    d.push_back(mkId(DurationType::Activity, 950, 1800, "id-second"));
    QCOMPARE((int)d.size(), 2);

    // Act
    auto removed = cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-first"));
    QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), (qint64)1800);
    QCOMPARE(d[0].duration, (qint64)1800);
    QCOMPARE((int)removed.size(), 1);
    QCOMPARE(removed[0], QString("id-second"));
}

void CleanDurationsTest::test_cleanDurations_removedIds_chainMerge_returns_two_ids()
{
    // Description: Three entries merged into one via chain merge -> two IDs returned.
    // [0,1000] + [1050,2000] (gap=50 < 500, branch 6) + [2100,3000] (gap=100 < 500, branch 6)
    // First merge: id-B removed, prevIt=[0,2000,id-A]. Second merge: id-C removed.

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 0, 1000, "id-A"));
    d.push_back(mkId(DurationType::Activity, 1050, 2000, "id-B"));
    d.push_back(mkId(DurationType::Activity, 2100, 3000, "id-C"));
    QCOMPARE((int)d.size(), 3);

    // Act
    auto removed = cleanDurations(&d);

    // Assert: one surviving entry with id-A, two removed
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-A"));
    QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), (qint64)3000);
    QCOMPARE(d[0].duration, (qint64)3000);
    QCOMPARE((int)removed.size(), 2);
    // Both id-B and id-C should be in removed (order matches iteration order)
    QVERIFY(std::find(removed.begin(), removed.end(), QString("id-B")) != removed.end());
    QVERIFY(std::find(removed.begin(), removed.end(), QString("id-C")) != removed.end());
}

void CleanDurationsTest::test_cleanDurations_removedIds_single_entry_returns_empty()
{
    // Description: Single entry -> nothing to merge, removedIds is empty

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 0, 1000, "id-only"));
    QCOMPARE((int)d.size(), 1);

    // Act
    auto removed = cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-only"));
    QVERIFY(removed.empty());
}
