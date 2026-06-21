#ifndef TIMELINE_H
#define TIMELINE_H

#include <deque>
#include <optional>
#include <vector>
#include <QDate>
#include <QMap>
#include <QDateTime>
#include <QString>
#include "types.h"

struct NormalizedResult;

class Timeline {
public:
    Timeline();
    Timeline(std::deque<TimeDuration> completed,
             std::optional<TimeDuration> ongoing);

    // Factory for Timelines built from persisted DB rows: bypasses the same-day invariant
    // check because DB rows are already stored and may legitimately include cross-midnight
    // entries that must be preserved as-is.
    static Timeline fromPersistedRows(std::deque<TimeDuration> completed,
                                      std::optional<TimeDuration> ongoing = std::nullopt);

    const std::deque<TimeDuration>& completed() const;
    const std::optional<TimeDuration>& ongoing() const;

    // Sums durations by type across completed + ongoing
    qint64 activeMsec() const;
    qint64 pauseMsec() const;

    // Groups completed segments by start date
    QMap<QDate, std::vector<TimeDuration>> groupByDate() const;

    // Pure editing operations — return new Timeline, do not modify *this
    Timeline withSegmentType(size_t index, DurationType t) const;
    Timeline withSplit(size_t index, QDateTime at,
                       DurationType first, DurationType second) const;

    // Normalization: equivalent to cleanDurations from helpers.cpp
    Timeline normalized() const;

    /// Normalizes and returns both the merged timeline and the segment_id strings
    /// of completed segments that were merged away during normalization.
    NormalizedResult normalizedWithRemovedIds() const;

#ifndef QT_NO_DEBUG
    void assertSameDayInvariant() const;
#endif

private:
    std::deque<TimeDuration> completed_;
    std::optional<TimeDuration> ongoing_;
};

struct NormalizedResult {
    Timeline timeline;
    std::vector<QString> removedIds;
};

/// Merge decision returned by classifyMerge().
/// Describes what normalized() should do with the (prev, curr) pair.
enum class MergeDecision {
    None,            ///< No merge — types differ or no branch matched; advance iterator.
    EraseIt,         ///< Erase curr (prev is unchanged, e.g. near-duplicate or subset).
    Supersede,       ///< curr supersedes prev: copy curr's fields to prev, then erase curr.
    ExtendPrevStart, ///< Extend prev's start to curr's start (join), then erase curr.
    ExtendPrevEnd    ///< Extend prev's end to curr's end (overlap/gap merge), then erase curr.
};

/// Classify how prev and curr should be merged in normalized().
/// Only meaningful when prev.type == curr.type; returns None for differing types.
/// Branches are order-dependent and mirror the normalized() merge loop exactly.
MergeDecision classifyMerge(const TimeDuration& prev, const TimeDuration& curr);

#endif // TIMELINE_H
