#ifndef TIMELINE_H
#define TIMELINE_H

#include <deque>
#include <optional>
#include <vector>
#include <QDate>
#include <QMap>
#include <QDateTime>
#include "types.h"

class Timeline {
public:
    Timeline();
    Timeline(std::deque<TimeDuration> completed,
             std::optional<TimeDuration> ongoing);

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

private:
    std::deque<TimeDuration> completed_;
    std::optional<TimeDuration> ongoing_;
};

#endif // TIMELINE_H
