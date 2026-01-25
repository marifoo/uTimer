#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QMessageBox>
#include <QSettings>
#include <QStyleFactory>
#include <QTimer>
#ifdef Q_OS_LINUX
#include <QLoggingCategory>
#include <QSocketNotifier>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>
#endif

#include "settings.h"
#include "mainwin.h"
#include "timetracker.h"
#include "lockstatewatcher.h"
#include "logger.h"
#include "types.h"

#ifdef Q_OS_LINUX
// Unix signal handling - safe pattern using socketpair
// Signal handlers can only call async-signal-safe functions,
// so we write to a socket and let Qt handle it in the event loop

static int signalFd[2] = {-1, -1};

static void unixSignalHandler(int signum)
{
    // Write the signal number to the socket (async-signal-safe)
    char sig = static_cast<char>(signum);
    if (write(signalFd[0], &sig, sizeof(sig)) == -1) {
        // Can't do much here - we're in a signal handler
    }
}

static bool setupUnixSignalHandlers()
{
    // Create socket pair for signal communication
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, signalFd) != 0) {
        qWarning() << "Failed to create socketpair for signal handling";
        return false;
    }

    // Install signal handlers
    struct sigaction sa;
    sa.sa_handler = unixSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;  // Restart interrupted system calls

    if (sigaction(SIGTERM, &sa, nullptr) != 0) {
        qWarning() << "Failed to install SIGTERM handler";
        return false;
    }
    if (sigaction(SIGINT, &sa, nullptr) != 0) {
        qWarning() << "Failed to install SIGINT handler";
        return false;
    }
    if (sigaction(SIGHUP, &sa, nullptr) != 0) {
        qWarning() << "Failed to install SIGHUP handler";
        return false;
    }

    return true;
}
#endif

/**
 * Application Entry Point.
 *
 * Startup Sequence:
 * 1. Initialize Signal Handlers (Linux-specific for graceful shutdown).
 * 2. Setup Qt Application and Style.
 * 3. Instantiate Core Components:
 *    - Settings (Configuration)
 *    - LockStateWatcher (Hardware abstraction)
 *    - TimeTracker (Business Logic)
 *    - MainWin (Presentation)
 * 4. Wire up Signals/Slots to connect the components.
 * 5. Start the main event loop.
 */
int main(int argc, char *argv[])
{
#ifdef Q_OS_LINUX
	// Suppress KDE Framework warnings about missing platform plugin
	QLoggingCategory::setFilterRules("kf.windowsystem=false");

	// Set up Unix signal handlers before creating QApplication
	setupUnixSignalHandlers();
#endif

	QCoreApplication::setApplicationName("µTimer");

	QApplication application(argc, argv);
	application.setStyle(QStyleFactory::create("Fusion"));

	QTimer timer;

	Settings settings(QDir(QCoreApplication::applicationDirPath()).filePath("user-settings.ini"));
	LockStateWatcher lockstate_watcher(settings);
	TimeTracker time_tracker(settings);

	// Check database schema before starting the UI
	if (!time_tracker.checkDatabaseSchema()) {
		QMessageBox::critical(nullptr, "Database Error",
			"The database schema is outdated and incompatible with this version.\n\n"
			"Please delete or rename the following file and restart:\n"
			+ QDir(QCoreApplication::applicationDirPath()).filePath("uTimer.sqlite") +
			"\n\nNote: This will delete your history data.");
		return 1;
	}

	MainWin main_win(settings, time_tracker);

#ifdef Q_OS_LINUX
	// Set up socket notifier to handle Unix signals in the Qt event loop
	QSocketNotifier* signalNotifier = nullptr;
	if (signalFd[1] != -1) {
		signalNotifier = new QSocketNotifier(signalFd[1], QSocketNotifier::Read, &application);
		QObject::connect(signalNotifier, &QSocketNotifier::activated, [&](int) {
			// Read and discard the signal number
			char sig;
			if (read(signalFd[1], &sig, sizeof(sig)) > 0) {
				if (settings.logToFile()) {
					QString signalName;
					switch (sig) {
						case SIGTERM: signalName = "SIGTERM"; break;
						case SIGINT:  signalName = "SIGINT"; break;
						case SIGHUP:  signalName = "SIGHUP"; break;
						default:      signalName = QString::number(sig); break;
					}
					Logger::Log("[SIGNAL] Received " + signalName + " - initiating shutdown");
				}

				// Trigger graceful shutdown
				main_win.onAboutToQuit();
				QCoreApplication::quit();
			}
		});
	}
#endif

	QObject::connect(&main_win, SIGNAL(sendButtons(Button)), &time_tracker, SLOT(useTimerViaButton(Button)));

	QObject::connect(&timer, SIGNAL(timeout()), &main_win, SLOT(update()));

	QObject::connect(&timer, SIGNAL(timeout()), &lockstate_watcher, SLOT(update()));
	QObject::connect(&lockstate_watcher, SIGNAL(desktopLockEvent(LockEvent)),	&time_tracker, SLOT(useTimerViaLockEvent(LockEvent)));
	QObject::connect(&lockstate_watcher, SIGNAL(desktopLockEvent(LockEvent)), &main_win, SLOT(reactOnLockState(LockEvent)));

	QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &main_win, &MainWin::onAboutToQuit);

	timer.setInterval(100);
	timer.start();

	main_win.start();

	return application.exec();
}
