#include "test_timeline.h"
#include "timeline.h"
#include "helpers.h"
#include <QtTest>

using TestCommon::mk;

namespace {

QDateTime makeUtc(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC);
}

} // namespace

// ============================================================================
// Test M — activeMsec/pauseMsec sums including ongoing
// ============================================================================

void TimelineTest::test_M_totals()
{
    const qint64 base = 1'000'000;

    // Two completed segments: 500ms Activity, 300ms Pause
    std::deque<TimeDuration> comp;
    comp.push_back(mk(DurationType::Activity, base, base + 500));
    comp.push_back(mk(DurationType::Pause, base + 600, base + 900));

    // Ongoing: 200ms Activity
    TimeDuration ongoing(DurationType::Activity, makeUtc(base + 1000), makeUtc(base + 1200));

    Timeline t(comp, ongoing);

    QCOMPARE(t.activeMsec(), static_cast<qint64>(500 + 200));
    QCOMPARE(t.pauseMsec(), static_cast<qint64>(300));
}

// ============================================================================
// Test N — withSegmentType is pure
// ============================================================================

void TimelineTest::test_N_withSegmentType_is_pure()
{
    const qint64 base = 2'000'000;

    std::deque<TimeDuration> comp;
    comp.push_back(mk(DurationType::Activity, base, base + 1000));
    comp.push_back(mk(DurationType::Pause, base + 1100, base + 1600));

    Timeline original(comp, std::nullopt);
    Timeline modified = original.withSegmentType(0, DurationType::Pause);

    // Original is unchanged
    QCOMPARE(original.completed()[0].type, DurationType::Activity);
    QCOMPARE(original.completed()[1].type, DurationType::Pause);

    // Modified has the new type at index 0
    QCOMPARE(modified.completed()[0].type, DurationType::Pause);
    QCOMPARE(modified.completed()[1].type, DurationType::Pause);

    // Sizes unchanged
    QCOMPARE(modified.completed().size(), original.completed().size());
}

// ============================================================================
// Test O — withSplit preserves total duration and produces new segment_id
// ============================================================================

void TimelineTest::test_O_withSplit_preserves_total_duration()
{
    const qint64 base = 3'000'000;
    const SegmentId origId = SegmentId::fromString("original-segment-id");

    std::deque<TimeDuration> comp;
    comp.push_back(TimeDuration(DurationType::Activity, makeUtc(base), makeUtc(base + 10000), origId));

    Timeline original(comp, std::nullopt);
    QDateTime splitAt = makeUtc(base + 4000);

    Timeline split = original.withSplit(0, splitAt, DurationType::Activity, DurationType::Pause);

    // Two segments now
    QCOMPARE(split.completed().size(), static_cast<size_t>(2));

    const auto& first = split.completed()[0];
    const auto& second = split.completed()[1];

    // Times are correct
    QCOMPARE(first.startTime, makeUtc(base));
    QCOMPARE(first.endTime, splitAt);
    QCOMPARE(second.startTime, splitAt);
    QCOMPARE(second.endTime, makeUtc(base + 10000));

    // Total duration preserved
    QCOMPARE(first.duration + second.duration, static_cast<qint64>(10000));

    // First half retains original segment_id; second gets a new one
    QCOMPARE(first.segment_id, origId);
    QVERIFY(!second.segment_id.isEmpty());
    QVERIFY(second.segment_id != origId);

    // Original is unchanged
    QCOMPARE(original.completed().size(), static_cast<size_t>(1));
    QCOMPARE(original.completed()[0].segment_id, origId);
}

// ============================================================================
// Test P — normalized() matches legacy cleanDurations
// ============================================================================

void TimelineTest::test_P_normalized_matches_cleanDurations()
{
    const qint64 base = 4'000'000;

    // Use fixed segment IDs so both copies start identical
    const SegmentId id1 = SegmentId::fromString("id-activity-1");
    const SegmentId id2 = SegmentId::fromString("id-activity-2");
    const SegmentId id3 = SegmentId::fromString("id-pause-1");
    const SegmentId id4 = SegmentId::fromString("id-pause-2");

    auto makeDeque = [&]() {
        std::deque<TimeDuration> d;
        // Two Activity segments with small gap (< 500ms) — should be merged (Branch 6)
        d.push_back(TimeDuration(DurationType::Activity,
            QDateTime::fromMSecsSinceEpoch(base, Qt::UTC),
            QDateTime::fromMSecsSinceEpoch(base + 1000, Qt::UTC), id1));
        d.push_back(TimeDuration(DurationType::Activity,
            QDateTime::fromMSecsSinceEpoch(base + 1200, Qt::UTC),
            QDateTime::fromMSecsSinceEpoch(base + 2000, Qt::UTC), id2));
        // Near-duplicate Pause entries (within 50ms diff) — should be de-duped (Branch 1)
        d.push_back(TimeDuration(DurationType::Pause,
            QDateTime::fromMSecsSinceEpoch(base + 3000, Qt::UTC),
            QDateTime::fromMSecsSinceEpoch(base + 4000, Qt::UTC), id3));
        d.push_back(TimeDuration(DurationType::Pause,
            QDateTime::fromMSecsSinceEpoch(base + 3010, Qt::UTC),
            QDateTime::fromMSecsSinceEpoch(base + 4020, Qt::UTC), id4));
        return d;
    };

    std::deque<TimeDuration> forClean = makeDeque();
    std::deque<TimeDuration> forTimeline = makeDeque();

    cleanDurations(&forClean);
    Timeline normed = Timeline(forTimeline, std::nullopt).normalized();

    const auto& normedComp = normed.completed();

    QCOMPARE(normedComp.size(), forClean.size());
    for (size_t i = 0; i < forClean.size(); ++i) {
        QCOMPARE(normedComp[i].segment_id, forClean[i].segment_id);
        QCOMPARE(normedComp[i].startTime, forClean[i].startTime);
        QCOMPARE(normedComp[i].endTime, forClean[i].endTime);
        QCOMPARE(normedComp[i].type, forClean[i].type);
        QCOMPARE(normedComp[i].duration, forClean[i].duration);
    }
}

// ============================================================================
// Test R — TimeDuration::create factory invariants
// ============================================================================

void TimelineTest::test_R_factory_rejects_cross_midnight()
{
    QDateTime start(QDate(2025, 6, 15), QTime(23, 30, 0), Qt::LocalTime);
    QDateTime end(QDate(2025, 6, 16), QTime(0, 30, 0), Qt::LocalTime);
    auto result = TimeDuration::create(DurationType::Activity, start, end);
    QVERIFY(!result.has_value());
}

void TimelineTest::test_R_factory_accepts_same_day()
{
    QDateTime start(QDate(2025, 6, 15), QTime(9, 0, 0), Qt::LocalTime);
    QDateTime end(QDate(2025, 6, 15), QTime(10, 0, 0), Qt::LocalTime);
    auto result = TimeDuration::create(DurationType::Activity, start, end);
    QVERIFY(result.has_value());
    QVERIFY(result->duration > 0);
}

void TimelineTest::test_R_factory_rejects_zero_duration()
{
    QDateTime dt(QDate(2025, 6, 15), QTime(9, 0, 0), Qt::LocalTime);
    auto result = TimeDuration::create(DurationType::Activity, dt, dt);
    QVERIFY(!result.has_value());
}

void TimelineTest::test_R_factory_rejects_negative_duration()
{
    QDateTime start(QDate(2025, 6, 15), QTime(10, 0, 0), Qt::LocalTime);
    QDateTime end(QDate(2025, 6, 15), QTime(9, 0, 0), Qt::LocalTime);
    auto result = TimeDuration::create(DurationType::Activity, start, end);
    QVERIFY(!result.has_value());
}

// ============================================================================
// Test S — normalized() cross-day merge guard
// ============================================================================

void TimelineTest::test_S_normalized_no_cross_day_merge()
{
    // Segment A ends at 2025-06-15 23:59:59.500, segment B starts at 2025-06-16 00:00:00.000.
    // Each is individually same-day (assertSameDayInvariant passes for both).
    // Branch 6 (gap = 500ms) should NOT merge them because they cross a day boundary.
    QDateTime aStart(QDate(2025, 6, 15), QTime(23, 0, 0), Qt::LocalTime);
    QDateTime aEnd(QDate(2025, 6, 15), QTime(23, 59, 59, 500), Qt::LocalTime);
    QDateTime bStart(QDate(2025, 6, 16), QTime(0, 0, 0), Qt::LocalTime);
    QDateTime bEnd(QDate(2025, 6, 16), QTime(1, 0, 0), Qt::LocalTime);

    auto segA = TimeDuration::create(DurationType::Activity, aStart, aEnd);
    auto segB = TimeDuration::create(DurationType::Activity, bStart, bEnd);
    QVERIFY(segA.has_value());
    QVERIFY(segB.has_value());

    std::deque<TimeDuration> comp;
    comp.push_back(std::move(*segA));
    comp.push_back(std::move(*segB));

    Timeline t(std::move(comp), std::nullopt);
    Timeline normed = t.normalized();

    QCOMPARE(normed.completed().size(), static_cast<size_t>(2));
}

void TimelineTest::test_S_normalized_same_day_still_merges()
{
    // Two same-type segments on the same day with gap = 100ms < 500ms — should merge.
    // aEnd = 10:00:01.000, bStart = 10:00:01.100, gap = 100ms
    QDateTime aStart(QDate(2025, 6, 15), QTime(10, 0, 0), Qt::LocalTime);
    QDateTime aEnd(QDate(2025, 6, 15), QTime(10, 0, 1, 0), Qt::LocalTime);
    QDateTime bStart(QDate(2025, 6, 15), QTime(10, 0, 1, 100), Qt::LocalTime);
    QDateTime bEnd(QDate(2025, 6, 15), QTime(10, 30, 0), Qt::LocalTime);

    auto segA = TimeDuration::create(DurationType::Activity, aStart, aEnd);
    auto segB = TimeDuration::create(DurationType::Activity, bStart, bEnd);
    QVERIFY(segA.has_value());
    QVERIFY(segB.has_value());

    std::deque<TimeDuration> comp;
    comp.push_back(std::move(*segA));
    comp.push_back(std::move(*segB));

    Timeline t(std::move(comp), std::nullopt);
    Timeline normed = t.normalized();

    QCOMPARE(normed.completed().size(), static_cast<size_t>(1));
}

// ============================================================================
// Test Q — groupByDate assigns segments to correct dates
// ============================================================================

void TimelineTest::test_Q_groupByDate()
{
    // Day 1: 2024-01-10
    const QDateTime day1Start(QDate(2024, 1, 10), QTime(9, 0, 0), Qt::UTC);
    const QDateTime day1End(QDate(2024, 1, 10), QTime(10, 0, 0), Qt::UTC);
    // Day 2: 2024-01-11
    const QDateTime day2Start(QDate(2024, 1, 11), QTime(14, 0, 0), Qt::UTC);
    const QDateTime day2End(QDate(2024, 1, 11), QTime(15, 30, 0), Qt::UTC);
    const QDateTime day2Start2(QDate(2024, 1, 11), QTime(16, 0, 0), Qt::UTC);
    const QDateTime day2End2(QDate(2024, 1, 11), QTime(17, 0, 0), Qt::UTC);

    std::deque<TimeDuration> comp;
    comp.push_back(TimeDuration(DurationType::Activity, day1Start, day1End));
    comp.push_back(TimeDuration(DurationType::Pause, day2Start, day2End));
    comp.push_back(TimeDuration(DurationType::Activity, day2Start2, day2End2));

    Timeline t(comp, std::nullopt);
    auto byDate = t.groupByDate();

    QCOMPARE(byDate.size(), 2);
    QVERIFY(byDate.contains(QDate(2024, 1, 10)));
    QVERIFY(byDate.contains(QDate(2024, 1, 11)));

    QCOMPARE(byDate[QDate(2024, 1, 10)].size(), static_cast<size_t>(1));
    QCOMPARE(byDate[QDate(2024, 1, 10)][0].startTime, day1Start);

    QCOMPARE(byDate[QDate(2024, 1, 11)].size(), static_cast<size_t>(2));
    QCOMPARE(byDate[QDate(2024, 1, 11)][0].startTime, day2Start);
    QCOMPARE(byDate[QDate(2024, 1, 11)][1].startTime, day2Start2);
}
