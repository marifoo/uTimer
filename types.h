#ifndef TYPES_H
#define TYPES_H

#include <QDateTime>

enum class Button {Start, Pause, Stop};

enum class LockEvent {None, Unlock, Lock, LongOngoingLock};

enum class DurationType { Activity, Pause };

struct TimeDuration {
    DurationType type;
    qint64 duration;
    QDateTime endTime;

    TimeDuration(DurationType type, qint64 dur, QDateTime end)
        : type(type), duration(dur), endTime(end) {
    }
};

enum class TransactionMode { Append, Replace };

#endif // TYPES_H
