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

void ShutdownCoordinator::run(ShutdownMode mode)
{
    if (shutdown_completed_) {
        Logger::Log("[TIMER] Shutdown already completed, skipping");
        return;
    }

    Logger::Log(QString("[TIMER] Shutdown requested (mode=%1)")
                .arg(mode == ShutdownMode::Direct ? "Direct" : "DrainEvents"));

    auto result = timetracker_.shutdown();

    if (mode == ShutdownMode::DrainEvents) {
        // Normal shutdown: pump the event loop so connected signals
        // (GUI sync, checkpoint flush) can fire before we proceed.
        pumpEvents(150);
    }

    Logger::Log("[DB] Flushing database to disk before shutdown");
    const SessionStoreResult flushResult = db_.flushToDisc();
    if (!flushResult.ok() && flushResult.category != SessionStoreResult::Disabled) {
        Logger::Log("[DB] Warning: flushToDisc failed during shutdown: " + flushResult.message);
    }

    if (result.canCleanMark && !result.stopCalled) {
        const SessionStoreResult markerResult = db_.setLastCleanShutdownMarker(QDateTime::currentDateTime());
        if (!markerResult.ok() && markerResult.category != SessionStoreResult::Disabled) {
            Logger::Log("[DB] Warning: setLastCleanShutdownMarker failed during shutdown: " + markerResult.message);
        }
    }

    if (!result.stopped) {
        Logger::Log("[TIMER] Error: Timer did not stop correctly during shutdown");
    } else {
        Logger::Log("[TIMER] Shutdown completed successfully");
    }

    shutdown_completed_ = true;
}
