#ifndef TEST_TIMELINE_H
#define TEST_TIMELINE_H

#include <QObject>
#include "testcommon.h"

class TimelineTest : public QObject
{
    Q_OBJECT

private slots:
    // Test M: activeMsec/pauseMsec sums including ongoing
    void test_M_totals();

    // Test N: withSegmentType is pure — original unchanged, returned has new type
    void test_N_withSegmentType_is_pure();

    // Test O: withSplit preserves total duration and produces new segment_id for second half
    void test_O_withSplit_preserves_total_duration();

    // Test P: normalized() matches legacy cleanDurations
    void test_P_normalized_matches_cleanDurations();

    // Test Q: groupByDate assigns segments to correct dates
    void test_Q_groupByDate();

    // Test R: TimeDuration::create factory invariants (return type + error field)
    void test_R_factory_rejects_cross_midnight();
    void test_R_factory_accepts_same_day();
    void test_R_factory_rejects_zero_duration();
    void test_R_factory_rejects_negative_duration();
    // 9.1: typed error field for each rejection cause
    void test_R_create_error_invalid_timestamp();
    void test_R_create_error_nonpositive();
    void test_R_create_error_cross_midnight();

    // Test S: normalized() cross-day merge guard
    void test_S_normalized_no_cross_day_merge();
    void test_S_normalized_same_day_still_merges();

    // 9.3: classifyMerge targeted cases
    void test_classifyMerge_none_for_different_types();
    void test_classifyMerge_branch1_near_duplicate();
    void test_classifyMerge_branch2_supersede();
    void test_classifyMerge_branch3_extend_prev_start();
    void test_classifyMerge_branch4_forward_overlap();
    void test_classifyMerge_branch5_subset();
    void test_classifyMerge_branch6_small_gap();
    void test_classifyMerge_branch7_slight_overlap();
    void test_classifyMerge_none_large_gap();
};

#endif // TEST_TIMELINE_H
