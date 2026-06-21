#ifndef TYPES_H
#define TYPES_H

#include <optional>
#include <QDateTime>
#include <QString>
#include <QUuid>

enum class Button {Start, Pause, Stop};

enum class LockEvent {None, Unlock, Lock, LongOngoingLock};

enum class DurationType { Activity, Pause };

/**
 * Opaque, immutable segment identity.
 *
 * SegmentId wraps a UUID string to prevent empty-string IDs from
 * propagating undetected.  fromString() asserts non-empty in debug
 * builds; mint() always produces a fresh non-empty UUID.
 *
 * The default constructor produces an empty SegmentId (used when no
 * segment is active, e.g. Mode::None in Timer).
 */
class SegmentId {
public:
    SegmentId() = default;

    static SegmentId mint() {
        return SegmentId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    }

    static SegmentId fromString(const QString& s) {
        Q_ASSERT_X(!s.isEmpty(), "SegmentId::fromString", "segment_id must not be empty");
        return SegmentId(s);
    }

    bool isEmpty() const { return value_.isEmpty(); }
    const QString& toString() const { return value_; }

    bool operator==(const SegmentId& o) const { return value_ == o.value_; }
    bool operator!=(const SegmentId& o) const { return value_ != o.value_; }
    bool operator<(const SegmentId& o) const { return value_ < o.value_; }

private:
    explicit SegmentId(const QString& s) : value_(s) {}
    QString value_;
};

inline bool isCrossMidnight(const QDateTime& a, const QDateTime& b);

struct TimeDuration {
    SegmentId segment_id;
    DurationType type;
    qint64 duration;
    QDateTime startTime;
    QDateTime endTime;

    static SegmentId createSegmentId() { return SegmentId::mint(); }

    // Factory: returns nullopt for cross-midnight, zero/negative duration, or invalid timestamps.
    static std::optional<TimeDuration> create(DurationType type, QDateTime start, QDateTime end,
                                               SegmentId segmentId = SegmentId{})
    {
        if (!start.isValid() || !end.isValid())
            return std::nullopt;
        if (start.msecsTo(end) <= 0)
            return std::nullopt;
        if (isCrossMidnight(start, end))
            return std::nullopt;
        return TimeDuration(type, start, end, std::move(segmentId));
    }

    // For transient (non-stored) durations that may legitimately cross day boundaries.
    static TimeDuration fromTrusted(DurationType type, QDateTime start, QDateTime end,
                                    SegmentId segmentId = SegmentId{})
    {
        return TimeDuration(type, start, end, std::move(segmentId));
    }

    // Raw constructor — bypasses create()'s validation guards (same-day, positive duration).
    // Prefer create() for production-path data; use fromTrusted() or this directly only
    // when the caller guarantees validity (e.g. loading from DB, test seeding).
    explicit TimeDuration(DurationType type, QDateTime start, QDateTime end, SegmentId segmentId = SegmentId{})
        : segment_id(segmentId.isEmpty() ? SegmentId::mint() : std::move(segmentId)),
          type(type), duration(start.msecsTo(end)), startTime(start), endTime(end) {
    }

};

inline bool isCrossMidnight(const QDateTime& a, const QDateTime& b) { return a.date() != b.date(); }

/**
 * Tri-state result for hasEntriesForDate.
 *
 * Distinguishes between "definitely no entries" (safe to add boot time)
 * and "we don't know" (history disabled or DB inaccessible — never add
 * boot time to avoid double-counting).
 *
 * - Yes:     Finalized entries exist for the given date.
 * - No:      The DB was queried successfully and zero entries were found.
 * - Unknown: The DB could not be queried (history disabled or open failed).
 */
enum class EntriesForDateResult { Yes, No, Unknown };

#endif // TYPES_H
