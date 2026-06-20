#include "shutdowncoordinator.h"
#include "timer.h"
#include "sessionstore.h"
#include "logger.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QCoreApplication>

ShutdownCoordinator::ShutdownCoordinator(Timer& timetracker, SessionStore& db)
    : timetracker_(timetracker), db_(db), shutdown_completed_(false)
{
}

void ShutdownCoordinator::pumpEvents(int budgetMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
}

void ShutdownCoordinator::run(bool forceDirectPath)
{
    if (shutdown_completed_) {
        Logger::Log("[TIMER] Shutdown already completed, skipping");
        return;
    }

    Logger::Log(QString("[TIMER] Shutdown requested (force_direct=%1)").arg(forceDirectPath));

    auto result = timetracker_.shutdown();

    if (!forceDirectPath) {
        // Normal shutdown: pump the event loop so connected signals
        // (GUI sync, checkpoint flush) can fire before we proceed.
        pumpEvents(150);
    }

    Logger::Log("[DB] Flushing database to disk before shutdown");
    db_.flushToDisc();

    if (result.canCleanMark && !result.stopCalled) {
        db_.setLastCleanShutdownMarker(QDateTime::currentDateTime());
    }

    if (!result.stopped) {
        Logger::Log("[TIMER] Error: Timer did not stop correctly during shutdown");
    } else {
        Logger::Log("[TIMER] Shutdown completed successfully");
    }

    shutdown_completed_ = true;
}
