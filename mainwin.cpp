#include "mainwin.h"
#include <QtDebug>
#include <QTime>
#include <QSystemTrayIcon>
#include <QMessageBox>

MainWin::MainWin(Settings &settings, QWidget *parent /* = nullptr */) : QMainWindow(parent), settings_(settings), warning_activity_shown(false), warning_pause_shown(false)
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

	const QString t_active_string = QDateTime::fromTime_t(t_active/1000).toUTC().toString("hh:mm:ss");
	const QString t_pause_string = QDateTime::fromTime_t(t_pause/1000).toUTC().toString("hh:mm:ss");

	if ((!warning_activity_shown) && (content_widget_->isGUIinActivity())
			&& (t_active > settings_.getWarnTimeActivityMsec())) {
		warning_activity_shown = true;
		showMsgBox("Total activity time: " + t_active_string);
	}

	if ((!warning_pause_shown) && (content_widget_->isGUIinActivity())
			&& (t_active > settings_.getWarnTimeNoPauseMsec()) && (t_pause < settings_.getPauseTimeForWarnTimeNoPauseMsec())) {
		warning_pause_shown = true;
		showMsgBox("Pause time: " + t_pause_string + "\nwith activity time: " + t_active_string);
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

	warning_activity_shown = !settings_.showTooMuchActivityWarning();
	warning_pause_shown = !settings_.showNoPauseWarning();
}

void MainWin::reactOnLockState(LockEvent event)
{
	if (event == LockEvent::Unlock)
		content_widget_->setGUItoActivity();
	else if (event == LockEvent::LongOngoingLock)
		content_widget_->setGUItoPause();
}



