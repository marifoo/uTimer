#include "mainwin.h"
#include <QtDebug>
#include <QTime>
#include <QSystemTrayIcon>
#include <QMessageBox>

MainWin::MainWin(Settings &settings, QWidget *parent /* = nullptr */)	: QMainWindow(parent), settings_(settings), warning_activity_shown_(false), warning_pause_shown_(false)
{
	content_widget_ = new ContentWidget(settings, this);
	setCentralWidget(content_widget_);
	QObject::connect(content_widget_, SIGNAL(pressedButton(Button)), this, SIGNAL(sendButtons(Button)));
	QObject::connect(content_widget_, SIGNAL(minToTray()), this, SLOT(minToTray()));
	QObject::connect(content_widget_, SIGNAL(toggleAlwaysOnTop()), this, SLOT(toggleAlwaysOnTop()));

	const QIcon icon(":/clock.png");
	setWindowIcon(icon);
	tray_icon_ = new QSystemTrayIcon(icon, this);
	tray_icon_->setToolTip("Timing Inactive");
	tray_icon_->show();
	QObject::connect(tray_icon_, SIGNAL(activated(QSystemTrayIcon::ActivationReason)), this, SLOT(iconActivated(QSystemTrayIcon::ActivationReason)));

	setWindowTitle("µTimer");
	setWindowFlags(windowFlags() &(~Qt::WindowMaximizeButtonHint));
}

void MainWin::updateAllTimes(qint64 t_active, qint64 t_pause)
{
	content_widget_->setAllTimes(t_active, t_pause);
	tray_icon_->setToolTip(content_widget_->getTooltip());

	if((content_widget_->isGUIinActivity()) && (settings_.showTooMuchActivityWarning() || settings_.showTooMuchActivityWarning()))
		showActivityWarnings(t_active, t_pause);
}

void MainWin::showActivityWarnings(const qint64 &t_active, const qint64 &t_pause)
{
	auto time_to_string = [](qint64 time)
	{
		return QDateTime::fromTime_t(time/1000).toUTC().toString("hh:mm:ss");
	};

	if ((!warning_activity_shown_)
			&& (t_active > settings_.getWarnTimeActivityMsec())) {
		warning_activity_shown_ = true;
		showMsgBox("Total activity time: " + time_to_string(t_active));
	}

	if ((!warning_pause_shown_)
			&& (t_active > settings_.getWarnTimeNoPauseMsec())
			&& (t_pause < settings_.getPauseTimeForWarnTimeNoPauseMsec())) {
		warning_pause_shown_ = true;
		showMsgBox("Pause time: " + time_to_string(t_pause) + "\nwith activity time: " + time_to_string(t_active));
	}
}

void MainWin::showMsgBox(QString text)
{
	activateWindow();
	show();
	QMessageBox msgBox(this);
	msgBox.setWindowTitle("µTimer Warning");
	msgBox.setText(text);
	msgBox.setIcon(QMessageBox::Warning);
	msgBox.exec();
}

void MainWin::reactOnLockState(LockEvent event)
{
	if (event == LockEvent::Unlock)
		content_widget_->setGUItoActivity();
	else if (event == LockEvent::LongOngoingLock)
		content_widget_->setGUItoPause();
}

void MainWin::iconActivated(QSystemTrayIcon::ActivationReason reason)
{
	if (reason != QSystemTrayIcon::DoubleClick)
		return;

	if (isVisible())
		minToTray();
	else
		showMainWin();
}

void MainWin::minToTray()
{
	hide();
}

void MainWin::toggleAlwaysOnTop()
{
	toggleAlwaysOnTopFlag();
	showMainWin();
}

void MainWin::showMainWin()
{
	activateWindow();
	show();
}

void MainWin::toggleAlwaysOnTopFlag()
{
	setWindowFlags(windowFlags() ^ (Qt::CustomizeWindowHint | Qt::WindowStaysOnTopHint));
}

void MainWin::start()
{
	if (settings_.isPinnedStartEnabled())
		toggleAlwaysOnTopFlag();

	if(settings_.isMinimizedStartEnabled())
		minToTray();
	else
		showMainWin();

	resize(0,0);

	if (settings_.isAutostartTimingEnabled())
		content_widget_->pressedStartPauseButton();

	warning_activity_shown_ = !settings_.showTooMuchActivityWarning();
	warning_pause_shown_ = !settings_.showNoPauseWarning();
}




