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

Timeline Timeline::fromPersistedRows(std::deque<TimeDuration> completed,
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

// Thresholds used by the classifyMerge() branches 1, 6, 7.
// Two segments whose endpoints differ by less than this are treated as the
// same segment (clock jitter / duplicate saves).
static constexpr qint64 kNearDuplicateMs = 50;
// A gap this small between same-type segments is noise — merge them.
static constexpr qint64 kSmallGapMergeMs = 500;
// A negative gap (overlap) this small is likely a rounding artefact — merge.
static constexpr qint64 kSlightOverlapMs = 100;

/**
 * Classify how prev and curr should be merged during normalization.
 *
 * Returns MergeDecision::None when the types differ or when no branch applies.
 * Branches 1–7 are mutually exclusive and order-dependent: each is checked
 * only after the earlier ones have been ruled out.
 *
 *  1. Near-duplicate  → EraseIt          (same endpoint within 50 ms)
 *  2. curr supersedes → Supersede        (curr starts earlier and is at least as long)
 *  3. curr joins prev → ExtendPrevStart  (curr starts earlier but ends before prev)
 *  4. Forward overlap → ExtendPrevEnd    (curr starts inside prev and extends past it)
 *  5. Subset          → EraseIt          (curr is contained within prev)
 *  6. Small gap       → ExtendPrevEnd    (gap < 500 ms, same calendar day)
 *  7. Slight overlap  → ExtendPrevEnd    (overlap < 100 ms, same calendar day)
 */
MergeDecision classifyMerge(const TimeDuration& prev, const TimeDuration& curr)
{
    if (prev.type != curr.type)
        return MergeDecision::None;

    const qint64 prev_start = prev.startTime.toMSecsSinceEpoch();
    const qint64 curr_start = curr.startTime.toMSecsSinceEpoch();
    const qint64 prev_end   = prev.endTime.toMSecsSinceEpoch();
    const qint64 curr_end   = curr.endTime.toMSecsSinceEpoch();

    const qint64 diff_end  = prev_end - curr_end;
    const qint64 diff_dur  = prev.duration - curr.duration;
    const qint64 gap       = curr_start - prev_end;

    // Branch 1: near-duplicate
    if (std::abs(diff_end) < kNearDuplicateMs && std::abs(diff_dur) < kNearDuplicateMs)
        return MergeDecision::EraseIt;

    // Branch 2: curr supersedes prev (starts earlier, at least as long)
    if (curr_start < prev_start && prev_end <= curr_end)
        return MergeDecision::Supersede;

    // Branch 3: curr starts before prev but ends inside it — join
    if (curr_start < prev_start && curr_end < prev_end && curr_start < prev_end)
        return MergeDecision::ExtendPrevStart;

    // Branch 4: overlap extending forward
    if (prev_start <= curr_start && curr_start <= prev_end && prev_end <= curr_end)
        return MergeDecision::ExtendPrevEnd;

    // Branch 5: curr is a subset of prev
    if (prev_start <= curr_start && curr_start <= prev_end && curr_end <= prev_end)
        return MergeDecision::EraseIt;

    const bool crossesDay = isCrossMidnight(prev.endTime, curr.startTime);

    // Branch 6: small gap between same-day segments
    if (!crossesDay && gap >= 0 && gap < kSmallGapMergeMs)
        return MergeDecision::ExtendPrevEnd;

    // Branch 7: slight overlap between same-day segments
    if (!crossesDay && gap < 0 && std::abs(gap) < kSlightOverlapMs)
        return MergeDecision::ExtendPrevEnd;

    return MergeDecision::None;
}

/// Apply a merge decision to the (prev, curr) iterator pair.
/// Erases curr when the decision is not None, returning the new valid iterator.
static std::deque<TimeDuration>::iterator applyMergeDecision(
    MergeDecision decision,
    std::deque<TimeDuration>::iterator prevIt,
    std::deque<TimeDuration>::iterator it,
    std::deque<TimeDuration>& durations)
{
    const qint64 curr_end = it->endTime.toMSecsSinceEpoch();

    switch (decision) {
    case MergeDecision::EraseIt:
        return durations.erase(it);

    case MergeDecision::Supersede:
        prevIt->segment_id = it->segment_id;
        prevIt->startTime  = it->startTime;
        prevIt->endTime    = it->endTime;
        prevIt->duration   = it->duration;
        return durations.erase(it);

    case MergeDecision::ExtendPrevStart: {
        const qint64 prev_end = prevIt->endTime.toMSecsSinceEpoch();
        prevIt->segment_id = it->segment_id;
        prevIt->startTime  = it->startTime;
        prevIt->duration   = prev_end - it->startTime.toMSecsSinceEpoch();
        return durations.erase(it);
    }

    case MergeDecision::ExtendPrevEnd:
        prevIt->endTime  = it->endTime;
        prevIt->duration = prevIt->startTime.msecsTo(prevIt->endTime);
        return durations.erase(it);

    case MergeDecision::None:
        return std::next(it);
    }
    Q_UNREACHABLE();
}

Timeline Timeline::normalized() const
{
    // Precondition: input entries are sorted by (start, end) — the sort below
    // establishes this.  classifyMerge() branches 1–7 are mutually exclusive and
    // order-dependent: each is checked only after earlier ones have been ruled out.
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
        const MergeDecision decision = classifyMerge(*prevIt, *it);
        if (decision == MergeDecision::None) {
            ++it;
        } else {
            it = applyMergeDecision(decision, prevIt, it, durations);
        }
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
