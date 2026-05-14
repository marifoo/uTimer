#ifndef SHUTDOWNCOORDINATOR_H
#define SHUTDOWNCOORDINATOR_H

class TimeTracker;
class SessionStore;

class ShutdownCoordinator
{
public:
    ShutdownCoordinator(TimeTracker& timetracker, SessionStore& db);

    void run(bool forceDirectPath = false);

private:
    TimeTracker& timetracker_;
    SessionStore& db_;
    bool shutdown_completed_;
};

#endif // SHUTDOWNCOORDINATOR_H
