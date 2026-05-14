#ifndef TYPES_H
#define TYPES_H

#include <QDateTime>
#include <QString>
#include <QUuid>

enum class Button {Start, Pause, Stop};

enum class LockEvent {None, Unlock, Lock, LongOngoingLock};

enum class DurationType { Activity, Pause };

struct TimeDuration {
    QString segment_id;
    DurationType type;
    qint64 duration;
    QDateTime startTime;
    QDateTime endTime;

    static QString createSegmentId()
    {
        return QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    // Primary constructor: explicit start and end times
    TimeDuration(DurationType type, QDateTime start, QDateTime end, const QString& segmentId = QString())
        : segment_id(segmentId.isEmpty() ? createSegmentId() : segmentId),
          type(type), duration(start.msecsTo(end)), startTime(start), endTime(end) {
    }
};

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
