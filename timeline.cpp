#include "timeline.h"
#include "logger.h"
#include <algorithm>
#include <cmath>
#include <cassert>

Timeline::Timeline()
{
}

Timeline::Timeline(std::deque<TimeDuration> completed,
                   std::optional<TimeDuration> ongoing)
    : completed_(std::move(completed)), ongoing_(std::move(ongoing))
{
#ifndef QT_NO_DEBUG
    assertSameDayInvariant();
#endif
}

Timeline Timeline::fromUnchecked(std::deque<TimeDuration> completed,
                                 std::optional<TimeDuration> ongoing)
{
    Timeline t;
    t.completed_ = std::move(completed);
    t.ongoing_ = std::move(ongoing);
    return t;
}

const std::deque<TimeDuration>& Timeline::completed() const
{
    return completed_;
}

const std::optional<TimeDuration>& Timeline::ongoing() const
{
    return ongoing_;
}

qint64 Timeline::activeMsec() const
{
    qint64 sum = 0;
    for (const auto& d : completed_) {
        if (d.type == DurationType::Activity)
            sum += d.duration;
    }
    if (ongoing_.has_value() && ongoing_->type == DurationType::Activity)
        sum += ongoing_->duration;
    return sum;
}

qint64 Timeline::pauseMsec() const
{
    qint64 sum = 0;
    for (const auto& d : completed_) {
        if (d.type == DurationType::Pause)
            sum += d.duration;
    }
    if (ongoing_.has_value() && ongoing_->type == DurationType::Pause)
        sum += ongoing_->duration;
    return sum;
}

QMap<QDate, std::vector<TimeDuration>> Timeline::groupByDate() const
{
    QMap<QDate, std::vector<TimeDuration>> result;
    for (const auto& d : completed_) {
        result[d.startTime.date()].push_back(d);
    }
    return result;
}

Timeline Timeline::withSegmentType(size_t index, DurationType t) const
{
    assert(index < completed_.size());
    Timeline copy(*this);
    copy.completed_[index].type = t;
    return copy;
}

Timeline Timeline::withSplit(size_t index, QDateTime at,
                              DurationType first, DurationType second) const
{
    assert(index < completed_.size());
    Timeline copy(*this);
    const TimeDuration& original = copy.completed_[index];
    QDateTime origStart = original.startTime;
    QDateTime origEnd = original.endTime;
    SegmentId origId = original.segment_id;

    auto firstHalf = TimeDuration::create(first, origStart, at, origId);
    if (!firstHalf.has_value()) {
        Logger::Log("[SPLIT] Cross-day first half, aborting split");
        return *this;
    }
    auto secondHalf = TimeDuration::create(second, at, origEnd, SegmentId::mint());
    if (!secondHalf.has_value()) {
        Logger::Log("[SPLIT] Cross-day second half, aborting split");
        return *this;
    }

    copy.completed_.erase(copy.completed_.begin() + static_cast<std::ptrdiff_t>(index));
    copy.completed_.insert(copy.completed_.begin() + static_cast<std::ptrdiff_t>(index), std::move(*secondHalf));
    copy.completed_.insert(copy.completed_.begin() + static_cast<std::ptrdiff_t>(index), std::move(*firstHalf));

    return copy;
}

#ifndef QT_NO_DEBUG
void Timeline::assertSameDayInvariant() const
{
    for (const auto& d : completed_) {
        Q_ASSERT(!isCrossMidnight(d.startTime, d.endTime));
    }
}
#endif

// Thresholds used by the normalized() merge loop (branches 1, 6, 7).
// Two segments whose endpoints differ by less than this are treated as the
// same segment (clock jitter / duplicate saves).
static constexpr qint64 kNearDuplicateMs = 50;
// A gap this small between same-type segments is noise — merge them.
static constexpr qint64 kSmallGapMergeMs = 500;
// A negative gap (overlap) this small is likely a rounding artefact — merge.
static constexpr qint64 kSlightOverlapMs = 100;

Timeline Timeline::normalized() const
{
    // Precondition: input entries are sorted by (start, end) — the sort below
    // establishes this.  Branches 1–7 are mutually exclusive and order-dependent:
    // each branch must be checked before the next because later branches assume
    // the earlier ones did not match.
    std::deque<TimeDuration> durations = completed_;

    if (durations.size() < 2) {
        return Timeline(durations, ongoing_);
    }

    std::sort(durations.begin(), durations.end(), [](const TimeDuration& a, const TimeDuration& b) {
        const qint64 a_start = a.startTime.toMSecsSinceEpoch();
        const qint64 b_start = b.startTime.toMSecsSinceEpoch();
        if (a_start != b_start) return a_start < b_start;
        const qint64 a_end = a.endTime.toMSecsSinceEpoch();
        const qint64 b_end = b.endTime.toMSecsSinceEpoch();
        if (a_end != b_end) return a_end < b_end;
        return a.duration < b.duration;
    });

    for (auto it = durations.begin() + 1; it != durations.end(); ) {
        auto prevIt = std::prev(it);

        if (prevIt->type == it->type) {
            const qint64 prev_start = prevIt->startTime.toMSecsSinceEpoch();
            const qint64 it_start = it->startTime.toMSecsSinceEpoch();
            const qint64 prev_end = prevIt->endTime.toMSecsSinceEpoch();
            const qint64 it_end = it->endTime.toMSecsSinceEpoch();

            const qint64 diff_end = prev_end - it_end;
            const qint64 diff_dur = prevIt->duration - it->duration;
            const qint64 gap = it_start - prev_end;

            // Branch 1: near-duplicate — erase it
            if (std::abs(diff_end) < kNearDuplicateMs && std::abs(diff_dur) < kNearDuplicateMs) {
                it = durations.erase(it);
                continue;
            }

            // Branch 2: current starts before prev (shorter) — adopt current fields
            if (it_start < prev_start && prev_end <= it_end) {
                prevIt->segment_id = it->segment_id;
                prevIt->startTime = it->startTime;
                prevIt->endTime = it->endTime;
                prevIt->duration = it->duration;
                it = durations.erase(it);
                continue;
            }

            // Branch 3: current starts before prev (longer) — join
            if (it_start < prev_start && it_end < prev_end && it_start < prev_end) {
                prevIt->segment_id = it->segment_id;
                prevIt->startTime = it->startTime;
                prevIt->duration = prev_end - it_start;
                it = durations.erase(it);
                continue;
            }

            // Branch 4: overlap extend forward
            if (prev_start <= it_start && it_start <= prev_end && prev_end <= it_end) {
                prevIt->endTime = it->endTime;
                prevIt->duration = it_end - prev_start;
                it = durations.erase(it);
                continue;
            }

            // Branch 5: subset — erase it
            if (prev_start <= it_start && it_start <= prev_end && it_end <= prev_end) {
                it = durations.erase(it);
                continue;
            }

            const bool crossesDay = isCrossMidnight(prevIt->endTime, it->startTime);

            // Branch 6: small gap merge
            if (!crossesDay && gap >= 0 && gap < kSmallGapMergeMs) {
                prevIt->endTime = it->endTime;
                prevIt->duration = prevIt->startTime.msecsTo(prevIt->endTime);
                it = durations.erase(it);
                continue;
            }

            // Branch 7: slight overlap merge
            if (!crossesDay && gap < 0 && std::abs(gap) < kSlightOverlapMs) {
                prevIt->endTime = it->endTime;
                prevIt->duration = prevIt->startTime.msecsTo(prevIt->endTime);
                it = durations.erase(it);
                continue;
            }
        }
        ++it;
    }

    Timeline result(durations, ongoing_);
#ifndef QT_NO_DEBUG
    result.assertSameDayInvariant();
#endif
    return result;
}

NormalizedResult Timeline::normalizedWithRemovedIds() const
{
    std::vector<QString> before;
    before.reserve(completed_.size());
    for (const auto& d : completed_)
        before.push_back(d.segment_id.toString());

    Timeline normed = normalized();

    std::vector<QString> removed;
    for (const auto& id : before) {
        bool found = false;
        for (const auto& d : normed.completed_)
            if (d.segment_id.toString() == id) { found = true; break; }
        if (!found)
            removed.push_back(id);
    }

    return {normed, removed};
}
