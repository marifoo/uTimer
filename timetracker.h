#ifndef TIMETRACKER_H
#define TIMETRACKER_H

#include <QObject>
#include <QtGlobal>
#include <QElapsedTimer>
#include <vector>
#include <memory>
#include "settings.h"
#include "types.h"


class TimeTracker : public QObject
{
	Q_OBJECT
private:
	enum class Mode {Activity, Pause, None};

	std::unique_ptr<QElapsedTimer> timer_;
	std::vector<qint64> activities_;
	std::vector<qint64> pauses_;
	Mode mode_;
	const Settings & settings_;

	qint64 getActiveTime() const;
	qint64 getPauseTime() const;

	void startTimer();
	void stopTimer();
	void pauseTimer();
	void backpauseTimer();

public:
	explicit TimeTracker(const Settings & settings, QObject *parent = nullptr);

signals:
	void sendAllTimes(qint64 t_active, qint64 t_pause);

public slots:
	void useTimerViaButton(Button button);
	void useTimerViaLockEvent(LockEvent event);
	void sendTimes();
};

#endif // TIMETRACKER_H
