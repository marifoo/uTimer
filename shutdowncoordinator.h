#ifndef SHUTDOWNCOORDINATOR_H
#define SHUTDOWNCOORDINATOR_H

class TimeTracker;
class IDatabaseManager;

class ShutdownCoordinator
{
public:
    ShutdownCoordinator(TimeTracker& timetracker, IDatabaseManager& db);

    void run(bool forceDirectPath = false);

private:
    TimeTracker& timetracker_;
    IDatabaseManager& db_;
    bool shutdown_completed_;
};

#endif // SHUTDOWNCOORDINATOR_H
