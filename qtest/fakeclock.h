/**
 * FakeClock -- deterministic clock for tests.
 *
 * Provides a controllable wall-clock time that starts at a given instant
 * and can be advanced by an explicit amount.  Useful for constructing
 * timestamps without sleeping (QTest::qWait) and for writing tests that
 * are independent of the real system clock.
 *
 * Usage:
 *     FakeClock clock(QDateTime(QDate(2025, 6, 1), QTime(9, 0, 0)));
 *     QDateTime t1 = clock.now();            // 2025-06-01 09:00:00
 *     clock.advance(5000);                   // advance 5 seconds
 *     QDateTime t2 = clock.now();            // 2025-06-01 09:00:05
 */

#ifndef FAKECLOCK_H
#define FAKECLOCK_H

#include <QDateTime>

class FakeClock
{
public:
    /// Construct with a starting wall-clock time.
    explicit FakeClock(const QDateTime& start = QDateTime::currentDateTime())
        : now_(start)
    {}

    /// Returns the current fake time.
    QDateTime now() const { return now_; }

    /// Advances the clock by the given number of milliseconds.
    void advance(qint64 msec) { now_ = now_.addMSecs(msec); }

    /// Sets the clock to an absolute time.
    void setNow(const QDateTime& dt) { now_ = dt; }

private:
    QDateTime now_;
};

#endif // FAKECLOCK_H
