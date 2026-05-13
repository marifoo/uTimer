/**
 * MainWin — Application entry point and orchestrator.
 *
 * Responsibilities:
 * - Window management (System Tray, Always-on-Top, minimize behaviors)
 * - Forced day-boundary stop:
 *     A single-shot QTimer fires at 23:59:59.500 each day a session
 *     is running and forces the timer off via the standard Stop
 *     pipeline. There is NO automatic restart on the new day; the
 *     user must start the next session manually.
 * - Cross-midnight watchdog:
 *     update() polls TimeTracker every 100 ms. If the ongoing
 *     segment is observed to cross local midnight (because the
 *     scheduled stop did not fire — e.g., sleep/hibernate, modal
 *     dialog), the watchdog triggers the same Stop pipeline. The
 *     in-flight segment is discarded; that span of time is lost.
 * - Global event handling (Shutdown, Sleep/Wake)
 * - User warning system (Health checks for too much work / no pause)
 */

#include "mainwin.h"
#include <QtDebug>
#include <QTime>
#include <QTimer>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QStatusBar>
#include "logger.h"
#include "helpers.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif


MainWin::MainWin(Settings& settings, TimeTracker& timetracker, QWidget *parent)	
	: QMainWindow(parent), settings_(settings), timetracker_(timetracker), 
	  warning_pause_shown_(false), was_active_before_autopause_(false)
{
	setupCentralWidget(settings, timetracker);

	setupIcon();

	setWindowTitle("µTimer");
	setWindowFlags(windowFlags() &(~Qt::WindowMaximizeButtonHint));
	
	midnight_timer_ = new QTimer(this);
	midnight_timer_->setSingleShot(true);
	// Connection is made dynamically in scheduleMidnightStop.
}

void MainWin::setupCentralWidget(Settings& settings, TimeTracker& timetracker)
{
	content_widget_ = new ContentWidget(settings, timetracker, this);

	setCentralWidget(content_widget_);
	QObject::connect(content_widget_, SIGNAL(pressedButton(Button)), this, SIGNAL(sendButtons(Button)));
	QObject::connect(content_widget_, &ContentWidget::historyLoadReconciliationAvailable,
		this, &MainWin::showHistoryLoadReconciliation);
	QObject::connect(content_widget_, SIGNAL(minToTray()), this, SLOT(minToTray()));
	QObject::connect(content_widget_, SIGNAL(toggleAlwaysOnTop()), this, SLOT(toggleAlwaysOnTop()));
	
	QObject::connect(content_widget_, &ContentWidget::pressedButton, this, [this](Button button) {
		if (button == Button::Start) {
			scheduleMidnightStop();
		} else if (button == Button::Stop) {
			midnight_timer_->stop();
			// Any stop (manual, scheduled, or watchdog-driven) must cancel
			// the pending unlock-restart path. Without this, an unlock that
			// arrives after Stop while was_active_before_autopause_ is still
			// true would flip the GUI back to Activity.
			was_active_before_autopause_ = false;
			Logger::Log("[MIDNIGHT] Timer stopped - cancelled midnight timer");
		}
	});
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
	// Primary check: ongoing segment crosses midnight — drive Stop pipeline.
	if (timetracker_.isOngoingSegmentCrossMidnight()) {
		Logger::Log("[MIDNIGHT] Watchdog: cross-midnight ongoing segment - forcing stop");
		content_widget_->pressedStopButton();
	}
	// Secondary check: TimeTracker is already in None (e.g., because the
	// checkpoint guard fired first), but the GUI hasn't caught up.
	else if ((content_widget_->isGUIinActivity() || content_widget_->isGUIinPause())
	         && !timetracker_.getOngoingDuration().has_value()) {
		Logger::Log("[MIDNIGHT] Watchdog: TimeTracker is stopped but GUI lagged - syncing GUI");
		content_widget_->setGUItoStop();
		midnight_timer_->stop();
		was_active_before_autopause_ = false;
	}

	content_widget_->updateTimes();
	tray_icon_->setToolTip(content_widget_->getTooltip());

	if ((content_widget_->isGUIinActivity())
	    && (settings_.showTooMuchActivityWarning() || settings_.showNoPauseWarning()))
		showActivityWarnings();
}

/**
 * Health Check: Evaluates current user activity against health thresholds.
 *
 * Checks two conditions:
 * 1. Total daily activity exceeds limit (e.g., 9h 45m).
 * 2. Activity without sufficient pause (e.g., 6h worked with < 30m break).
 *
 * Shows a warning dialog if thresholds are crossed for the first time this session.
 */
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

void MainWin::showUserWarning(const QString& text)
{
	if (tray_icon_ && tray_icon_->isVisible()) {
		tray_icon_->showMessage("uTimer Warning", text, QSystemTrayIcon::Warning, 10000);
	}

	statusBar()->showMessage(text, 10000);
}

void MainWin::showHistoryLoadReconciliation(const QString& text)
{
	statusBar()->showMessage(text, 10000);
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
#ifdef Q_OS_LINUX
	// On Linux (especially KDE/Wayland), DoubleClick doesn't work - use single-click
	if (reason != QSystemTrayIcon::Trigger)
		return;
#else
	if (reason != QSystemTrayIcon::DoubleClick)
		return;
#endif

	if (isVisible())
		minToTray();
	else
		showMainWin();
}

void MainWin::showMainWin()
{
#ifdef Q_OS_LINUX
	setVisible(true);
	setWindowState(windowState() & ~Qt::WindowMinimized);
#else
	activateWindow();
	show();
#endif
}

void MainWin::minToTray()
{
#ifdef Q_OS_LINUX
	setVisible(false);
#else
	hide();
#endif
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

	if (settings_.isAutostartTimingEnabled()) {
		content_widget_->pressedStartPauseButton();
		// The pressedButton signal will trigger midnight scheduling
	}

	const qint64 recoveredSeconds = timetracker_.getStartupRecoveredSeconds();
	if (recoveredSeconds > 0 && timetracker_.shouldShowStartupRecoveryNotification()) {
		const QString message = QString("Recovered %1 seconds from the last session after an unclean shutdown.")
			.arg(recoveredSeconds);
		statusBar()->showMessage(message, 10000);
		if (tray_icon_ && tray_icon_->isVisible()) {
			tray_icon_->showMessage("uTimer Recovery", message, QSystemTrayIcon::Information, 10000);
		}
	}

	warning_activity_shown_ = !settings_.showTooMuchActivityWarning();
	warning_pause_shown_ = !settings_.showNoPauseWarning();
}

/**
 * Handles application shutdown sequence.
 *
 * Critical for data integrity:
 * 1. Stops the timer (forcing a DB save of the current session)
 * 2. Waits for events to process (ensures DB writes complete)
 * 3. Handles both graceful (Quit) and forced (System Shutdown) paths
 *
 * @param force_direct If true, bypasses the event loop (needed for Windows WM_ENDSESSION)
 */
void MainWin::shutdown(bool force_direct)
{
	// Guard against multiple shutdown calls
	static bool shutdown_completed = false;
	if (shutdown_completed) {
		Logger::Log("[TIMER] Shutdown already completed, skipping");
		return;
	}

	Logger::Log(QString("[TIMER] Shutdown requested (force_direct=%1)").arg(force_direct));

	bool timer_was_running = content_widget_->isGUIinActivity() || content_widget_->isGUIinPause();

	if (timer_was_running) {
		if (force_direct) {
			// During Windows shutdown, use direct call - event loop may not work
			timetracker_.useTimerViaButton(Button::Stop);
			content_widget_->setGUItoStop();
		} else {
			// Normal shutdown - try via GUI button first (triggers signals)
			content_widget_->pressedStopButton();

			// Allow some time for the timer to fully stop and database operations to complete
			auto dieTime = QTime::currentTime().addMSecs(150);
			while (QTime::currentTime() < dieTime)
				QCoreApplication::processEvents(QEventLoop::AllEvents, 30);

			// Fallback: try direct call if signals didn't work
			if (content_widget_->isGUIinActivity() || content_widget_->isGUIinPause()) {
				timetracker_.useTimerViaButton(Button::Stop);
				content_widget_->setGUItoStop();

				dieTime = QTime::currentTime().addMSecs(70);
				while (QTime::currentTime() < dieTime)
					QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
			}
		}
	}

	// Verify timer stopped correctly
	// Flush database to disk to reduce risk of loss during Windows shutdown
	Logger::Log("[DB] Flushing database to disk before shutdown");
	timetracker_.flushDatabaseToDisc();

	if (timetracker_.canMarkCleanShutdown()) {
		timetracker_.markCleanShutdown();
	}

	if (content_widget_->isGUIinActivity() || content_widget_->isGUIinPause()) {
		Logger::Log("[TIMER] Error: Timer did not stop correctly during shutdown");
	} else {
		Logger::Log("[TIMER] Shutdown completed successfully");
	}

	shutdown_completed = true;
}

void MainWin::onAboutToQuit()
{
	Logger::Log("[TIMER] AboutToQuit received");
	shutdown(false);  // Normal shutdown, use event loop
}

void MainWin::closeEvent(QCloseEvent *event)
{
	Logger::Log("[TIMER] CloseEvent received");
	// Handle manual window closing (Alt+F4, X button, etc.)
	shutdown(false);  // Normal shutdown, use event loop
	event->accept();
}

bool MainWin::nativeEvent([[maybe_unused]]const QByteArray& eventType, void* message, long* result)
{
#ifdef Q_OS_WIN
	MSG* msg = static_cast<MSG*>(message);
	if (msg->message == WM_QUERYENDSESSION)
	{
		Logger::Log("[TIMER] WM_QUERYENDSESSION received - starting early save");

		// Windows is asking if we can shutdown
		// Best practice: start saving NOW to have more time before WM_ENDSESSION
		// Use force_direct=true since event loop may be restricted during shutdown
		shutdown(true);

		// Return TRUE to allow shutdown
		*result = TRUE;
		return true;
	}
	else if (msg->message == WM_ENDSESSION)
	{
		// wParam indicates if session is actually ending (TRUE) or shutdown was cancelled (FALSE)
		bool session_ending = (msg->wParam != 0);

		Logger::Log(QString("[TIMER] WM_ENDSESSION received (session_ending=%1)").arg(session_ending));

		if (session_ending) {
			// Windows is actually shutting down - ensure timer is stopped
			// shutdown() guard will skip if already done in WM_QUERYENDSESSION
			shutdown(true);
		}
		// If session_ending is false, shutdown was cancelled - nothing to do
		// (data was already saved in WM_QUERYENDSESSION, which is fine)

		*result = 0;
		return true;
	}
#endif
	return QMainWindow::nativeEvent(eventType, message, result);
}

/**
 * Schedules a one-shot fire at 23:59:59.500 today (or 1 ms from now if
 * the current local time is already past that).
 *
 * On firing, onMidnightStop() runs and emits Button::Stop if the timer
 * is still active. The midnight_timer_ is NOT re-armed for the next
 * day — the user has to press Start again to track a new session.
 */
void MainWin::scheduleMidnightStop()
{
	// Calculate milliseconds until 23:59:59.500
	QTime current_time = QTime::currentTime();
	QTime midnight_stop_time(23, 59, 59, 500);
	
	qint64 msecs_until_stop;
	if (current_time < midnight_stop_time) {
		// Same day - calculate time until 23:59:59.500
		msecs_until_stop = current_time.msecsTo(midnight_stop_time);
	} else {
		// Already past the stop time today - stop immediately
		msecs_until_stop = 1;
	}
	
	Logger::Log(QString("[MIDNIGHT] Scheduled auto-stop in %1 seconds")
		.arg(msecs_until_stop / 1000.0, 0, 'f', 1));

	// Disconnect any previous connection and connect to stop handler
	midnight_timer_->disconnect();
	connect(midnight_timer_, &QTimer::timeout, this, &MainWin::onMidnightStop);
	midnight_timer_->start(static_cast<int>(msecs_until_stop));
}


void MainWin::onMidnightStop()
{
	const bool timer_is_running =
		content_widget_->isGUIinActivity() || content_widget_->isGUIinPause();
	if (!timer_is_running) {
		Logger::Log("[MIDNIGHT] Scheduled stop fired but timer is not running - ignoring");
		return;
	}

	Logger::Log("[MIDNIGHT] Scheduled stop: forcing timer off at end of day");

	// Emits Button::Stop which:
	//   - cancels midnight_timer_ (lambda in setupCentralWidget)
	//   - clears MainWin's was_active_before_autopause_
	//   - drives TimeTracker::useTimerViaButton(Stop)
	content_widget_->pressedStopButton();
}
