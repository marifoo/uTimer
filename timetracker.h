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
#include "databasemanager.h"



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
	DatabaseManager db_;

	void startTimer();
	void stopTimer();
	void pauseTimer();
	void backpauseTimer();
	
public:
	explicit TimeTracker(const Settings & settings, QObject *parent = nullptr);
	~TimeTracker();

	qint64 getActiveTime() const;
	qint64 getPauseTime() const;
	const std::deque<TimeDuration>& getCurrentDurations() const;
	void setCurrentDurations(const std::deque<TimeDuration>& newDurations);
	std::deque<TimeDuration> getDurationsHistory();
    void setDurationType(size_t idx, DurationType type);
	bool appendDurationsToDB();
	bool replaceDurationsInDB(std::deque<TimeDuration> durations);
	bool hasEntriesForToday();

public slots:
	void useTimerViaButton(Button button);
	void useTimerViaLockEvent(LockEvent event);
};

#endif // TIMETRACKER_H
