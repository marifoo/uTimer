#include "mainwin.h"
#include <QtDebug>
#include <QTime>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include "logger.h"
#include "helpers.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

MainWin::MainWin(Settings& settings, TimeTracker& timetracker, QWidget *parent)	
	: QMainWindow(parent), settings_(settings), timetracker_(timetracker), warning_pause_shown_(false), was_active_before_autopause_(false)
{
	setupCentralWidget(settings, timetracker);

	setupIcon();

	setWindowTitle("µTimer");
	setWindowFlags(windowFlags() &(~Qt::WindowMaximizeButtonHint));
}

void MainWin::setupCentralWidget(Settings& settings, TimeTracker& timetracker)
{
	content_widget_ = new ContentWidget(settings, timetracker, this);

	setCentralWidget(content_widget_);

	QObject::connect(content_widget_, SIGNAL(pressedButton(Button)), this, SIGNAL(sendButtons(Button)));
	QObject::connect(content_widget_, SIGNAL(minToTray()), this, SLOT(minToTray()));
	QObject::connect(content_widget_, SIGNAL(toggleAlwaysOnTop()), this, SLOT(toggleAlwaysOnTop()));
}

void MainWin::setupIcon()
{
	const QIcon icon(":/clock.png");

	setWindowIcon(icon);

	tray_icon_ = new QSystemTrayIcon(icon, this);
	tray_icon_->setToolTip("Timing Inactive");
	tray_icon_->show();

	QObject::connect(tray_icon_, SIGNAL(activated(QSystemTrayIcon::ActivationReason)), this, SLOT(iconActivated(QSystemTrayIcon::ActivationReason)));
}

void MainWin::update()
{
	content_widget_->updateTimes();
	tray_icon_->setToolTip(content_widget_->getTooltip());

	if ((content_widget_->isGUIinActivity()) && (settings_.showTooMuchActivityWarning() || settings_.showTooMuchActivityWarning()))
		showActivityWarnings();
}

void MainWin::showActivityWarnings()
{
	const qint64 t_active = timetracker_.getActiveTime();
	const qint64 t_pause = timetracker_.getPauseTime();

	if ((!warning_activity_shown_)
			&& (t_active > settings_.getWarnTimeActivityMsec())) {
		warning_activity_shown_ = true;
		showMsgBox("Total activity time: " + convMSecToTimeStr(t_active));
	}

	if ((!warning_pause_shown_)
			&& (t_active > settings_.getWarnTimeNoPauseMsec())
			&& (t_pause < settings_.getPauseTimeForWarnTimeNoPauseMsec())) {
		warning_pause_shown_ = true;
		showMsgBox("Pause time: " + convMSecToTimeStr(t_pause) + "\nwith activity time: " + convMSecToTimeStr(t_active));
	}
}

void MainWin::showMsgBox(const QString &text)
{
	showMainWin();

	QMessageBox msgBox(this);
	msgBox.setWindowTitle("µTimer Warning");
	msgBox.setText(text);
	msgBox.setIcon(QMessageBox::Warning);
	msgBox.exec();
}

void MainWin::reactOnLockState(LockEvent event)
{
	if (settings_.isAutopauseEnabled()) {
		if (event == LockEvent::LongOngoingLock) {
			if (content_widget_->isGUIinActivity()) {
				was_active_before_autopause_ = true;
				content_widget_->setGUItoPause();
			}
			else {
				was_active_before_autopause_ = false;
			}
		}
		else if (event == LockEvent::Unlock) {
			if (was_active_before_autopause_)
				content_widget_->setGUItoActivity();
			was_active_before_autopause_ = false;
		}
	}
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

void MainWin::showMainWin()
{
	activateWindow();
	show();
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

void MainWin::toggleAlwaysOnTopFlag()
{
	setWindowFlags(windowFlags() ^ (Qt::CustomizeWindowHint | Qt::WindowStaysOnTopHint));
}

void MainWin::start()
{
	if (settings_.isPinnedStartEnabled())
		toggleAlwaysOnTopFlag();

	if (settings_.isMinimizedStartEnabled())
		minToTray();
	else
		showMainWin();

	if (settings_.isAutostartTimingEnabled())
		content_widget_->pressedStartPauseButton();

	warning_activity_shown_ = !settings_.showTooMuchActivityWarning();
	warning_pause_shown_ = !settings_.showNoPauseWarning();
}

void MainWin::shutdown()
{
	static int already_called = 0;

	if (settings_.logToFile())
		Logger::Log("[TIMER] Shutdown requested (" + QString::number(already_called) + ")");

	if (content_widget_->isGUIinActivity() || content_widget_->isGUIinPause()) {
		content_widget_->pressedStopButton();
	}

	// Allow some time for the timer to fully stop and database operations to complete
	auto dieTime = QTime::currentTime().addMSecs(150);
	while (QTime::currentTime() < dieTime)
		QCoreApplication::processEvents(QEventLoop::AllEvents, 30);

	// try again if nice shutdown via signals did not work
	if (content_widget_->isGUIinActivity() || content_widget_->isGUIinPause()) {
		timetracker_.useTimerViaButton(Button::Stop);
		content_widget_->setGUItoStop();

		dieTime = QTime::currentTime().addMSecs(70);
		while (QTime::currentTime() < dieTime)
			QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
	}

	// Verify timer stopped correctly
	if (settings_.logToFile()) {
		if (content_widget_->isGUIinActivity() || content_widget_->isGUIinPause()) {
			Logger::Log("[TIMER] Error: Timer did not stop correctly during shutdown");
		} else {
			Logger::Log("[TIMER] Shutdown completed successfully");
		}
	}

	++already_called;
}

void MainWin::onAboutToQuit()
{
	if (settings_.logToFile()) {
		Logger::Log("[TIMER] AboutToQuit received");
	}

	shutdown();
}

void MainWin::closeEvent(QCloseEvent *event)
{
	if (settings_.logToFile()) {
		Logger::Log("[TIMER] CloseEvent received");
	}

	// Handle manual window closing (Alt+F4, X button, etc.)
	shutdown();
	event->accept();
}

bool MainWin::nativeEvent([[maybe_unused]]const QByteArray& eventType, void* message, long* result)
{
#ifdef Q_OS_WIN
	MSG* msg = static_cast<MSG*>(message);
	if (msg->message == WM_QUERYENDSESSION)
	{
		if (settings_.logToFile())
			Logger::Log("[TIMER] WM_QUERYENDSESSION requested");

		// Windows is asking if we can shutdown - allow it
		*result = TRUE;
		return true;
	}
	else if (msg->message == WM_ENDSESSION)
	{
		if (settings_.logToFile()) {
			Logger::Log("[TIMER] WM_ENDSESSION received");
		}

		// Windows is shutting down - stop the timer if it's running
		shutdown();

		*result = 0;
		return true;
	}
#endif
	return QMainWindow::nativeEvent(eventType, message, result);
}