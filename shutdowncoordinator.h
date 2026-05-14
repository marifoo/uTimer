#ifndef SHUTDOWNCOORDINATOR_H
#define SHUTDOWNCOORDINATOR_H

#include "settings.h"

class TimeTracker;
class IDatabaseManager;

class ShutdownCoordinator
{
public:
    ShutdownCoordinator(TimeTracker& timetracker, IDatabaseManager& db, const Settings& settings);

    void run(bool forceDirectPath = false);

private:
    TimeTracker& timetracker_;
    IDatabaseManager& db_;
    const Settings& settings_;
    bool shutdown_completed_;
};

#endif // SHUTDOWNCOORDINATOR_H
