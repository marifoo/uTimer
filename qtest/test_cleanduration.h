#ifndef TEST_CLEANDURATION_H
#define TEST_CLEANDURATION_H

#include <QObject>
#include "testcommon.h"

class CleanDurationsTest : public QObject
{
    Q_OBJECT

private slots:
    void test_cleanDurations_duplicateRemoval();
    void test_cleanDurations_nearDuplicateRemoval();
    void test_cleanDurations_mergeSmallGap();
    void test_cleanDurations_subsetRemoval();
    void test_cleanDurations_overlapExtendForward();
    void test_cleanDurations_sameEndDifferentLength_keepLonger();
    void test_cleanDurations_mergeTwoThenRemoveSuperset();
    void test_cleanDurations_gapEqualsThreshold_noMerge_supersetRemains();
    void test_cleanDurations_gapEqualsThreshold_noMerge_allUntouched();
    void test_cleanDurations_chainMerge_threeSmallGaps();
    void test_cleanDurations_removeMultipleNearDuplicates();
    void test_cleanDurations_leftOverlapJoin();
    void test_cleanDurations_chainMergeThenRemoveDuplicate();
    void test_cleanDurations_touchingIntervals_mergedByOverlap();
    void test_cleanDurations_longerFirst_shorterSecond_sameEnd();
    void test_cleanDurations_disjointLargeGap_noMerge();
    void test_cleanDurations_gapJustUnderThreshold_merges();
    void test_cleanDurations_presorted_mixedTypes_touching_noChange();
    void test_cleanDurations_presorted_mixedTypes_smallGaps_noChange();
    void test_cleanDurations_presorted_identicalTimes_differentTypes_keepBoth();
    void test_cleanDurations_differentTypesNotMerged();
};

#endif // TEST_CLEANDURATION_H
