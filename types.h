#ifndef TYPES_H
#define TYPES_H

#include <QDateTime>

enum class Button {Start, Pause, Stop};

enum class LockEvent {None, Unlock, Lock, LongOngoingLock};

enum class DurationType { Activity, Pause };

struct TimeDuration {
    DurationType type;
    qint64 duration;
    QDateTime startTime;
    QDateTime endTime;

    // Primary constructor: explicit start and end times
    TimeDuration(DurationType type, QDateTime start, QDateTime end)
        : type(type), duration(start.msecsTo(end)), startTime(start), endTime(end) {
    }
};

enum class TransactionMode { Append, Replace };

#endif // TYPES_H
