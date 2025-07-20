#ifndef TIMETRACKER_H
#define TIMETRACKER_H

#include <QObject>
#include <QtGlobal>
#include <QElapsedTimer>
#include <QDateTime>
#include <deque>
#include <memory>
#include "settings.h"
#include "types.h"

enum class DurationType { Activity, Pause };

struct TimeDuration {
	DurationType type;
    qint64 duration;
    QDateTime endTime;
    
    TimeDuration(DurationType type, qint64 dur, QDateTime end)
        : type(type) ,duration(dur), endTime(end) {}
};

class TimeTracker : public QObject
{
	Q_OBJECT
private:
	enum class Mode {Activity, Pause, None};

	const Settings & settings_;
	QElapsedTimer timer_;
	std::deque<TimeDuration> durations_;
	Mode mode_;
	bool was_active_before_autopause_;

	void startTimer();
	void stopTimer();
	void pauseTimer();
	void backpauseTimer();

public:
	explicit TimeTracker(const Settings & settings, QObject *parent = nullptr);
	~TimeTracker();

	qint64 getActiveTime() const;
	qint64 getPauseTime() const;
	const std::deque<TimeDuration>& getDurations() const;
    void setDurationType(size_t idx, DurationType type); // Add setter

signals:
	void sendAllTimes(qint64 t_active, qint64 t_pause);

public slots:
	void useTimerViaButton(Button button);
	void useTimerViaLockEvent(LockEvent event);
	void sendTimes();
};

#endif // TIMETRACKER_H
