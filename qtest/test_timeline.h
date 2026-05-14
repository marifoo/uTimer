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
};

#endif // TEST_TIMELINE_H
