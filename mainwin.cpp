#include "mainwin.h"
#include <QtDebug>
#include <QTime>
#include <QSystemTrayIcon>
#include <QMessageBox>

MainWin::MainWin(Settings &settings, QWidget *parent) : QMainWindow(parent), settings_(settings), warning_activity_shown(false), warning_pause_shown(false)
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
	QString t_active_string = QDateTime::fromTime_t(t_active/1000).toUTC().toString("hh:mm:ss");
	QString t_pause_string = QDateTime::fromTime_t(t_pause/1000).toUTC().toString("hh:mm:ss");

	content_widget_->setAllTimes(t_active_string, t_pause_string);
	tray_icon_->setToolTip(content_widget_->getTooltip());

	if ((!warning_activity_shown) && (t_active > settings_.getWarnTimeActivityMsec())) {
		warning_activity_shown = true;
		showMsgBox("Total activity time: " + t_active_string);
	}

	if ((!warning_pause_shown) && (t_active > settings_.getWarnTimeNoPauseMsec()) && (t_pause < settings_.getPauseTimeForWarnTimeNoPauseMsec())) {
		warning_pause_shown = true;
		showMsgBox("Pause time: " + t_pause_string + "\nwith activity time: " + t_active_string);
	}
}

void MainWin::showMsgBox(QString text)
{
	activateWindow();
	show();
	QMessageBox msgBox;
	msgBox.setText(text);
	msgBox.setIcon(QMessageBox::Warning);
	msgBox.exec();
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
	if (settings_.isPinnedStartEnabled())
		setWindowFlags(windowFlags() ^ (Qt::CustomizeWindowHint | Qt::WindowStaysOnTopHint));

	if(settings_.isMinimizedStartEnabled())
		minToTray();
	else
		show();

	if (settings_.isAutostartTimingEnabled())
		content_widget_->pressedStartPauseButton();

	warning_activity_shown = !settings_.showTooMuchActivityWarning();
	warning_pause_shown = !settings_.showNoPauseWarning();
}

void MainWin::reactOnLockState(LockEvent event)
{
	if (event == LockEvent::Unlock)
		content_widget_->setGUItoActivity();
	else if (event == LockEvent::LongLock)
		content_widget_->setGUItoPause();
}

