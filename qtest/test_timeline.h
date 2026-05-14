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

    // Test R: TimeDuration::create factory invariants
    void test_R_factory_rejects_cross_midnight();
    void test_R_factory_accepts_same_day();
    void test_R_factory_rejects_zero_duration();
    void test_R_factory_rejects_negative_duration();

    // Test S: normalized() cross-day merge guard
    void test_S_normalized_no_cross_day_merge();
    void test_S_normalized_same_day_still_merges();
};

#endif // TEST_TIMELINE_H
