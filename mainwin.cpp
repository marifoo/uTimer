#include "mainwin.h"
#include <QtDebug>
#include <QTime>
#include <QSystemTrayIcon>

MainWin::MainWin(Settings &settings, QWidget *parent) : QMainWindow(parent), settings_(settings)
{
	content_widget_ = std::make_unique<ContentWidget>(settings);
	setCentralWidget(content_widget_.get());
	QObject::connect(content_widget_.get(), SIGNAL(pressedButton(Button)), this, SIGNAL(sendButtons(Button)));
	QObject::connect(content_widget_.get(), SIGNAL(minToTray()), this, SLOT(minToTray()));
	QObject::connect(content_widget_.get(), SIGNAL(toggleAlwaysOnTop()), this, SLOT(toggleAlwaysOnTop()));

	QIcon icon(":/clock.png");
	setWindowIcon(icon);
	tray_icon_ = std::make_unique<QSystemTrayIcon>(icon);
	tray_icon_->setToolTip("Timing Inactive");
	tray_icon_->show();
	QObject::connect(tray_icon_.get(), SIGNAL(activated(QSystemTrayIcon::ActivationReason)), this, SLOT(iconActivated(QSystemTrayIcon::ActivationReason)));

	resize(280, 1);
	setWindowTitle("µTimer");
	setWindowFlags(windowFlags() &(~Qt::WindowMaximizeButtonHint));
}

void MainWin::updateAllTimes(qint64 t_active, qint64 t_pause)
{
	content_widget_->setAllTimes(QDateTime::fromTime_t(t_active/1000).toUTC().toString("hh:mm:ss"),
															 QDateTime::fromTime_t(t_pause/1000).toUTC().toString("hh:mm:ss"));
	tray_icon_->setToolTip(content_widget_->getTooltip());
}

void MainWin::iconActivated(QSystemTrayIcon::ActivationReason reason)
{
	if (reason == QSystemTrayIcon::DoubleClick) {
		if (isVisible()) {
			minToTray();
		}
		else {
			activateWindow();
			show();
		}
	}
}

void MainWin::minToTray()
{
	hide();
}

void MainWin::toggleAlwaysOnTop()
{
	setWindowFlags(windowFlags() ^ (Qt::CustomizeWindowHint | Qt::WindowStaysOnTopHint));
	activateWindow();
	show();
}

void MainWin::start()
{
	if(settings_.isMinimizedStartEnabled())
		minToTray();
	else
		show();

	if (settings_.isAutostartTimingEnabled())
		content_widget_->pressedStartPauseButton();
}

void MainWin::reactOnLockState(LockEvent event)
{
	if (event == LockEvent::Unlock)
		content_widget_->setGUItoActivity();
	else if (event == LockEvent::LongLock)
		content_widget_->setGUItoPause();
}

