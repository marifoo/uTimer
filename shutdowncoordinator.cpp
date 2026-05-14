#include "shutdowncoordinator.h"
#include "timetracker.h"
#include "sessionstore.h"
#include "types.h"
#include "logger.h"
#include <QDateTime>
#include <QTime>
#include <QCoreApplication>

ShutdownCoordinator::ShutdownCoordinator(TimeTracker& timetracker, SessionStore& db)
    : timetracker_(timetracker), db_(db), shutdown_completed_(false)
{
}

void ShutdownCoordinator::run(bool forceDirectPath)
{
    if (shutdown_completed_) {
        Logger::Log("[TIMER] Shutdown already completed, skipping");
        return;
    }

    Logger::Log(QString("[TIMER] Shutdown requested (force_direct=%1)").arg(forceDirectPath));

    bool timer_was_running = timetracker_.getOngoingDuration().has_value();

    if (timer_was_running) {
        if (forceDirectPath) {
            // During Windows shutdown, use direct call - event loop may not work
            timetracker_.useTimerViaButton(Button::Stop);
        } else {
            // Normal shutdown - stop via TimeTracker directly then pump the event loop
            // so any connected signals (GUI sync, checkpoint flush) can fire
            timetracker_.useTimerViaButton(Button::Stop);

            auto dieTime = QTime::currentTime().addMSecs(150);
            while (QTime::currentTime() < dieTime)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 30);

            // Fallback pump if the timer is somehow still running
            if (timetracker_.getOngoingDuration().has_value()) {
                timetracker_.useTimerViaButton(Button::Stop);

                dieTime = QTime::currentTime().addMSecs(70);
                while (QTime::currentTime() < dieTime)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            }
        }
    }

    Logger::Log("[DB] Flushing database to disk before shutdown");
    db_.flushToDisc();

    if (timetracker_.canMarkCleanShutdown()) {
        db_.setLastCleanShutdownMarker(QDateTime::currentDateTime());
    }

    if (timetracker_.getOngoingDuration().has_value()) {
        Logger::Log("[TIMER] Error: Timer did not stop correctly during shutdown");
    } else {
        Logger::Log("[TIMER] Shutdown completed successfully");
    }

    shutdown_completed_ = true;
}
