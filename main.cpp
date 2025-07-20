#include <QApplication>
#include <QStyleFactory>
#include <QDebug>
#include <QEvent>
#include <QTimer>
#include <QSettings>

#include "settings.h"
#include "mainwin.h"
#include "timetracker.h"
#include "lockstatewatcher.h"
#include "types.h"

int main(int argc, char *argv[])
{
	QCoreApplication::setApplicationName("µTimer");

	QApplication application(argc, argv);
	application.setStyle(QStyleFactory::create("Fusion"));

	QTimer timer;

	Settings settings("user-settings.ini");
	LockStateWatcher lockstate_watcher(settings);
	TimeTracker time_tracker(settings);
	MainWin main_win(settings, time_tracker);

	QObject::connect(&main_win, SIGNAL(sendButtons(Button)), &time_tracker, SLOT(useTimerViaButton(Button)));

	QObject::connect(&timer, SIGNAL(timeout()), &main_win, SLOT(update()));

	QObject::connect(&timer, SIGNAL(timeout()), &lockstate_watcher, SLOT(update()));
	QObject::connect(&lockstate_watcher, SIGNAL(desktopLockEvent(LockEvent)),	&time_tracker, SLOT(useTimerViaLockEvent(LockEvent)));
	QObject::connect(&lockstate_watcher, SIGNAL(desktopLockEvent(LockEvent)), &main_win, SLOT(reactOnLockState(LockEvent)));

	timer.setInterval(100);
	timer.start();

	main_win.start();

	return application.exec();
}
